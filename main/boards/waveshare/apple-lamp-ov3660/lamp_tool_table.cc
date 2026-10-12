#include "lamp_tool_table.h"

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

#include "lamp_catalog.h"
#include "lamp_protocol.h"
#include "omni_lamp_tools.h"
#include "settings.h"

namespace lamp {
namespace {

constexpr const char* kNamespace = "lamp_tools";
constexpr const char* kTableKey = "table_v1";
constexpr const char* kTableProtocol = "lamp-tools/1";

struct JsonDeleter {
    void operator()(cJSON* value) const { cJSON_Delete(value); }
};
using Unique = std::unique_ptr<cJSON, JsonDeleter>;

Unique Parse(const std::string& text) {
    return Unique(cJSON_ParseWithOpts(text.c_str(), nullptr, true));
}
bool OnlyKeys(const cJSON* object, std::initializer_list<const char*> allowed) {
    for (const auto* item = object->child; item; item = item->next) {
        bool known = false;
        for (const char* key : allowed)
            known = known || item->string == std::string(key);
        if (!known)
            return false;
    }
    return true;
}
bool Bounded(const std::string& value, size_t maximum) { return value.size() <= maximum; }

struct Builtin {
    const char* name;
    const char* operation;
};
constexpr Builtin kBuiltins[] = {
    {"lamp_get_state", "get_state"},
    {"lamp_end_conversation", ""},
    {"lamp_rest", "rest"},
    {"lamp_set_mode", "execute_group"},
    {"lamp_stop", "cancel"},
    {"lamp_get_task", "get_task"},
    {"lamp_set_torque", "set_torque"},
    {"lamp_record_calibration", "record_calibration"},
    {"lamp_express", "execute_group"},
    {"lamp_prepare_expression", "execute_group"},
    {"lamp_run_sequence", "execute_group"},
};
// lamp_prepare_expression is runtime-internal and never advertised, so it is not here.
constexpr const char* kAdvertised[] = {
    "lamp_get_state",  "lamp_end_conversation",      "lamp_rest",     "lamp_set_mode",
    "lamp_stop",       "lamp_get_task",              "lamp_set_torque",
    "lamp_record_calibration",                        "lamp_express",  "lamp_run_sequence",
};
bool Advertised(const std::string& name) {
    for (const char* entry : kAdvertised)
        if (name == entry)
            return true;
    return false;
}
const Builtin* BuiltinFor(const std::string& name) {
    for (const auto& tool : kBuiltins)
        if (name == tool.name)
            return &tool;
    return nullptr;
}
cJSON* Object() {
    auto* schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "object");
    cJSON_AddBoolToObject(schema, "additionalProperties", false);
    cJSON_AddObjectToObject(schema, "properties");
    return schema;
}
void AddTool(cJSON* tools, const char* name, const char* description, cJSON* schema) {
    auto* tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "type", "function");
    auto* function = cJSON_AddObjectToObject(tool, "function");
    cJSON_AddStringToObject(function, "name", name);
    cJSON_AddStringToObject(function, "description", description);
    cJSON_AddItemToObject(function, "parameters", schema);
    cJSON_AddItemToArray(tools, tool);
}

