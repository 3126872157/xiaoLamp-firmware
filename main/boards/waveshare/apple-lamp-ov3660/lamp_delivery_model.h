#pragma once
#include "lamp_pose_plan.h"

namespace lamp::delivery {
inline constexpr size_t kMaxActionBytes = 12288;
inline constexpr unsigned kMaxFrames = 32;
inline constexpr unsigned kMaxActions = 24;
struct Segment {
    pose::Pose delta{};
    double seconds = 1;
};
struct Action {
    std::string id, name, description, binding;
    std::array<bool, 5> joints{};
    std::vector<Segment> segments;
    std::string end_mode = "return_to_reference";
};
bool FileId(const std::string& id);
std::expected<Action, std::string> ParseAction(const std::string& json);
std::string EncodeAction(const Action& action);
std::expected<pose::Plan, std::string> RelativePlan(const Action& action,
                                                    const pose::Configuration& config,
                                                    const pose::Pose& entry,
                                                    const std::string& end_mode);
struct Observation {
    bool found = false;
    double cx = .5, cy = .5, confidence = 0;
};
struct TrackingOptions {
    double interval = 5, gain_deg = 6, deadzone = .08, max_step_deg = 2, duration = 2,
           confidence = .7, yaw_sign = 0, pitch_sign = 0;
};
std::expected<Observation, std::string> ParseObservation(const std::string& response);
std::expected<TrackingOptions, std::string> ParseTracking(const cJSON* json);
pose::Pose TrackingTarget(const pose::Pose& current, const Observation& observation,
                          const TrackingOptions& options);
}  // namespace lamp::delivery
