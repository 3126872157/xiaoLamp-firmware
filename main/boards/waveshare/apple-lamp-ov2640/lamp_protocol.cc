#include "lamp_protocol.h"
#include "lamp_catalog.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <vector>

namespace lamp {
namespace {
using Result = std::expected<void, std::string>;
Result Keys(const cJSON* object, std::initializer_list<const char*> allowed) {
    if (!cJSON_IsObject(object))
        return std::unexpected("Expected an object");
    std::set<std::string> seen;
    for (auto* item = object->child; item; item = item->next) {
        if (!item->string || !seen.insert(item->string).second)
            return std::unexpected("Duplicate JSON field");
        if (std::none_of(allowed.begin(), allowed.end(),
                         [&](const char* key) { return std::strcmp(item->string, key) == 0; }))
            return std::unexpected(std::string("Unsupported field: ") + item->string);
    }
    return {};
}
bool OneOf(const std::string& value, std::initializer_list<const char*> values) {
    return std::any_of(values.begin(), values.end(),
                       [&](const char* item) { return value == item; });
}
CJsonUniquePtr Parse(const std::string& input) {
    if (input.empty() || input.size() > kMaxCommandBytes)
        return nullptr;
    return CJsonUniquePtr(cJSON_ParseWithOpts(input.c_str(), nullptr, true));
}
const cJSON* FindSkill(const cJSON* catalog, const std::string& id) {
    auto* array = cJSON_GetObjectItemCaseSensitive(catalog, "primitives");
    for (auto* item = array ? array->child : nullptr; item; item = item->next)
        if (Text(item, "id") == id)
            return item;
    return nullptr;
}
Result Step(const cJSON* step, const cJSON* catalog, bool finite_group) {
    const auto action = Text(step, "action_id");
    Result keys;
    if (action == "set_mode") {
        keys = Keys(step, {"action_id", "mode"});
        if (!OneOf(Text(step, "mode"), {"sit", "stand", "work"}))
            return std::unexpected("Invalid mode");
    } else if (action == "restore_home" || action == "restore_initial") {
        keys = Keys(step, {"action_id"});
    } else if (action == "adjust_pose") {
        keys = Keys(step, {"action_id", "direction", "size"});
        if (!OneOf(Text(step, "direction"), {"up", "down", "yaw_positive", "yaw_negative"}))
            return std::unexpected("Invalid adjustment direction");
        if (cJSON_GetObjectItemCaseSensitive(step, "size") &&
            !OneOf(Text(step, "size"), {"small", "medium", "large"}))
            return std::unexpected("Invalid adjustment size");
    } else if (action == "gaze_calibrated") {
        keys = Keys(step, {"action_id", "target_id"});
        if (!OneOf(Text(step, "target_id"), {"user", "book"}))
            return std::unexpected("A calibrated user/book target is required");
    } else if (action == "wait") {
        keys = Keys(step, {"action_id", "duration_sec"});
        auto* duration = cJSON_GetObjectItemCaseSensitive(step, "duration_sec");
        if (!cJSON_IsNumber(duration) || !std::isfinite(duration->valuedouble) ||
            duration->valuedouble < 0 || duration->valuedouble > 10)
            return std::unexpected("Wait must be between 0 and 10 seconds");
    } else if (action == "execute_primitive") {
        keys = Keys(step, {"action_id", "skill_id", "target_id", "options"});
        auto* def = FindSkill(catalog, Text(step, "skill_id"));
        if (!def)
            return std::unexpected("Unknown or disabled primitive");
        auto* required = cJSON_GetObjectItemCaseSensitive(def, "target_required");
        if (cJSON_IsTrue(required) && !OneOf(Text(step, "target_id"), {"user", "book"}))
            return std::unexpected("This primitive requires a calibrated target");
        auto* options = cJSON_GetObjectItemCaseSensitive(step, "options");
        if (options) {
            if (!cJSON_IsObject(options))
                return std::unexpected("options must be an object");
            auto* schema = cJSON_GetObjectItemCaseSensitive(def, "inputSchema");
            auto* properties = cJSON_GetObjectItemCaseSensitive(schema, "properties");
            std::set<std::string> seen;
            for (auto* value = options->child; value; value = value->next) {
                if (!value->string || !seen.insert(value->string).second)
                    return std::unexpected("Duplicate option");
                auto* field = cJSON_GetObjectItemCaseSensitive(properties, value->string);
                if (!field)
                    return std::unexpected("Option is not exposed to the model");
                if (Text(field, "type") == "integer") {
                    auto* min = cJSON_GetObjectItemCaseSensitive(field, "minimum");
                    auto* max = cJSON_GetObjectItemCaseSensitive(field, "maximum");
                    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
                        std::floor(value->valuedouble) != value->valuedouble ||
                        value->valuedouble < min->valuedouble ||
                        value->valuedouble > max->valuedouble)
                        return std::unexpected("Integer option is out of range");
                } else {
                    bool valid = false;
                    auto* enums = cJSON_GetObjectItemCaseSensitive(field, "enum");
                    for (auto* item = enums ? enums->child : nullptr; item; item = item->next)
                        if (cJSON_IsString(value) && cJSON_IsString(item) &&
                            std::strcmp(value->valuestring, item->valuestring) == 0)
                            valid = true;
                    if (!valid)
                        return std::unexpected("Invalid semantic option");
                }
            }
            if (finite_group && Text(options, "hold_mode") == "holding")
                return std::unexpected(
                    "Use execute_action for persistent lighting, not a finite group");
        }
    } else
        return std::unexpected("Unsupported semantic action");
    if (!keys)
        return keys;
    if (cJSON_GetObjectItemCaseSensitive(step, "target_id") &&
        !OneOf(Text(step, "target_id"), {"user", "book"}))
        return std::unexpected("Invalid target");
    return {};
}
Result Context(const cJSON* args, const cJSON* steps, bool group) {
    auto* context = cJSON_GetObjectItemCaseSensitive(args, "context");
    if (!context)
        return {};
    if (auto result = Keys(context, {"source", "turn_id", "cue"}); !result)
        return result;
    const auto turn = Text(context, "turn_id");
    if (Text(context, "source") != "dialogue" || !ValidId(turn))
        return std::unexpected("Invalid dialogue context");
    const auto cue = Text(context, "cue");
    const std::pair<const char*, const char*> cues[] = {{"agreement", "A01"}, {"question", "B02"},
                                                        {"denial", "B01"},    {"excitement", "A02"},
                                                        {"regret", "A03"},    {"emphasis", "A16"}};
    const cJSON* step = group ? cJSON_GetArrayItem(steps, 0) : args;
    if (group && cJSON_GetArraySize(steps) != 1)
        return std::unexpected("Dialogue permits one expression per turn");
    bool match = false;
    for (const auto& [key, skill] : cues)
        if (cue == key && Text(step, "skill_id") == skill)
            match = true;
    if (!match || Text(step, "action_id") != "execute_primitive")
        return std::unexpected("Dialogue cue does not match expression");
    auto* options = cJSON_GetObjectItemCaseSensitive(step, "options");
    if (Text(options, "end_mode") == "hold_at_end")
        return std::unexpected("Dialogue expressions must return to reference");
    return {};
}
void Copy(cJSON* out, const cJSON* in, const char* key) {
    auto* value = cJSON_GetObjectItemCaseSensitive(in, key);
    if (value)
        cJSON_AddItemToObject(out, key, cJSON_Duplicate(value, true));
}
void StripDescriptions(cJSON* value) {
    if (cJSON_IsObject(value))
        cJSON_DeleteItemFromObjectCaseSensitive(value, "description");
    for (auto* item = value ? value->child : nullptr; item; item = item->next)
        StripDescriptions(item);
}
// One line per action: its id, the allowed value(s) of each field, and a short
// usage hint. Enums are inlined under the field name because the model needs the
// exact spellings, not the JSON-Schema shape they came from.
void CopyActionSummary(cJSON* item, const cJSON* spec) {
    Copy(item, spec, "action_id");
    auto* fields = cJSON_GetObjectItemCaseSensitive(spec, "fields");
    for (auto* field = cJSON_IsObject(fields) ? fields->child : nullptr; field;
         field = field->next) {
        if (!field->string)
            continue;
        auto* enums = cJSON_GetObjectItemCaseSensitive(field, "enum");
        if (cJSON_IsArray(enums)) {
            cJSON_AddItemToObject(item, field->string, cJSON_Duplicate(enums, true));
            continue;
        }
        auto* min = cJSON_GetObjectItemCaseSensitive(field, "minimum");
        auto* max = cJSON_GetObjectItemCaseSensitive(field, "maximum");
        if (cJSON_IsNumber(min) && cJSON_IsNumber(max)) {
            auto* range = cJSON_AddArrayToObject(item, field->string);
            cJSON_AddItemToArray(range, cJSON_CreateNumber(min->valuedouble));
            cJSON_AddItemToArray(range, cJSON_CreateNumber(max->valuedouble));
        } else {
            cJSON_AddStringToObject(item, field->string, Text(field, "type").c_str());
        }
    }
    Copy(item, spec, "use");
}
}  // namespace

