#include "lamp_delivery_model.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#include "lamp_pose_model.h"

namespace lamp::delivery {
namespace {
const cJSON* Get(const cJSON* o, const char* k) { return cJSON_GetObjectItemCaseSensitive(o, k); }
bool Keys(const cJSON* o, std::initializer_list<const char*> keys) {
    if (!cJSON_IsObject(o))
        return false;
    for (auto* p = o->child; p; p = p->next) {
        if (!p->string || std::none_of(keys.begin(), keys.end(),
                                       [&](auto* k) { return std::string(p->string) == k; }))
            return false;
        for (auto* q = p->next; q; q = q->next)
            if (q->string && std::string(p->string) == q->string)
                return false;
    }
    return true;
}
bool Number(const cJSON* p, double lo, double hi) {
    return cJSON_IsNumber(p) && std::isfinite(p->valuedouble) && p->valuedouble >= lo &&
           p->valuedouble <= hi;
}
}  // namespace
bool FileId(const std::string& id) {
    return !id.empty() && id.size() <= 40 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '-' || c == '_';
    });
}
std::expected<Action, std::string> ParseAction(const std::string& json) {
    if (json.empty() || json.size() > kMaxActionBytes)
        return std::unexpected("Action exceeds 12 KiB");
    const char* end = nullptr;
    CJsonUniquePtr root(cJSON_ParseWithLengthOpts(json.c_str(), json.size() + 1, &end, true));
    if (!root || !Keys(root.get(), {"format", "id", "name", "description", "model_version",
                                    "binding", "angle_unit", "joints", "segments", "end_mode"}))
        return std::unexpected("Invalid action object or unknown/duplicate fields");
    Action a;
    a.id = Text(root.get(), "id");
    a.name = Text(root.get(), "name");
    a.description = Text(root.get(), "description");
    a.binding = Text(root.get(), "binding");
    a.end_mode = Text(root.get(), "end_mode");
    if (Text(root.get(), "format") != "lamp-relative-action/1" ||
        Text(root.get(), "angle_unit") != "urdf_rad" ||
        Text(root.get(), "model_version") != pose::kModel || !FileId(a.id) || a.name.empty() ||
        a.name.size() > 96 || a.description.empty() || a.description.size() > 768 ||
        a.binding.size() != 64 ||
        !std::all_of(a.binding.begin(), a.binding.end(),
                     [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }) ||
        (a.end_mode != "return_to_reference" && a.end_mode != "hold_at_end"))
        return std::unexpected("Action identity, binding or metadata invalid");
    const auto* joints = Get(root.get(), "joints");
    if (!cJSON_IsArray(joints) || cJSON_GetArraySize(joints) != 5)
        return std::unexpected("Expected five joint selection booleans");
    bool any = false;
    for (int i = 0; i < 5; ++i) {
        auto* p = cJSON_GetArrayItem(joints, i);
        if (!cJSON_IsBool(p))
            return std::unexpected("Joint selection must be boolean");
        any |= a.joints[i] = cJSON_IsTrue(p);
    }
    if (!any)
        return std::unexpected("Select at least one joint");
    const auto* segments = Get(root.get(), "segments");
    if (!cJSON_IsArray(segments) || cJSON_GetArraySize(segments) < 1 ||
        cJSON_GetArraySize(segments) >= int(kMaxFrames))
        return std::unexpected("Action needs 2-32 keyframes (1-31 increments)");
    double total = 0;
    for (auto* p = segments->child; p; p = p->next) {
        if (!Keys(p, {"delta_rad", "duration_sec"}) || !Number(Get(p, "duration_sec"), .2, 10))
            return std::unexpected("Segment time must be 0.2-10 seconds");
        const auto* delta = Get(p, "delta_rad");
        if (!Keys(delta, {"baseYaw", "shoulder", "elbow", "wrist", "headRoll"}) ||
            cJSON_GetArraySize(delta) != 5)
            return std::unexpected(
                "Expected five increments; increments are not absolute URDF poses");
        Segment s;
        s.seconds = Get(p, "duration_sec")->valuedouble;
        for (int j = 0; j < 5; ++j) {
            if (!Number(Get(delta, pose::kNames[j]), -2 * std::numbers::pi, 2 * std::numbers::pi))
                return std::unexpected("Invalid joint increment");
            s.delta[j] = Get(delta, pose::kNames[j])->valuedouble;
        }
        for (int i = 0; i < 5; ++i)
            if (std::abs(s.delta[i]) > 2 * std::numbers::pi ||
                (!a.joints[i] && std::abs(s.delta[i]) > 1e-9))
                return std::unexpected("Invalid increment or increment on unselected joint");
        total += s.seconds;
        a.segments.push_back(s);
    }
    if (total > 50)
        return std::unexpected("Forward action must be <=50 seconds (10s return reserve)");
    return a;
}
std::string EncodeAction(const Action& a) {
    CJsonUniquePtr o(cJSON_CreateObject());
    cJSON_AddStringToObject(o.get(), "format", "lamp-relative-action/1");
    cJSON_AddStringToObject(o.get(), "id", a.id.c_str());
    cJSON_AddStringToObject(o.get(), "name", a.name.c_str());
    cJSON_AddStringToObject(o.get(), "description", a.description.c_str());
    cJSON_AddStringToObject(o.get(), "model_version", pose::kModel);
    cJSON_AddStringToObject(o.get(), "binding", a.binding.c_str());
    cJSON_AddStringToObject(o.get(), "angle_unit", "urdf_rad");
    cJSON_AddStringToObject(o.get(), "end_mode", a.end_mode.c_str());
    auto* js = cJSON_AddArrayToObject(o.get(), "joints");
    for (bool v : a.joints)
        cJSON_AddItemToArray(js, cJSON_CreateBool(v));
    auto* ss = cJSON_AddArrayToObject(o.get(), "segments");
    for (const auto& s : a.segments) {
        auto* seg = cJSON_CreateObject();
        auto* delta = cJSON_AddObjectToObject(seg, "delta_rad");
        for (int i = 0; i < 5; ++i)
            cJSON_AddNumberToObject(delta, pose::kNames[i], s.delta[i]);
        cJSON_AddNumberToObject(seg, "duration_sec", s.seconds);
        cJSON_AddItemToArray(ss, seg);
    }
    return Encode(o.get());
}
std::expected<pose::Plan, std::string> RelativePlan(const Action& a,
                                                    const pose::Configuration& config,
                                                    const pose::Pose& entry,
                                                    const std::string& end_mode) {
    if (end_mode != "hold_at_end" && end_mode != "return_to_reference")
        return std::unexpected("Invalid end_mode");
    pose::Plan plan;
    plan.entry = entry;
    plan.reference = entry;
    auto current = entry;
    for (const auto& seg : a.segments) {
        auto target = current;
        for (int j = 0; j < 5; ++j)
            target[j] += seg.delta[j];
        pose::Phase phase;
        phase.expressive = false;
        phase.from = current;
        phase.to = target;
        phase.duration = seg.seconds;
        plan.phases.push_back(phase);
        current = target;
    }
    if (end_mode == "return_to_reference") {
        pose::Phase p;
        p.expressive = false;
        p.from = current;
        p.to = entry;
        p.duration = 2;
        plan.phases.push_back(p);
    }
    if (!std::isfinite(plan.Duration()) || plan.Duration() > 60)
        return std::unexpected("Action duration exceeds 60 seconds");
    // Check every segment, not uniform samples over the whole group (short segments could be
    // missed).
    for (const auto& phase : plan.phases) {
        double distance = 0;
        for (int j = 0; j < 5; ++j)
            distance = std::max(distance, std::abs(phase.to[j] - phase.from[j]));
        const unsigned n = std::max(20U, unsigned(std::ceil(distance / (std::numbers::pi / 180))));
        for (unsigned k = 0; k <= n; ++k) {
            auto q = phase.At(double(k) / n);
            if (!pose::Safe(q, config.obstacles) || !pose::ToTicks(config, q))
                return std::unexpected(
                    "Relative path exceeds URDF, mapping or geometry limits; no scaling");
        }
    }
    plan.ok = true;
    return plan;
}
std::expected<Observation, std::string> ParseObservation(const std::string& response) {
    if (response.empty() || response.size() > 8192)
        return std::unexpected("Vision response exceeds 8 KiB");
    // Existing vision APIs may wrap the model text; unwrap known fields only, never scrape
    // arbitrary braces.
    CJsonUniquePtr root(
        cJSON_ParseWithLengthOpts(response.c_str(), response.size() + 1, nullptr, true));
    if (!root)
        return std::unexpected("Vision response is not JSON");
    if (!Get(root.get(), "found")) {
        const cJSON* text = nullptr;
        for (auto* key : {"text", "answer", "content"})
            if (cJSON_IsString(Get(root.get(), key))) {
                text = Get(root.get(), key);
                break;
            }
        const auto* content = Get(root.get(), "content");
        if (!text && cJSON_IsArray(content) && cJSON_GetArraySize(content) == 1)
            text = Get(cJSON_GetArrayItem(content, 0), "text");
        if (!cJSON_IsString(text))
            return std::unexpected("Vision response has no structured position");
        std::string value = text->valuestring;
        const auto first = value.find_first_not_of(" \r\n\t");
        if (first == std::string::npos)
            return std::unexpected("Empty vision content");
        const auto last = value.find_last_not_of(" \r\n\t");
        value = value.substr(first, last - first + 1);
        if (value.starts_with("```json\n") && value.ends_with("```"))
            value = value.substr(8, value.size() - 11);
        root.reset(cJSON_ParseWithLengthOpts(value.c_str(), value.size() + 1, nullptr, true));
    }
    if (!Keys(root.get(), {"found", "cx", "cy", "confidence"}) ||
        !cJSON_IsBool(Get(root.get(), "found")) || !Number(Get(root.get(), "confidence"), 0, 1))
        return std::unexpected("Vision must return found,cx,cy,confidence");
    Observation o;
    o.found = cJSON_IsTrue(Get(root.get(), "found"));
    o.confidence = Get(root.get(), "confidence")->valuedouble;
    if (o.found) {
        if (!Number(Get(root.get(), "cx"), 0, 1) || !Number(Get(root.get(), "cy"), 0, 1))
            return std::unexpected("Book center must be normalized 0-1");
        o.cx = Get(root.get(), "cx")->valuedouble;
        o.cy = Get(root.get(), "cy")->valuedouble;
    }
    return o;
}
std::expected<TrackingOptions, std::string> ParseTracking(const cJSON* o) {
    if (!Keys(o, {"interval_sec", "gain_deg", "deadzone", "max_step_deg", "duration_sec",
                  "confidence", "yaw_sign", "pitch_sign"}))
        return std::unexpected("Unknown tracking option");
    TrackingOptions t;
    struct Field {
        const char* name;
        double* value;
        double lo, hi;
    };
    for (auto f :
         {Field{"interval_sec", &t.interval, 3, 30}, Field{"gain_deg", &t.gain_deg, 1, 20},
          Field{"deadzone", &t.deadzone, .02, .25}, Field{"max_step_deg", &t.max_step_deg, .2, 3},
          Field{"duration_sec", &t.duration, .5, 5}, Field{"confidence", &t.confidence, .5, 1},
          Field{"yaw_sign", &t.yaw_sign, -1, 1}, Field{"pitch_sign", &t.pitch_sign, -1, 1}}) {
        const auto* p = Get(o, f.name);
        if (!p)
            continue;
        if (!Number(p, f.lo, f.hi))
            return std::unexpected(std::string("Invalid ") + f.name);
        *f.value = p->valuedouble;
    }
    if (std::abs(t.yaw_sign) != 1 || std::abs(t.pitch_sign) != 1)
        return std::unexpected(
            "First verify image directions and explicitly set yaw_sign/pitch_sign to +1 or -1");
    return t;
}
pose::Pose TrackingTarget(const pose::Pose& q, const Observation& o, const TrackingOptions& t) {
    auto target = q;
    if (!o.found || o.confidence < t.confidence)
        return target;
    auto delta = [&](double error, double sign) {
        if (std::abs(error) <= t.deadzone)
            return 0.;
        return sign * std::clamp(error * t.gain_deg, -t.max_step_deg, t.max_step_deg) *
               std::numbers::pi / 180;
    };
    target[0] += delta(o.cx - .5, t.yaw_sign);
    target[3] += delta(o.cy - .5, t.pitch_sign);
    return target;
}
}  // namespace lamp::delivery
