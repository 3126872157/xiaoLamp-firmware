#pragma once
#include <array>
#include <expected>
#include <optional>
#include <string>
#include <vector>
#include "lamp_protocol.h"

// Portable planning only. No clocks, UART, GPIO, torque or simulated completion.
namespace lamp::pose {
// TEMPORARY developer experiment. Restore validated posture envelopes before release.
// This does not bypass URDF travel, collision checks or installed servo limits.
inline constexpr bool kTemporaryModeLimitsDisabled = true;
using Pose = std::array<double, 5>;
struct Vec {
    double x = 0, y = 0, z = 0;
    Vec operator+(Vec b) const;
    Vec operator-(Vec b) const;
    Vec operator*(double s) const;
    double Dot(Vec b) const;
    double Length() const;
    Vec Unit() const;
};
struct Joint {
    Vec xyz, rpy, axis;
    double lower, upper;
};
struct Field {
    const char* skill;
    const char* name;
    double min, max;
    const char* choices;
};
struct Range {
    double min = 0, max = 0;
};
struct Target {
    Pose pose{};
    bool scene_matches = false;
};
struct Profile {
    Pose home{};
    std::array<Range, 5> ranges{};
    std::array<std::optional<Target>, 2> targets;
};
struct Box {
    Vec min, max;
};
struct Configuration {
    CJsonUniquePtr source;
    std::array<Profile, 3> modes;
    std::vector<Box> obstacles;
};
struct Fk {
    Vec shoulder, elbow, wrist, tip, gaze;
};
enum class Curve { Move, Hold, Nod, Wave, Tail, Pulse };
struct Phase {
    Curve curve = Curve::Move;
    bool expressive = true;
    Pose from{}, to{};
    double duration = 0, amplitude = 0, period = 0, gap = 0, cycles = 0;
    Pose At(double u) const;
};
struct Plan {
    bool ok = false, limited = false, requires_light = false;
    std::string error;
    int mode_after = 0;
    Pose entry{}, reference{}, prepare{};
    double prepare_duration = 0, scale = 1;
    std::vector<Phase> phases;
    double Duration() const;
    Pose At(double seconds) const;
};
std::expected<Configuration, std::string> ParseConfiguration(const std::string& json);
std::expected<Pose, std::string> ParsePose(const cJSON* json);
Fk Forward(const Pose& q);
Pose Blend(const Pose& a, const Pose& b, double u);
bool InUrdf(const Pose& q);
bool InMode(const Pose& q, const Profile& p);
bool Safe(const Pose& q, const std::vector<Box>& obstacles = {});
Plan Build(const Configuration& config, int mode, const Pose& current, const cJSON* semantic_step,
           bool dialogue = false);
// Rounding stays inside position limits. Explicit mapping confirmation belongs to hardware adapter.
std::expected<std::array<int, 5>, std::string> ToTicks(const Configuration& config, const Pose& q);
}  // namespace lamp::pose