// The compiled table. A stored table may rewrite descriptions and schemas, and may add
// names, but it cannot change which operations exist or which arguments they accept.
cJSON* DefaultTools() {
    auto catalog = Parse(kCatalog);
    auto* tools = cJSON_CreateArray();
    auto add = [&](const char* name, const char* description, cJSON* schema) {
        AddTool(tools, name, description, schema);
    };
    add("lamp_get_state",
        "查询机械臂缓存：available才表示五轴反馈新鲜；mode仅保存的动作参考，不能断言已经站起。",
        Object());
    add("lamp_end_conversation",
        "广义告别或停止聊天：简短告别播完退出监听，不坐下卸力。讨论告别不结束。", Object());
    add("lamp_rest",
        "用户说你去休息吧/休息一下/"
        "睡吧时必须调用这个完整动作，不能仅回答：必要时使能，坐下并确认，卸力，成功后告别并停止监听"
        "。用户说自己休息或讨论休息不调用。",
        Object());
    add("lamp_set_mode",
        "明确命令站起来/站好/坐下/"
        "开始工作时调用，不能仅口头答应。必要时自动使能，再切换姿态并等待真实完成。保持yaw；不用于"
        "无动作意图的聊天或后台表达。",
        cJSON_Parse(
            R"({"type":"object","additionalProperties":false,"properties":{"mode":{"type":"string","enum":["sit","stand","work"]}},"required":["mode"]})"));
    add("lamp_stop", "停止运动但保持支撑力矩，不自动补播。", Object());
    add("lamp_get_task", "只有需要实际完成结果时查询任务，避免反复调用。",
        cJSON_Parse(
            R"({"type":"object","additionalProperties":false,"properties":{"task_id":{"type":"string"}},"required":["task_id"]})"));
    add("lamp_set_torque",
        "仅用户明确要求使能或卸力时调用，true保持实际当前位置上力；false原地卸力。不用于普通对话表"
        "达。",
        cJSON_Parse(
            R"({"type":"object","additionalProperties":false,"properties":{"enabled":{"type":"boolean"}},"required":["enabled"]})"));
    add("lamp_record_calibration", "用户已手摆对准并明确说记录时保存当前模式的完整回读姿态。",
        cJSON_Parse(
            R"({"type":"object","additionalProperties":false,"properties":{"target_id":{"type":"string","enum":["user","book"]}},"required":["target_id"]})"));
    add("lamp_express",
        "纯动作传speak:false与action或skill_"
        "id，立即执行且成功不说话；默认伴随说话的表达：先调用预备，再自然回答；实际播放开始后本地释"
        "放动作。不等待运动完成，不用于"
        "单独站起/坐下/工作。开心/"
        "兴奋A02，好奇B02，同意A01，否定B01，低落A03，强调A16。无需目标或标定，必须已使能，不自动上"
        "力。每轮最多一次。",
        cJSON_Parse(
            R"({"type":"object","additionalProperties":false,"properties":{"skill_id":{"type":"string","enum":["A01","A02","A03","B01","B02","A16"]},"action":{"type":"string","enum":["点头","兴奋摆动","低头","摇头","好奇歪头","强调","nod","sway","lower_head","shake_head","tilt","emphasize"]},"speak":{"type":"boolean","description":"false只做动作不回复；true或省略伴随说话"},"repetitions":{"type":"integer","minimum":1,"maximum":5,"description":"仅A01点头；一次或两下保持默认2，明确三/四/五次才设置。不拆成多次调用。"},"end_mode":{"type":"string","enum":["return_to_reference","hold_at_end"]}},"oneOf":[{"required":["skill_id"]},{"required":["action"]}]})"));
    Unique schema(Object());
    auto* steps = cJSON_CreateObject();
    cJSON_AddStringToObject(steps, "type", "array");
    cJSON_AddNumberToObject(steps, "minItems", 1);
    cJSON_AddNumberToObject(steps, "maxItems", 16);
    auto* items = cJSON_AddObjectToObject(steps, "items");
    auto* choices = cJSON_AddArrayToObject(items, "oneOf");
    for (const char* collection : {"actions", "primitives"}) {
        auto* rows = cJSON_GetObjectItemCaseSensitive(catalog.get(), collection);
        for (auto* row = rows ? rows->child : nullptr; row; row = row->next) {
            const bool primitive = std::string(collection) == "primitives";
            const auto id = Text(row, primitive ? "id" : "action_id");
            if (id == "A10")
                continue;  // Hardware has no supported lighting output.
            auto* step = Object();
            auto* properties = cJSON_GetObjectItem(step, "properties");
            auto* action = cJSON_AddObjectToObject(properties, "action_id");
            cJSON_AddStringToObject(action, "const", primitive ? "execute_primitive" : id.c_str());
            auto* required = cJSON_AddArrayToObject(step, "required");
            cJSON_AddItemToArray(required, cJSON_CreateString("action_id"));
            if (primitive) {
                auto* skill = cJSON_AddObjectToObject(properties, "skill_id");
                cJSON_AddStringToObject(skill, "const", id.c_str());
                cJSON_AddItemToArray(required, cJSON_CreateString("skill_id"));
                cJSON_AddItemToObject(
                    properties, "options",
                    cJSON_Duplicate(cJSON_GetObjectItem(row, "inputSchema"), true));
                if (id != "A01" && id != "A02" && id != "A03" && id != "A16" && id != "B01" &&
                    id != "B02")
                    cJSON_AddItemToObject(
                        properties, "target_id",
                        cJSON_Parse(R"({"type":"string","enum":["user","book"]})"));
                if (cJSON_IsTrue(cJSON_GetObjectItem(row, "target_required")))
                    cJSON_AddItemToArray(required, cJSON_CreateString("target_id"));
            } else {
                auto* fields = cJSON_GetObjectItem(row, "fields");
                for (auto* f = fields ? fields->child : nullptr; f; f = f->next) {
                    cJSON_AddItemToObject(properties, f->string, cJSON_Duplicate(f, true));
                    if (std::string(f->string) != "size")
                        cJSON_AddItemToArray(required, cJSON_CreateString(f->string));
                }
            }
            cJSON_AddStringToObject(
                step, "description",
                primitive ? Text(row, "description").c_str() : id.c_str());
            cJSON_AddItemToArray(choices, step);
        }
    }
    cJSON_AddItemToObject(cJSON_GetObjectItem(schema.get(), "properties"), "steps", steps);
    cJSON_AddItemToObject(
        cJSON_GetObjectItem(schema.get(), "properties"), "enable_if_needed",
        cJSON_Parse(
            R"({"type":"boolean","description":"仅用户明确要求实际运动时true；自发表达/后台动作必须false，默认false。"})"));
    cJSON_AddItemToObject(schema.get(), "required", cJSON_Parse("[\"steps\"]"));
    add("lamp_run_sequence",
        "执行一个有限语义动作组。次数幅度用本地默认，点两下头只用一次A01。靠近用A13而非低头，缺标定"
        "不替代。set_mode保持yaw。受理后可正常回答，不声称已完成。若要边说边表达请用lamp_"
        "express，不在这里串行等待表达。",
        schema.release());
    return tools;
}

