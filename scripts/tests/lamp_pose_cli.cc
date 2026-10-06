#include <iostream>
#include <iterator>
#include "lamp_pose_model.h"

namespace {
void PoseJson(cJSON* o, const char* key, const lamp::pose::Pose& p) {
    auto* q = cJSON_AddObjectToObject(o, key);
    for (int j = 0; j < 5; j++)
        cJSON_AddNumberToObject(q, lamp::pose::kNames[j], p[j]);
}
void VectorJson(cJSON* o, const char* key, lamp::pose::Vec v) {
    const double values[] = {v.x, v.y, v.z};
    cJSON_AddItemToObject(o, key, cJSON_CreateDoubleArray(values, 3));
}
}  // namespace
int main() {
    using namespace lamp;
    std::string input(std::istreambuf_iterator<char>{std::cin}, {});
    CJsonUniquePtr root(cJSON_Parse(input.c_str())), output(cJSON_CreateObject());
    auto* raw_config = cJSON_GetObjectItemCaseSensitive(root.get(), "config");
    auto config =
        pose::ParseConfiguration(raw_config ? Encode(raw_config) : pose::kDeviceConfiguration);
    if (!config) {
        cJSON_AddBoolToObject(output.get(), "ok", false);
        cJSON_AddStringToObject(output.get(), "error", config.error().c_str());
        std::cout << Encode(output.get());
        return 0;
    }
    auto q = pose::ParsePose(cJSON_GetObjectItemCaseSensitive(root.get(), "pose"));
    if (!q) {
        cJSON_AddBoolToObject(output.get(), "ok", false);
        cJSON_AddStringToObject(output.get(), "error", q.error().c_str());
        std::cout << Encode(output.get());
        return 0;
    }
    if (Text(root.get(), "operation") == "plan") {
        const auto mode = Text(root.get(), "mode");
        const int m = mode == "sit" ? 0 : mode == "stand" ? 1 : mode == "work" ? 2 : -1;
        const auto plan =
            pose::Build(*config, m, *q, cJSON_GetObjectItemCaseSensitive(root.get(), "step"),
                        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root.get(), "dialogue")));
        cJSON_AddBoolToObject(output.get(), "ok", plan.ok);
        cJSON_AddStringToObject(output.get(), "error", plan.error.c_str());
        cJSON_AddBoolToObject(output.get(), "limited", plan.limited);
        cJSON_AddBoolToObject(output.get(), "requires_light", plan.requires_light);
        cJSON_AddNumberToObject(output.get(), "duration", plan.Duration());
        cJSON_AddNumberToObject(output.get(), "scale", plan.scale);
        cJSON_AddNumberToObject(output.get(), "mode_after", plan.mode_after);
        cJSON_AddNumberToObject(output.get(), "prepare_duration", plan.prepare_duration);
        PoseJson(output.get(), "prepare", plan.prepare);
        auto* phases = cJSON_AddArrayToObject(output.get(), "phases");
        for (const auto& p : plan.phases) {
            auto* v = cJSON_CreateNumber(p.duration);
            cJSON_AddItemToArray(phases, v);
        }
        auto* samples = cJSON_AddArrayToObject(output.get(), "samples");
        auto* times = cJSON_GetObjectItemCaseSensitive(root.get(), "times");
        if (plan.ok && cJSON_GetArraySize(times) <= 201)
            for (auto* t = times ? times->child : nullptr; t; t = t->next) {
                auto* sample = cJSON_CreateObject();
                PoseJson(sample, "pose", plan.At(t->valuedouble));
                cJSON_AddItemToArray(samples, sample);
            }
    } else {
        const auto f = pose::Forward(*q);
        cJSON_AddBoolToObject(output.get(), "ok", true);
        VectorJson(output.get(), "tip", f.tip);
        VectorJson(output.get(), "gaze", f.gaze);
        VectorJson(output.get(), "shoulder", f.shoulder);
        VectorJson(output.get(), "elbow", f.elbow);
        VectorJson(output.get(), "wrist", f.wrist);
        cJSON_AddBoolToObject(output.get(), "safe", pose::Safe(*q, config->obstacles));
        auto ticks = pose::ToTicks(*config, *q);
        if (ticks)
            cJSON_AddItemToObject(output.get(), "ticks", cJSON_CreateIntArray(ticks->data(), 5));
    }
    std::cout << Encode(output.get());
}
