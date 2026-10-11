#include <iostream>
#include "lamp_delivery_model.h"
#include "lamp_pose_model.h"
int main() {
    std::string input((std::istreambuf_iterator<char>(std::cin)), {});
    CJsonUniquePtr root(cJSON_Parse(input.c_str())), out(cJSON_CreateObject());
    auto get = [&](const char* k) { return cJSON_GetObjectItemCaseSensitive(root.get(), k); };
    std::string error;
    const auto op = lamp::Text(root.get(), "op");
    if (op == "vision") {
        auto v = lamp::delivery::ParseObservation(lamp::Text(root.get(), "response"));
        if (!v)
            error = v.error();
        else {
            cJSON_AddBoolToObject(out.get(), "found", v->found);
            cJSON_AddNumberToObject(out.get(), "cx", v->cx);
            cJSON_AddNumberToObject(out.get(), "cy", v->cy);
        }
    } else if (op == "tracking") {
        auto t = lamp::delivery::ParseTracking(get("options"));
        if (!t)
            error = t.error();
        else {
            auto v = lamp::delivery::ParseObservation(lamp::Text(root.get(), "response"));
            auto q = lamp::pose::ParsePose(get("entry"));
            if (!v || !q)
                error = "Invalid observation/entry";
            else {
                const auto target = lamp::delivery::TrackingTarget(*q, *v, *t);
                auto* p = cJSON_AddArrayToObject(out.get(), "target");
                for (double x : target)
                    cJSON_AddItemToArray(p, cJSON_CreateNumber(x));
            }
        }
    } else {
        auto a = lamp::delivery::ParseAction(lamp::Encode(get("action")));
        if (!a)
            error = a.error();
        else if (op == "parse")
            cJSON_AddItemToObject(out.get(), "action",
                                  cJSON_Parse(lamp::delivery::EncodeAction(*a).c_str()));
        else {
            auto cfg = lamp::pose::ParseConfiguration(lamp::pose::kDeviceConfiguration);
            auto q = lamp::pose::ParsePose(get("entry"));
            if (!cfg || !q)
                error = "Invalid configuration/entry";
            else {
                auto plan =
                    lamp::delivery::RelativePlan(*a, *cfg, *q, lamp::Text(root.get(), "end_mode"));
                if (!plan)
                    error = plan.error();
                else {
                    auto* p = cJSON_AddArrayToObject(out.get(), "final");
                    for (double x : plan->At(plan->Duration()))
                        cJSON_AddItemToArray(p, cJSON_CreateNumber(x));
                    cJSON_AddNumberToObject(out.get(), "duration", plan->Duration());
                }
            }
        }
    }
    cJSON_AddBoolToObject(out.get(), "ok", error.empty());
    if (!error.empty())
        cJSON_AddStringToObject(out.get(), "error", error.c_str());
    std::cout << lamp::Encode(out.get());
}