bool ValidToolName(const std::string& name) {
    if (name.size() < 6 || name.size() > 48 || !name.starts_with("lamp_"))
        return false;
    return std::all_of(name.begin() + 5, name.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}
std::string ValidateSchema(const cJSON* schema) {
    if (!cJSON_IsObject(schema))
        return "Tool schema must be an object";
    if (!OnlyKeys(schema, {"type", "properties", "additionalProperties", "required"}))
        return "Tool schema has unsupported keywords";
    if (Text(schema, "type") != "object")
        return "Tool schema type must be object";
    if (auto* additional = cJSON_GetObjectItemCaseSensitive(schema, "additionalProperties");
        additional && !cJSON_IsFalse(additional))
        return "Tool schema must not allow additional properties";
    auto* properties = cJSON_GetObjectItemCaseSensitive(schema, "properties");
    if (!cJSON_IsObject(properties) || cJSON_GetArraySize(properties) > 8)
        return "Tool schema needs at most eight bounded properties";
    for (const auto* property = properties->child; property; property = property->next) {
        if (!cJSON_IsObject(property) ||
            !OnlyKeys(property,
                      {"type", "description", "enum", "minimum", "maximum", "maxLength"}))
            return "Tool schema property is malformed";
        if (auto* description = cJSON_GetObjectItemCaseSensitive(property, "description");
            description &&
            (!cJSON_IsString(description) || !Bounded(Text(property, "description"), 120)))
            return "Tool schema property description must be bounded text";
        const auto type = Text(property, "type");
        auto* choices = cJSON_GetObjectItemCaseSensitive(property, "enum");
        auto* minimum = cJSON_GetObjectItemCaseSensitive(property, "minimum");
        auto* maximum = cJSON_GetObjectItemCaseSensitive(property, "maximum");
        auto* length = cJSON_GetObjectItemCaseSensitive(property, "maxLength");
        if (type == "integer" || type == "number") {
            if (choices || length || !cJSON_IsNumber(minimum) || !cJSON_IsNumber(maximum))
                return "Numeric property needs minimum and maximum only";
            if (minimum->valuedouble > maximum->valuedouble ||
                maximum->valuedouble - minimum->valuedouble > 10000)
                return "Numeric property range is invalid or too wide";
            if (type == "integer" && (minimum->valuedouble != minimum->valueint ||
                                      maximum->valuedouble != maximum->valueint))
                return "Integer property bounds must be whole numbers";
        } else if (type == "string") {
            if (choices) {
                if (length || !cJSON_IsArray(choices) || cJSON_GetArraySize(choices) < 1 ||
                    cJSON_GetArraySize(choices) > 16)
                    return "String property enum must list one to sixteen choices only";
                for (const auto* choice = choices->child; choice; choice = choice->next)
                    if (!cJSON_IsString(choice) || std::string(choice->valuestring).empty() ||
                        std::string(choice->valuestring).size() > 32)
                        return "String property enum choice is invalid";
            } else if (!cJSON_IsNumber(length) || length->valuedouble != length->valueint ||
                       length->valueint < 1 || length->valueint > 128) {
                return "String property needs an enum or maxLength between one and 128";
            }
        } else if (type != "boolean") {
            return "Tool schema property type is unsupported";
        }
    }
    if (auto* required = cJSON_GetObjectItemCaseSensitive(schema, "required"); required) {
        if (!cJSON_IsArray(required) || cJSON_GetArraySize(required) > 4)
            return "Tool schema required must list at most four properties";
        for (const auto* item = required->child; item; item = item->next)
            if (!cJSON_IsString(item) ||
                !cJSON_GetObjectItemCaseSensitive(properties, item->valuestring))
                return "Tool schema required must name declared properties";
    }
    return Bounded(Encode(schema), kMaxSchemaBytes) ? std::string() : "Tool schema is too large";
}
// A stored template is checked by the production semantic validator, so a table can never
// introduce an action or argument the firmware would reject from the model itself.
std::string ValidateTemplate(const std::string& operation, const cJSON* schema,
                             const cJSON* args) {
    if (!cJSON_IsObject(args))
        return "Tool args template must be an object";
    if (cJSON_GetObjectItemCaseSensitive(args, "context"))
        return "Tool args template must not carry context";
    auto* properties = cJSON_GetObjectItemCaseSensitive(schema, "properties");
    for (const auto* argument = args->child; argument; argument = argument->next)
        if (cJSON_GetObjectItemCaseSensitive(properties, argument->string))
            return "Tool args template must not fix a schema property";
    Unique command(cJSON_CreateObject());
    cJSON_AddStringToObject(command.get(), "protocol_version", kProtocol);
    cJSON_AddStringToObject(command.get(), "request_id", "tool-table-check");
    cJSON_AddStringToObject(command.get(), "operation", operation.c_str());
    cJSON_AddItemToObject(command.get(), "args", cJSON_Duplicate(args, true));
    auto checked = Command(Encode(command.get()));
    return checked ? std::string() : checked.error();
}

struct Addition {
    std::string name;
    std::string description;
    std::string operation;
    std::string args;
    Unique schema;
};
struct Table {
    bool custom = false;
    std::string json;
    std::string instructions;
    std::string tools;
    std::string error;
    std::map<std::string, std::string> descriptions;
    std::map<std::string, Unique> schemas;
    std::vector<Addition> additions;
    int total = 0;
    int added = 0;
};
std::mutex& Mutex() {
    static std::mutex mutex;
    return mutex;
}
Table& State() {
    static Table table;
    return table;
}
bool& Loaded() {
    static bool loaded = false;
    return loaded;
}

void RebuildLocked() {
    Table& table = State();
    Unique tools(DefaultTools());
    for (auto* tool = tools->child; tool; tool = tool->next) {
        auto* function = cJSON_GetObjectItemCaseSensitive(tool, "function");
        const auto name = Text(function, "name");
        if (auto found = table.descriptions.find(name); found != table.descriptions.end())
            cJSON_ReplaceItemInObjectCaseSensitive(function, "description",
                                                   cJSON_CreateString(found->second.c_str()));
        if (auto found = table.schemas.find(name);
            found != table.schemas.end() && found->second)
            cJSON_ReplaceItemInObjectCaseSensitive(function, "parameters",
                                                   cJSON_Duplicate(found->second.get(), true));
    }
    for (const auto& addition : table.additions)
        AddTool(tools.get(), addition.name.c_str(), addition.description.c_str(),
                cJSON_Duplicate(addition.schema.get(), true));
    table.total = cJSON_GetArraySize(tools.get());
    table.tools = Encode(tools.get());
}
void ApplyLocked(const std::string& table_json) {
    Table& table = State();
    table = Table();
    if (table_json.empty()) {
        RebuildLocked();
        return;
    }
    std::string error;
    if (!ToolTableValidate(table_json, error)) {
        table.error = error;
        RebuildLocked();
        return;
    }
    Unique root(Parse(table_json));
    table.custom = true;
    table.json = table_json;
    table.instructions = Text(root.get(), "instructions");
    auto* tools = cJSON_GetObjectItemCaseSensitive(root.get(), "tools");
    for (auto* entry = tools ? tools->child : nullptr; entry; entry = entry->next) {
        const auto name = Text(entry, "name");
        auto* description = cJSON_GetObjectItemCaseSensitive(entry, "description");
        auto* schema = cJSON_GetObjectItemCaseSensitive(entry, "schema");
        if (BuiltinFor(name)) {
            if (description)
                table.descriptions[name] = description->valuestring;
            if (schema)
                table.schemas[name] = Unique(cJSON_Duplicate(schema, true));
            continue;
        }
        Addition addition;
        addition.name = name;
        addition.description = description ? description->valuestring : "";
        addition.operation = Text(entry, "operation");
        if (auto* args = cJSON_GetObjectItemCaseSensitive(entry, "args"))
            addition.args = Encode(args);
        addition.schema = Unique(cJSON_Duplicate(schema, true));
        table.additions.push_back(std::move(addition));
    }
    table.added = static_cast<int>(table.additions.size());
    RebuildLocked();
}
}  // namespace