std::string Encode(const cJSON* value) {
    CJsonStringUniquePtr encoded(cJSON_PrintUnformatted(value));
    return encoded ? std::string(encoded.get()) : std::string();
}
std::string Text(const cJSON* object, const char* key) {
    auto* value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) ? value->valuestring : "";
}
bool ValidId(const std::string& id) {
    return !id.empty() && id.size() <= 96 && !id.starts_with("__lamp:") &&
           std::any_of(id.begin(), id.end(), [](unsigned char c) { return c > 32; }) &&
           std::none_of(id.begin(), id.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
std::string UrlEncode(const std::string& value) {
    std::string out;
    constexpr char hex[] = "0123456789ABCDEF";
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
            out += static_cast<char>(c);
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}
std::expected<std::string, std::string> Command(const std::string& input) {
    auto root = Parse(input);
    if (!root)
        return std::unexpected("Invalid JSON or command exceeds 24 KiB");
    if (auto result = Keys(root.get(), {"protocol_version", "request_id", "operation", "args"});
        !result)
        return std::unexpected(result.error());
    if (Text(root.get(), "protocol_version") != kProtocol ||
        !ValidId(Text(root.get(), "request_id")))
        return std::unexpected("lamp-action/2 and a valid request_id are required");
    CJsonUniquePtr catalog(cJSON_Parse(kCatalog));
    if (!catalog)
        return std::unexpected("Action catalog unavailable");
    auto operation = Text(root.get(), "operation");
    auto* args = cJSON_GetObjectItemCaseSensitive(root.get(), "args");
    if (!args) {
        args = cJSON_CreateObject();
        cJSON_AddItemToObject(root.get(), "args", args);
    }
    if (!cJSON_IsObject(args))
        return std::unexpected("args must be an object");
    if (operation == "execute_group") {
        if (auto result = Keys(args, {"steps", "context"}); !result)
            return std::unexpected(result.error());
        auto* steps = cJSON_GetObjectItemCaseSensitive(args, "steps");
        if (!cJSON_IsArray(steps) || cJSON_GetArraySize(steps) < 1 ||
            cJSON_GetArraySize(steps) > 16)
            return std::unexpected("A group requires 1-16 steps");
        double waits = 0;
        for (auto* step = steps->child; step; step = step->next) {
            if (auto result = Step(step, catalog.get(), true); !result)
                return std::unexpected(result.error());
            if (Text(step, "action_id") == "wait")
                waits += cJSON_GetObjectItemCaseSensitive(step, "duration_sec")->valuedouble;
        }
        if (waits > 60)
            return std::unexpected("Group waits exceed 60 seconds");
        if (auto result = Context(args, steps, true); !result)
            return std::unexpected(result.error());
    } else if (operation == "execute") {
        CJsonUniquePtr step(cJSON_Duplicate(args, true));
        cJSON_DeleteItemFromObjectCaseSensitive(step.get(), "context");
        if (auto result = Step(step.get(), catalog.get(), false); !result)
            return std::unexpected(result.error());
        if (auto result = Context(args, args, false); !result)
            return std::unexpected(result.error());
    } else if (operation == "get_task" || operation == "cancel") {
        if (auto result = Keys(args, {"task_id"}); !result)
            return std::unexpected(result.error());
        if ((operation == "get_task" || cJSON_GetObjectItemCaseSensitive(args, "task_id")) &&
            !ValidId(Text(args, "task_id")))
            return std::unexpected("Invalid task_id");
    } else if (operation == "set_torque") {
        if (auto result = Keys(args, {"enabled"}); !result)
            return std::unexpected(result.error());
        if (!cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(args, "enabled")))
            return std::unexpected("enabled must be a boolean");
    } else if (operation == "record_calibration") {
        if (auto result = Keys(args, {"target_id"}); !result)
            return std::unexpected(result.error());
        if (!OneOf(Text(args, "target_id"), {"user", "book"}))
            return std::unexpected("Calibration target must be user/book");
    } else if (operation == "set_light") {
        if (auto result = Keys(args, {"brightness"}); !result)
            return std::unexpected(result.error());
        auto* brightness = cJSON_GetObjectItemCaseSensitive(args, "brightness");
        if (!cJSON_IsNumber(brightness) || !std::isfinite(brightness->valuedouble) ||
            brightness->valuedouble < 0 || brightness->valuedouble > 1)
            return std::unexpected("brightness must be a number between 0 and 1");
    } else if (OneOf(operation, {"discover", "get_state", "end_lighting", "rest"})) {
        if (auto result = Keys(args, {}); !result)
            return std::unexpected(result.error());
    } else
        return std::unexpected("Unsupported operation");
    return Encode(root.get());
}
std::expected<std::string, std::string> Sequence(const std::string& input) {
    auto root = Parse(input);
    if (!root)
        return std::unexpected("Invalid sequence JSON");
    if (auto result = Keys(root.get(), {"protocol_version", "request_id", "steps", "context"});
        !result)
        return std::unexpected(result.error());
    CJsonUniquePtr command(cJSON_CreateObject());
    Copy(command.get(), root.get(), "protocol_version");
    Copy(command.get(), root.get(), "request_id");
    cJSON_AddStringToObject(command.get(), "operation", "execute_group");
    auto* args = cJSON_AddObjectToObject(command.get(), "args");
    Copy(args, root.get(), "steps");
    Copy(args, root.get(), "context");
    return Command(Encode(command.get()));
}
std::string Query(const char* operation, const std::string& request_id,
                  const std::string& task_id) {
    CJsonUniquePtr root(cJSON_CreateObject());
    cJSON_AddStringToObject(root.get(), "protocol_version", kProtocol);
    cJSON_AddStringToObject(root.get(), "request_id", request_id.c_str());
    cJSON_AddStringToObject(root.get(), "operation", operation);
    auto* args = cJSON_AddObjectToObject(root.get(), "args");
    if (!task_id.empty())
        cJSON_AddStringToObject(args, "task_id", task_id.c_str());
    return Encode(root.get());
}
std::string Catalog(int cursor, const std::string& skill_id) {
    CJsonUniquePtr catalog(cJSON_Parse(kCatalog));
    CJsonUniquePtr out(cJSON_CreateObject());
    for (auto* key : {"protocol_version", "model_version", "execution", "mode_targets", "limits"})
        Copy(out.get(), catalog.get(), key);
    cJSON_AddStringToObject(out.get(), "local_parameters", "device_presets");
    cJSON_AddStringToObject(out.get(), "executor", "web_sim");
    auto* primitives = cJSON_AddArrayToObject(out.get(), "primitives");
    auto* source = cJSON_GetObjectItemCaseSensitive(catalog.get(), "primitives");
    const int count = cJSON_GetArraySize(source);
    // Page 0 is the capability summary: it carries the actions a step may use,
    // so every primitive page can stay small enough to stream one at a time.
    const bool summary = skill_id.empty() && cursor <= 0;
    const int index = cursor - 1;
    const cJSON* def = nullptr;
    if (!skill_id.empty())
        def = FindSkill(catalog.get(), skill_id);
    else if (!summary && index >= 0 && index < count)
        def = cJSON_GetArrayItem(source, index);
    if (def) {
        auto* item = cJSON_CreateObject();
        for (auto* key :
             {"id", "name", "modes", "target_required", "conversation_cue", "inputSchema"})
            Copy(item, def, key);
        StripDescriptions(item);
        cJSON_AddItemToArray(primitives, item);
    }
    cJSON_AddNumberToObject(out.get(), "total", count);
    int next = -1;
    if (skill_id.empty()) {
        if (summary)
            next = count ? 1 : 0;
        else if (index >= 0 && index + 1 < count)
            next = cursor + 1;
    }
    cJSON_AddNumberToObject(out.get(), "next_cursor", next);
    if (!skill_id.empty() && !def)
        cJSON_AddStringToObject(out.get(), "error", "Unknown primitive");
    if (summary) {
        // controls stay out of the payload: set_torque, rest and
        // record_calibration already have dedicated tools.
        auto* actions = cJSON_AddArrayToObject(out.get(), "actions");
        auto* specs = cJSON_GetObjectItemCaseSensitive(catalog.get(), "actions");
        for (auto* spec = specs ? specs->child : nullptr; spec; spec = spec->next) {
            auto* item = cJSON_CreateObject();
            CopyActionSummary(item, spec);
            cJSON_AddItemToArray(actions, item);
        }
    }
    return Encode(out.get());
}
std::string Summarize(const cJSON* response) {
    CJsonUniquePtr out(cJSON_CreateObject());
    for (auto* key : {"protocol_version", "request_id", "ok", "error"})
        Copy(out.get(), response, key);
    auto* result = cJSON_GetObjectItemCaseSensitive(response, "result");
    if (cJSON_IsObject(result)) {
        auto* brief = cJSON_AddObjectToObject(out.get(), "result");
        for (auto* key :
             {"task_id", "status", "step_index", "total_steps", "reason", "completion_basis",
              "mode", "busy", "holding", "calibrated_targets", "player_status", "torque_enabled",
              "teaching", "stored_at", "persistent", "target_id", "rest_task"})
            Copy(brief, result, key);
        auto* active = cJSON_GetObjectItemCaseSensitive(result, "active_task");
        if (cJSON_IsObject(active)) {
            auto* task = cJSON_AddObjectToObject(brief, "active_task");
            for (auto* key : {"task_id", "status", "step_index", "total_steps", "completion_basis"})
                Copy(task, active, key);
        }
    }
    return Encode(out.get());
}
}  // namespace lamp