void ToolTableLoad() {
    std::lock_guard lock(Mutex());
    if (Loaded())
        return;
    Loaded() = true;
    Settings settings(kNamespace, false);
    ApplyLocked(settings.GetString(kTableKey));
}
void ToolTableReload() {
    std::lock_guard lock(Mutex());
    Loaded() = true;
    Settings settings(kNamespace, false);
    ApplyLocked(settings.GetString(kTableKey));
}
bool ToolTableValidate(const std::string& table_json, std::string& error) {
    error.clear();
    if (table_json.size() > kMaxTableBytes) {
        error = "Table exceeds 3000 bytes";
        return false;
    }
    Unique root(Parse(table_json));
    if (!cJSON_IsObject(root.get())) {
        error = "Table must be a JSON object";
        return false;
    }
    if (!OnlyKeys(root.get(), {"protocol_version", "instructions", "tools"})) {
        error = "Table has unsupported keys";
        return false;
    }
    if (Text(root.get(), "protocol_version") != kTableProtocol) {
        error = "protocol_version must be lamp-tools/1";
        return false;
    }
    if (auto* instructions = cJSON_GetObjectItemCaseSensitive(root.get(), "instructions")) {
        const auto text = Text(root.get(), "instructions");
        if (!cJSON_IsString(instructions) || text.empty() || text.size() > kMaxInstructionsBytes) {
            error = "instructions must be bounded non-empty text";
            return false;
        }
    }
    auto* tools = cJSON_GetObjectItemCaseSensitive(root.get(), "tools");
    if (tools && !cJSON_IsArray(tools)) {
        error = "tools must be an array";
        return false;
    }
    if (tools && cJSON_GetArraySize(tools) > kMaxTableTools) {
        error = "Table lists too many tools";
        return false;
    }
    std::set<std::string> names;
    int added = 0;
    for (auto* entry = tools ? tools->child : nullptr; entry; entry = entry->next) {
        if (!cJSON_IsObject(entry) ||
            !OnlyKeys(entry, {"name", "description", "operation", "schema", "args"})) {
            error = "Every tool needs name, description, schema and optional operation/args";
            return false;
        }
        const auto name = Text(entry, "name");
        if (!ValidToolName(name)) {
            error = "Tool name must be lamp_ followed by [a-z0-9_]";
            return false;
        }
        if (!names.insert(name).second) {
            error = "Tool names must be unique";
            return false;
        }
        const auto description = Text(entry, "description");
        auto* description_item = cJSON_GetObjectItemCaseSensitive(entry, "description");
        if (!cJSON_IsString(description_item) || description.empty() ||
            description.size() > kMaxDescriptionBytes) {
            error = "Tool description must be bounded non-empty text";
            return false;
        }
        auto* operation = cJSON_GetObjectItemCaseSensitive(entry, "operation");
        if (operation && !cJSON_IsString(operation)) {
            error = "Tool operation must be a string";
            return false;
        }
        const auto op = Text(entry, "operation");
        const Builtin* builtin = BuiltinFor(name);
        if (builtin) {
            if (!Advertised(name)) {
                error = "Only advertised tools can be overridden";
                return false;
            }
            if (operation && op != builtin->operation) {
                error = "A built-in tool cannot change its operation";
                return false;
            }
        } else if (!KnownOperation(op)) {
            error = "Tool operation must be one of the compiled operations";
            return false;
        } else if (++added > kMaxAddedTools) {
            error = "Table adds too many tools";
            return false;
        }
        auto* schema = cJSON_GetObjectItemCaseSensitive(entry, "schema");
        if (!schema) {
            error = "Every tool needs a schema";
            return false;
        }
        if (auto invalid = ValidateSchema(schema); !invalid.empty()) {
            error = std::move(invalid);
            return false;
        }
        auto* args = cJSON_GetObjectItemCaseSensitive(entry, "args");
        if (builtin) {
            if (args && cJSON_GetArraySize(args) != 0) {
                error = "A built-in tool cannot carry an args template";
                return false;
            }
            continue;
        }
        Unique empty;
        if (!args) {
            empty = Unique(cJSON_CreateObject());
            args = empty.get();
        }
        if (auto invalid = ValidateTemplate(op, schema, args); !invalid.empty()) {
            error = std::move(invalid);
            return false;
        }
    }
    return true;
}
bool ToolTableSave(const std::string& table_json, std::string& error) {
    ToolTableLoad();
    if (!table_json.empty() && !ToolTableValidate(table_json, error))
        return false;
    {
        Settings settings(kNamespace, true);
        settings.SetString(kTableKey, table_json);
    }
    {
        std::lock_guard lock(Mutex());
        ApplyLocked(table_json);
    }
    error.clear();
    return true;
}
void ToolTableReset() {
    std::lock_guard lock(Mutex());
    Loaded() = true;
    ApplyLocked("");
}
std::string ToolTableTools() {
    ToolTableLoad();
    std::lock_guard lock(Mutex());
    return State().tools;
}
std::string ToolTableInstructions() {
    ToolTableLoad();
    std::lock_guard lock(Mutex());
    return State().instructions;
}
std::string ToolTableJson() {
    ToolTableLoad();
    std::lock_guard lock(Mutex());
    return State().json;
}
ToolTableState ToolTableStatus() {
    ToolTableLoad();
    std::lock_guard lock(Mutex());
    ToolTableState status;
    status.tools = State().total;
    status.added = State().added;
    status.custom = State().custom;
    status.custom_instructions = !State().instructions.empty();
    status.error = State().error;
    return status;
}
std::string ToolTableOperation(const std::string& name) {
    if (const auto builtin = ToolOperation(name); !builtin.empty())
        return builtin;
    ToolTableLoad();
    std::lock_guard lock(Mutex());
    for (const auto& addition : State().additions)
        if (addition.name == name)
            return addition.operation;
    return "";
}
std::string ToolTableArgsTemplate(const std::string& name) {
    if (!ToolOperation(name).empty())
        return "";
    ToolTableLoad();
    std::lock_guard lock(Mutex());
    for (const auto& addition : State().additions)
        if (addition.name == name)
            return addition.args;
    return "";
}
}  // namespace lamp
