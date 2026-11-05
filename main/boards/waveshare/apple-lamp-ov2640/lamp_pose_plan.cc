#include "lamp_pose_plan.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include "lamp_pose_model.h"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#if !defined(ESP_PLATFORM) || CONFIG_APPLE_LAMP_LOCAL_EXECUTOR
#if defined(ESP_PLATFORM) && !CONFIG_COMPILER_CXX_EXCEPTIONS
#error "Local planner validation requires its variant's C++ exception configuration"
#endif

namespace lamp::pose {
namespace {
constexpr double kPi = std::numbers::pi, kDeg = kPi / 180;
// A spoken reaction is deliberately quieter than an explicit request: the same
// primitive at full amplitude reads as a command, not as a mannerism.
constexpr double kDialogueIntensity = .6;
constexpr double kDialogueNodIntensity = .8;
const char* kModes[] = {"sit", "stand", "work"};
const char* kTargets[] = {"user", "book"};
const cJSON* Item(const cJSON* o, const char* k) { return cJSON_GetObjectItemCaseSensitive(o, k); }
double Number(const cJSON* o, const char* k) {
    auto* v = Item(o, k);
    if (!cJSON_IsNumber(v) || !std::isfinite(v->valuedouble))
        throw std::runtime_error(std::string("Invalid number: ") + k);
    return v->valuedouble;
}
bool Close(double a, double b) { return std::abs(a - b) <= 1e-8; }
double Smooth(double u) {
    u = std::clamp(u, 0., 1.);
    return u * u * u * (u * (u * 6 - 15) + 10);
}
double Smooth3(double u) {
    u = std::clamp(u, 0., 1.);
    return u * u * (3 - 2 * u);
}
Vec Cross(Vec a, Vec b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vec Vector(const cJSON* value) {
    if (!cJSON_IsArray(value) || cJSON_GetArraySize(value) != 3)
        throw std::runtime_error("Expected three-vector");
    double v[3];
    for (int i = 0; i < 3; i++) {
        auto* n = cJSON_GetArrayItem(value, i);
        if (!cJSON_IsNumber(n) || !std::isfinite(n->valuedouble))
            throw std::runtime_error("Invalid vector");
        v[i] = n->valuedouble;
    }
    return {v[0], v[1], v[2]};
}
bool Same(Vec a, Vec b) { return (a - b).Length() < 1e-8; }
using Matrix = std::array<std::array<double, 3>, 3>;
Vec Rotate(const Matrix& m, Vec v) {
    return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z,
            m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
            m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
}
Matrix Multiply(const Matrix& a, const Matrix& b) {
    Matrix c{};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            for (int k = 0; k < 3; k++)
                c[i][j] += a[i][k] * b[k][j];
    return c;
}
Matrix Axis(Vec a, double q) {
    a = a.Unit();
    const double c = std::cos(q), s = std::sin(q), t = 1 - c;
    const double v[] = {a.x, a.y, a.z};
    Matrix m{};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            m[i][j] = v[i] * v[j] * t + (i == j ? c : 0);
    m[0][1] -= a.z * s;
    m[0][2] += a.y * s;
    m[1][0] += a.z * s;
    m[1][2] -= a.x * s;
    m[2][0] -= a.y * s;
    m[2][1] += a.x * s;
    return m;
}
Vec Scene(Vec v) { return {-v.x, v.z + kLift, v.y}; }
bool Path(const Pose& a, const Pose& b, const std::vector<Box>& boxes, int minimum = 32,
          double step = .015) {
    double span = 0;
    for (int j = 0; j < 5; j++)
        span = std::max(span, std::abs(a[j] - b[j]));
    const int samples = std::max(minimum, static_cast<int>(std::ceil(span / step)));
    for (int i = 0; i <= samples; i++)
        if (!Safe(Blend(a, b, double(i) / samples), boxes))
            return false;
    return true;
}
bool AllowedTarget(int mode, int target) { return target == 0 ? mode != 2 : mode != 0; }
Pose Aim(const Configuration& config, int mode, const std::string& id) {
    const int target = id == "user" ? 0 : id == "book" ? 1 : -1;
    if (target < 0 || !AllowedTarget(mode, target))
        throw std::runtime_error("Target unavailable in current mode");
    const auto& t = config.modes[mode].targets[target];
    if (!t)
        throw std::runtime_error("Target not calibrated");
    if (!t->scene_matches)
        throw std::runtime_error("Target scene changed; calibration needs confirmation");
    return t->pose;
}
Phase Move(const Pose& a, const Pose& b, double duration) {
    Phase p;
    p.from = a;
    p.to = b;
    p.duration = duration;
    return p;
}
Phase Hold(const Pose& q, double duration) {
    auto p = Move(q, q, duration);
    p.curve = Curve::Hold;
    return p;
}
// Same bounded damped least-squares solve as the Web, moving joints 2/3/4 only.
Pose Solve(Vec position, Vec direction, const Pose& seed) {
    Pose q = seed;
    for (int j = 0; j < 5; j++)
        q[j] = std::clamp(q[j], kJoints[j].lower, kJoints[j].upper);
    auto residual = [&](const Pose& p) {
        const auto f = Forward(p);
        const auto a = (f.tip - position) * (1 / kHeight), b = f.gaze - direction;
        return std::array<double, 9>{a.x,
                                     a.y,
                                     a.z,
                                     b.x,
                                     b.y,
                                     b.z,
                                     (p[1] - seed[1]) * .003,
                                     (p[2] - seed[2]) * .003,
                                     (p[3] - seed[3]) * .003};
    };
    for (int iter = 0; iter < 65; iter++) {
        const auto r = residual(q);
        double cols[3][9]{};
        for (int i = 0; i < 3; i++) {
            auto plus = q;
            const int j = i + 1;
            const double h = q[j] + 1e-5 <= kJoints[j].upper ? 1e-5 : -1e-5;
            plus[j] += h;
            const auto rr = residual(plus);
            for (int k = 0; k < 9; k++)
                cols[i][k] = (rr[k] - r[k]) / h;
        }
        double m[3][4]{};
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) {
                m[i][j] = i == j ? .0025 : 0;
                for (int k = 0; k < 9; k++)
                    m[i][j] += cols[i][k] * cols[j][k];
            }
            for (int k = 0; k < 9; k++)
                m[i][3] -= cols[i][k] * r[k];
        }
        for (int k = 0; k < 3; k++) {
            int pivot = k;
            for (int i = k + 1; i < 3; i++)
                if (std::abs(m[i][k]) > std::abs(m[pivot][k]))
                    pivot = i;
            for (int j = 0; j < 4; j++)
                std::swap(m[k][j], m[pivot][j]);
            const double d = m[k][k];
            for (int j = k; j < 4; j++)
                m[k][j] /= d;
            for (int i = 0; i < 3; i++)
                if (i != k) {
                    const double v = m[i][k];
                    for (int j = k; j < 4; j++)
                        m[i][j] -= v * m[k][j];
                }
        }
        double maximum = .18, moved = 0, norm = 0;
        for (auto v : r)
            norm += v * v;
        for (int i = 0; i < 3; i++)
            maximum = std::max(maximum, std::abs(m[i][3]));
        for (int i = 0; i < 3; i++) {
            const int j = i + 1;
            const double v =
                std::clamp(q[j] + m[i][3] * .18 / maximum, kJoints[j].lower, kJoints[j].upper);
            moved = std::max(moved, std::abs(v - q[j]));
            q[j] = v;
        }
        if (moved < 1e-6 || norm < 1e-9)
            break;
    }
    return q;
}
}  // namespace
Vec Vec::operator+(Vec b) const { return {x + b.x, y + b.y, z + b.z}; }
Vec Vec::operator-(Vec b) const { return {x - b.x, y - b.y, z - b.z}; }
Vec Vec::operator*(double s) const { return {x * s, y * s, z * s}; }
double Vec::Dot(Vec b) const { return x * b.x + y * b.y + z * b.z; }
double Vec::Length() const { return std::sqrt(Dot(*this)); }
Vec Vec::Unit() const {
    const double n = Length();
    return n > 0 ? *this * (1 / n) : Vec{};
}
Pose Blend(const Pose& a, const Pose& b, double u) {
    Pose p;
    for (int j = 0; j < 5; j++) {
        double d = b[j] - a[j];
        if (j == 0) {
            while (d > kPi)
                d -= 2 * kPi;
            while (d <= -kPi)
                d += 2 * kPi;
        }
        p[j] = a[j] + d * u;
    }
    return p;
}
bool InUrdf(const Pose& q) {
    for (int j = 0; j < 5; j++)
        if (!std::isfinite(q[j]) || q[j] < kJoints[j].lower || q[j] > kJoints[j].upper)
            return false;
    return true;
}
bool InMode(const Pose& q, const Profile& p) {
    if (!InUrdf(q))
        return false;
    for (int j = 0; j < 5; j++)
        if (j != 4 && (q[j] - p.home[j] < p.ranges[j].min - 1e-8 ||
                       q[j] - p.home[j] > p.ranges[j].max + 1e-8))
            return false;
    return true;
}
std::expected<Pose, std::string> ParsePose(const cJSON* raw) {
    try {
        Pose q;
        for (int j = 0; j < 5; j++)
            q[j] = Number(raw, kNames[j]);
        if (!InUrdf(q))
            throw std::runtime_error("Pose outside URDF limits");
        return q;
    } catch (const std::exception& e) {
        return std::unexpected(e.what());
    }
}
Fk Forward(const Pose& q) {
    Matrix r = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    Vec t;
    std::array<Vec, 5> origins;
    for (int i = 0; i < 5; i++) {
        const auto& j = kJoints[i];
        t = t + Rotate(r, j.xyz);
        r = Multiply(r, Multiply(Axis({0, 0, 1}, j.rpy.z),
                                 Multiply(Axis({0, 1, 0}, j.rpy.y), Axis({1, 0, 0}, j.rpy.x))));
        r = Multiply(r, Axis(j.axis, q[i]));
        origins[i] = Scene(t);
    }
    const auto axis = Rotate(r, kOptical).Unit();
    return {
        origins[1], origins[2], origins[3], Scene(t + Rotate(r, kTool)), {-axis.x, axis.z, axis.y}};
}
bool Safe(const Pose& q, const std::vector<Box>& boxes) {
    if (!InUrdf(q))
        return false;
    const auto f = Forward(q);
    auto side = Cross(f.gaze, {0, 1, 0});
    if (side.Dot(side) < 1e-8)
        side = {1, 0, 0};
    side = side.Unit();
    const auto other = Cross(f.gaze, side).Unit(), back = f.tip - f.gaze * kShadeLength;
    std::array<Vec, 15> pts = {f.tip, back};
    int count = 2;
    for (auto d : {side, other, side * (-1), other * (-1)}) {
        pts[count++] = back + d * kShadeRadius;
        pts[count++] = f.tip + d * (kShadeRadius * .72);
    }
    pts[count++] = f.shoulder;
    pts[count++] = (f.shoulder + f.elbow) * .5;
    pts[count++] = f.elbow;
    pts[count++] = (f.elbow + f.wrist) * .5;
    pts[count++] = f.wrist;
    for (auto p : pts) {
        if (p.y < .001 || (p.y < .0433 - .002 && std::hypot(p.x, p.z) < kBaseRadius + .001))
            return false;
        for (const auto& b : boxes) {
            const double dx = std::max({b.min.x - p.x, 0., p.x - b.max.x}),
                         dy = std::max({b.min.y - p.y, 0., p.y - b.max.y}),
                         dz = std::max({b.min.z - p.z, 0., p.z - b.max.z});
            if (std::sqrt(dx * dx + dy * dy + dz * dz) < .005)
                return false;
        }
    }
    const auto ab = f.elbow - f.shoulder;
    const double len = ab.Dot(ab);
    for (int i = 0; i < 10; i++) {
        const double u = len < 1e-12 ? 0 : std::clamp((pts[i] - f.shoulder).Dot(ab) / len, 0., 1.);
        if ((pts[i] - f.shoulder - ab * u).Length() < .012)
            return false;
    }
    return true;
}
std::expected<Configuration, std::string> ParseConfiguration(const std::string& json) {
    try {
        if (json.size() > 65536)
            throw std::runtime_error("Configuration exceeds 64KiB");
        Configuration c;
        c.source.reset(cJSON_Parse(json.c_str()));
        const auto* root = c.source.get();
        if (Text(root, "format") != "lamp-device/1" || Text(root, "model_version") != kModel ||
            Text(root, "calibration_version") != kCalibration ||
            Text(root, "angle_unit") != "urdf_rad")
            throw std::runtime_error("Configuration model/version/unit mismatch");
        const auto* joints = Item(root, "joints");
        if (cJSON_GetArraySize(joints) != 5)
            throw std::runtime_error("Expected five joints");
        for (int i = 0; i < 5; i++) {
            const auto* j = cJSON_GetArrayItem(joints, i);
            const auto& trusted = kJoints[i];
            if (Text(j, "name") != kNames[i] ||
                Text(j, "urdf_joint") != "joint" + std::to_string(i + 1) ||
                Text(j, "parent") != (i == 0 ? "base" : "link" + std::to_string(i)) ||
                Text(j, "child") != "link" + std::to_string(i + 1) ||
                !Same(Vector(Item(j, "xyz")), trusted.xyz) ||
                !Same(Vector(Item(j, "rpy")), trusted.rpy) ||
                !Same(Vector(Item(j, "axis")), trusted.axis) ||
                !Close(Number(j, "lower"), trusted.lower) ||
                !Close(Number(j, "upper"), trusted.upper))
                throw std::runtime_error("Joint chain/limits differ from firmware model");
        }
        const auto* tool = Item(root, "tool");
        const auto* geometry = Item(root, "geometry");
        if (Text(tool, "link") != "link5" || !Same(Vector(Item(tool, "xyz")), kTool) ||
            !Same(Vector(Item(tool, "opticalAxis")), kOptical) ||
            !Same(Vector(Item(tool, "rpy")), {}) ||
            !Close(Number(geometry, "height_ref_m"), kHeight) ||
            !Close(Number(geometry, "shade_radius_m"), kShadeRadius) ||
            !Close(Number(geometry, "shade_length_m"), kShadeLength) ||
            !Close(Number(geometry, "base_radius_m"), kBaseRadius))
            throw std::runtime_error("Tool/geometry differs from firmware model");
        const auto* scene = Item(Item(root, "source_scene"), "targets");
        if (!cJSON_IsObject(scene) || cJSON_GetArraySize(scene) > 16)
            throw std::runtime_error("Invalid bounded scene");
        for (const auto* t = scene->child; t; t = t->next)
            if (Text(t, "kind") != "person") {
                const auto position = Vector(Item(t, "position")), size = Vector(Item(t, "size"));
                if (size.x <= 0 || size.y <= 0 || size.z <= 0 || size.Length() > 10 ||
                    position.Length() > 10)
                    throw std::runtime_error("Invalid scene dimensions");
                c.obstacles.push_back({position - size * .5, position + size * .5});
            }
        const auto* homes = Item(root, "homes");
        const auto* profiles = Item(root, "profiles");
        if (Text(homes, "robotModelVersion") != kModel ||
            Text(profiles, "robotModelVersion") != kModel ||
            Text(homes, "calibrationVersion") != kCalibration ||
            Text(profiles, "calibrationVersion") != kCalibration ||
            Number(homes, "formatVersion") != 1 || Number(profiles, "formatVersion") != 1 ||
            Text(profiles, "placementId").empty())
            throw std::runtime_error("Home/profile identity mismatch");
        for (int m = 0; m < 3; m++) {
            auto& p = c.modes[m];
            const auto* raw = Item(Item(profiles, "modes"), kModes[m]);
            const auto home = ParsePose(Item(Item(Item(homes, "homes"), kModes[m]), "pose")),
                       match = ParsePose(Item(raw, "home"));
            if (!home || !match || *home != *match || !Safe(*home))
                throw std::runtime_error("Invalid/inconsistent Home");
            p.home = *home;
            for (int j = 0; j < 5; j++) {
                const auto* r =
                    Item(Item(raw, j == 0 || j == 3 ? "ranges" : "bodyRanges"), kNames[j]);
                p.ranges[j] = {Number(r, "min"), Number(r, "max")};
                if (p.ranges[j].min > 0 || p.ranges[j].max < 0 || p.ranges[j].min < -2 * kPi ||
                    p.ranges[j].max > 2 * kPi)
                    throw std::runtime_error("Invalid mode range");
                // Mode envelopes are expressive workspace hints, not mechanical stops.
                // Use an idempotent minimum workspace so persisted configs do not grow
                // on every reload. URDF, installed ticks and geometry remain checked.
                if (j == 0 || j == 3 || j == 4)
                    p.ranges[j] = {kJoints[j].lower - p.home[j], kJoints[j].upper - p.home[j]};
                else
                    p.ranges[j] = {std::max(kJoints[j].lower - p.home[j],
                                            std::min(p.ranges[j].min, -45 * kDeg)),
                                   std::min(kJoints[j].upper - p.home[j],
                                            std::max(p.ranges[j].max, 45 * kDeg))};
                auto* stored = const_cast<cJSON*>(r);
                cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(stored, "min"),
                                     p.ranges[j].min);
                cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(stored, "max"),
                                     p.ranges[j].max);
            }
            const auto* targets = Item(raw, "targets");
            if (!cJSON_IsObject(targets))
                throw std::runtime_error("Invalid target calibrations");
            for (const auto* a = targets->child; a; a = a->next) {
                const std::string id = a->string ? a->string : "";
                const int t = id == "user" ? 0 : id == "book" ? 1 : -1;
                if (t < 0 || !AllowedTarget(m, t))
                    throw std::runtime_error("Unsupported mode calibration target");
                auto q = ParsePose(Item(a, "pose"));
                if (!q || !InMode(*q, p) || !Close((*q)[0] - p.home[0], Number(a, "yaw")) ||
                    !Close((*q)[3] - p.home[3], Number(a, "pitch")))
                    throw std::runtime_error("Invalid full calibration pose");
                const auto* source = Item(scene, kTargets[t]);
                bool matches = cJSON_IsTrue(Item(source, "valid"));
                const auto signature = Text(a, "sceneSignature");
                if (!signature.empty()) {
                    CJsonUniquePtr sig(cJSON_Parse(signature.c_str()));
                    matches &= cJSON_GetArraySize(sig.get()) == 3 &&
                               Same(Vector(cJSON_GetArrayItem(sig.get(), 0)),
                                    Vector(Item(source, "position"))) &&
                               Same(Vector(cJSON_GetArrayItem(sig.get(), 1)),
                                    Vector(Item(source, "size"))) &&
                               cJSON_IsTrue(cJSON_GetArrayItem(sig.get(), 2));
                }
                p.targets[t] = Target{*q, matches};
            }
        }
        const auto* presets = Item(root, "action_presets");
        for (const auto& f : kFields) {
            const auto* param = Item(Item(presets, f.skill), f.name);
            if (*f.choices) {
                const auto value = Text(Item(presets, f.skill), f.name);
                const auto hay = "|" + std::string(f.choices) + "|";
                if (value.empty() || hay.find("|" + value + "|") == std::string::npos)
                    throw std::runtime_error("Invalid local preset choice");
            } else {
                const double n = Number(Item(presets, f.skill), f.name);
                if (n < f.min || n > f.max)
                    throw std::runtime_error("Local preset outside schema bounds");
            }
            if (!param)
                throw std::runtime_error("Missing local preset");
        }
        const auto* hardware = Item(root, "hardware");
        const auto* servos = Item(hardware, "servos");
        const auto* uart = Item(hardware, "uart");
        if (cJSON_GetArraySize(servos) != 5 || Number(uart, "port") != 0 ||
            Number(uart, "tx_gpio") != 43 || Number(uart, "rx_gpio") != 44 ||
            Number(uart, "baud") != 115200)
            throw std::runtime_error("Invalid board UART mapping");
        for (int i = 0; i < 5; i++) {
            const auto* s = cJSON_GetArrayItem(servos, i);
            const double d = Number(s, "direction"), zero = Number(s, "zero_ticks"),
                         scale = Number(s, "degrees_per_tick");
            if (Text(s, "joint") != kNames[i] || Number(s, "id") != i + 1 || (d != 1 && d != -1) ||
                zero < -1000 || zero > 2000 || !Close(scale, .24))
                throw std::runtime_error("Invalid servo mapping");
        }
        return c;
    } catch (const std::exception& e) {
        return std::unexpected(e.what());
    }
}
Pose Phase::At(double u) const {
    u = std::clamp(u, 0., 1.);
    if (curve == Curve::Move)
        return Blend(from, to, Smooth(u));
    if (curve == Curve::Hold)
        return from;
    if (curve == Curve::Pulse)
        return Blend(from, to, std::sin(kPi * std::fmod(std::min(u, 1 - 1e-9) * cycles, 1.)));
    auto q = from;
    if (curve == Curve::Nod) {
        const double local = std::fmod(u * duration, period + gap) / period;
        const double v = local > 1      ? 0
                         : local <= .45 ? Smooth(local / .45)
                                        : 1 - Smooth((local - .45) / .55);
        q[3] -= amplitude * v;
    } else {
        const double wave = amplitude * std::min(Smooth3(u / .18), Smooth3((1 - u) / .18)) *
                            std::sin(u * cycles * 2 * kPi);
        q[0] += wave;
        if (curve == Curve::Tail) {
            q[1] += .8 * wave;
            q[2] -= .55 * wave;
        }
    }
    return q;
}
double Plan::Duration() const {
    double t = prepare_duration;
    for (const auto& p : phases)
        t += p.duration;
    return t;
}
Pose Plan::At(double seconds) const {
    if (seconds < prepare_duration)
        return Blend(entry, prepare, Smooth(seconds / prepare_duration));
    seconds -= prepare_duration;
    Pose q = prepare;
    for (const auto& p : phases) {
        const double u = p.duration == 0 ? 1 : std::min(seconds / p.duration, 1.);
        q = p.At(u);
        if (p.expressive)
            q = Blend(reference, q, scale);
        if (seconds <= p.duration)
            return q;
        seconds -= p.duration;
    }
    return q;
}
Plan Build(const Configuration& config, int mode, const Pose& measured, const cJSON* step,
           bool dialogue) {
    // Work on a mutable copy: a dialogue expression may first recover to the mode
    // home, and everything downstream must plan from that recovered pose.
    Pose current = measured;
    Plan plan;
    plan.entry = current;
    plan.reference = current;
    plan.prepare = current;
    plan.mode_after = mode;
    try {
        if (mode < 0 || mode > 2 || !InUrdf(current))
            throw std::runtime_error("Invalid current mode/pose");
        // Use the same production semantic validator as MCP; reject raw joint parameters.
        CJsonUniquePtr envelope(cJSON_CreateObject());
        cJSON_AddStringToObject(envelope.get(), "protocol_version", kProtocol);
        cJSON_AddStringToObject(envelope.get(), "request_id", "native-plan-check");
        cJSON_AddStringToObject(envelope.get(), "operation", "execute");
        cJSON_AddItemToObject(envelope.get(), "args", cJSON_Duplicate(step, true));
        const auto checked = Command(Encode(envelope.get()));
        if (!checked)
            throw std::runtime_error(checked.error());
        const auto action = Text(step, "action_id"), skill = Text(step, "skill_id");
        const auto& profile = config.modes[mode];
        auto route = [&](Pose target, int after, double duration, bool mode_only) {
            std::vector<std::vector<Pose>> routes = {{target}};
            if (!mode_only) {
                auto stand = config.modes[1].home;
                stand[0] = current[0];
                auto lifted = current;
                lifted[3] = std::clamp(15 * kDeg - current[1] - current[2], kJoints[3].lower,
                                       kJoints[3].upper);
                routes.push_back({stand, target});
                routes.push_back({lifted, stand, target});
            }
            for (const auto& points : routes) {
                auto previous = current;
                bool ok = true;
                for (const auto& p : points) {
                    if (!Path(previous, p, config.obstacles)) {
                        ok = false;
                        break;
                    }
                    previous = p;
                }
                if (ok) {
                    previous = current;
                    for (const auto& p : points) {
                        plan.phases.push_back(Move(previous, p, duration / points.size()));
                        previous = p;
                    }
                    plan.mode_after = after;
                    return;
                }
            }
            throw std::runtime_error("No safe transition route");
        };
        if (action == "wait") {
            plan.phases.push_back(Hold(current, Number(step, "duration_sec")));
            plan.ok = true;
            return plan;
        }
        if (action == "set_mode" || action == "restore_home" || action == "restore_initial" ||
            (action == "execute_primitive" && skill == "A04")) {
            int after = action == "restore_initial" || skill == "A04" ? 0 : mode;
            if (action == "set_mode") {
                const auto m = Text(step, "mode");
                after = m == "sit" ? 0 : m == "stand" ? 1 : 2;
            }
            auto target = config.modes[after].home;
            target[0] = action == "restore_initial" ? 0 : current[0];
            const double duration =
                skill == "A04" ? Number(Item(Item(config.source.get(), "action_presets"), "A04"),
                                        "durationSec")
                               : 1.8;
            route(target, after, duration, false);
            plan.ok = true;
            return plan;
        }
        const bool yaw_adjust =
            action == "adjust_pose" && (Text(step, "direction") == "yaw_positive" ||
                                        Text(step, "direction") == "yaw_negative");
        if (!yaw_adjust && !InMode(current, profile)) {
            // A spoken reply must still produce a visible reaction. When hand
            // guidance left the arm outside this mode's envelope, return to the mode
            // home at the CURRENT Yaw first, then plan the expression from there.
            if (!dialogue || !Safe(measured, config.obstacles))
                throw std::runtime_error("Current pose outside mode; restore Home first");
            auto home = profile.home;
            home[0] = current[0];
            if (!Path(current, home, config.obstacles))
                throw std::runtime_error("No safe route back to the mode home");
            plan.phases.push_back(Move(current, home, 0.9));
            plan.limited = true;
            current = home;
            plan.reference = home;
            plan.prepare = home;
        }
        if (action == "adjust_pose" || action == "gaze_calibrated") {
            auto target = current;
            if (action == "gaze_calibrated")
                target = Aim(config, mode, Text(step, "target_id"));
            else {
                const auto d = Text(step, "direction"), size = Text(step, "size");
                const double delta = (size == "large"    ? 20
                                      : size == "medium" ? 10
                                                         : 3) *
                                     kDeg * (d == "down" || d == "yaw_negative" ? -1 : 1);
                const int j = d == "up" || d == "down" ? 3 : 0;
                const double requested = current[j] + delta;
                target[j] = std::clamp(
                    requested,
                    yaw_adjust
                        ? kJoints[j].lower
                        : std::max(kJoints[j].lower, profile.home[j] + profile.ranges[j].min),
                    yaw_adjust
                        ? kJoints[j].upper
                        : std::min(kJoints[j].upper, profile.home[j] + profile.ranges[j].max));
                plan.limited = !Close(requested, target[j]);
                if (!Path(current, target, config.obstacles)) {
                    if (!Safe(current, config.obstacles))
                        throw std::runtime_error("Current pose geometry unsafe");
                    double lo = 0, hi = 1;
                    for (int i = 0; i < 18; i++) {
                        const double mid = (lo + hi) / 2;
                        if (Path(current, Blend(current, target, mid), config.obstacles))
                            lo = mid;
                        else
                            hi = mid;
                    }
                    target = Blend(current, target, lo);
                    plan.limited = true;
                }
            }
            if (!yaw_adjust && !InMode(target, profile))
                throw std::runtime_error("Target outside mode");
            route(target, mode, action == "adjust_pose" ? .6 : 1.8, true);
            plan.ok = true;
            return plan;
        }
        if (skill == "A15" && mode != 1)
            throw std::runtime_error("Attention switch requires stand mode");
        const auto* preset = Item(Item(config.source.get(), "action_presets"), skill.c_str());
        const auto* options = Item(step, "options");
        auto number = [&](const char* key, double fallback) {
            const auto* v = Item(preset, key);
            return cJSON_IsNumber(v) ? v->valuedouble : fallback;
        };
        auto choice = [&](const char* key, const char* fallback) {
            const auto s = Text(preset, key);
            return s.empty() ? std::string(fallback) : s;
        };
        const auto target = Text(step, "target_id");
        auto ref = current;
        if (!target.empty() && skill != "A03" && skill != "A15")
            ref = Aim(config, mode, target);
        plan.reference = ref;
        plan.prepare = ref;
        plan.prepare_duration = .4;
        const auto option_end = Text(options, "end_mode");
        const auto end_mode = dialogue             ? "return_to_reference"
                              : option_end.empty() ? choice("endMode", "hold_at_end")
                                                   : option_end;
        auto move = [&](Pose a, Pose b, double t) { plan.phases.push_back(Move(a, b, t)); };
        auto hold = [&](Pose p, double t) { plan.phases.push_back(Hold(p, t)); };
        if (skill == "A01") {
            Phase p = Hold(ref, 0);
            p.curve = Curve::Nod;
            p.amplitude = number("amplitudeDeg", 12) * kDeg;
            p.cycles = std::round(cJSON_IsNumber(Item(options, "repetitions"))
                                      ? Number(options, "repetitions")
                                      : number("repetitions", 2));
            p.period = number("periodSec", .65);
            p.gap = number("holdBetweenSec", .12);
            p.duration = p.cycles * p.period + (p.cycles - 1) * p.gap;
            plan.phases.push_back(p);
        } else if (skill == "A02" || skill == "B01") {
            Phase p = Hold(ref, 0);
            p.curve = skill == "A02" ? Curve::Tail : Curve::Wave;
            p.amplitude = number("amplitudeDeg", 6) * kDeg;
            p.cycles = std::round(number(skill == "A02" ? "cycles" : "repetitions", 2));
            p.duration = p.cycles * number("periodSec", .5);
            plan.phases.push_back(p);
        } else if (skill == "B02") {
            auto to = ref;
            to[4] += number("rollDeg", 18) * kDeg;
            move(ref, to, number("durationSec", .7));
            hold(to, number("holdSec", .8));
        } else if (skill == "A03") {
            auto to = ref;
            const double amount = std::min(.7, number("heightDropH", .05) * 5);
            to[1] += (1.56 - ref[1]) * amount;
            to[2] += (-1.56 - ref[2]) * amount;
            to[3] -= number("headPitchDeg", 20) * kDeg;
            move(ref, to, number("lowerSec", 1.2));
            hold(to, number("holdSec", 1));
            auto measured = to;
            for (int j = 0; j < 5; j++)
                measured[j] = std::clamp(measured[j], kJoints[j].lower, kJoints[j].upper);
            if (Forward(ref).tip.y - Forward(measured).tip.y + .002 <
                number("heightDropH", .05) * kHeight)
                plan.limited = true;
        } else if (skill == "A09" || skill == "A10") {
            plan.prepare_duration = number(skill == "A09" ? "turnSec" : "aimSec", .8);
            if (skill == "A09")
                hold(ref, number("holdSec", 1));
            else {
                plan.requires_light = true;
                hold(ref, std::max(.01, number("fadeSec", .2)));
                const auto hm = Text(options, "hold_mode");
                hold(ref, (hm.empty() ? choice("holdMode", "timed") : hm) == "holding"
                              ? INFINITY
                              : number("holdSec", .8));
            }
        } else if (skill == "A11") {
            auto to = ref;
            const auto side = choice("awaySide", "auto");
            const double sign = side == "left"              ? -1
                                : side == "right"           ? 1
                                : ref[0] >= profile.home[0] ? 1
                                                            : -1;
            to[0] += sign * number("yawOffsetDeg", 45) * kDeg;
            move(ref, to, number("turnSec", .6));
            hold(to, number("holdSec", 1));
        } else if (skill == "A13" || skill == "A14" || skill == "A16") {
            const auto f = Forward(ref);
            const double sign = skill == "A14" ? -1 : 1,
                         distance = (skill == "A16"
                                         ? .03
                                         : number(skill == "A13" ? "travelH" : "retreatH", .12)) *
                                    kHeight;
            auto to = Solve(f.tip + f.gaze * (sign * distance), f.gaze, ref);
            if (skill == "A16" && choice("mode", "approach") == "angle") {
                to = ref;
                to[3] -= 4 * kDeg;
            } else if ((Forward(to).tip - f.tip).Dot(f.gaze) * sign < distance - .003)
                plan.limited = true;
            if (skill == "A16") {
                Phase p = Move(ref, to, 0);
                p.curve = Curve::Pulse;
                p.cycles = std::round(number("pulses", 2));
                p.duration = p.cycles * number("pulsePeriodSec", .5);
                plan.phases.push_back(p);
                plan.requires_light = number("lightGain", 1.15) > 1;
            } else {
                move(ref, to, number("durationSec", .8));
                if (skill == "A14") {
                    const auto look = Text(options, "look_away");
                    const double deg = look == "small"    ? 10
                                       : look == "medium" ? 25
                                       : look == "large"  ? 40
                                                          : 0;
                    if (deg > 0) {
                        auto away = to;
                        away[0] += (ref[0] >= profile.home[0] ? 1 : -1) * deg * kDeg;
                        move(to, away, .4);
                    }
                }
            }
        } else if (skill == "A15") {
            plan.prepare = current;
            auto previous = current;
            for (int c = 0; c < std::round(number("cycles", 1)); c++)
                for (const auto* id : {"user", "book", "user"}) {
                    const auto q = Aim(config, mode, id);
                    move(previous, q, number("turnSec", .55));
                    hold(q, number("dwellSec", .4));
                    previous = q;
                }
        } else
            throw std::runtime_error("Primitive not implemented");
        if (!InMode(plan.prepare, profile) || !Path(current, plan.prepare, config.obstacles))
            throw std::runtime_error("Unsafe calibrated preparation");
        auto safe = [&](double scale) {
            auto previous = current;
            auto check = [&](const Pose& q) {
                if (!InMode(q, profile) || !Path(previous, q, config.obstacles, 2, .025))
                    return false;
                previous = q;
                return true;
            };
            if (!check(plan.prepare))
                return false;
            for (const auto& ph : plan.phases)
                for (int i = 0; i <= 48; i++)
                    if (!check(Blend(ref, ph.At(double(i) / 48), scale)))
                        return false;
            if (end_mode == "return_to_reference" && std::isfinite(plan.Duration()) &&
                !check(current))
                return false;
            return true;
        };
        // Dialogue plays the same primitive at reduced amplitude; the ceiling is
        // also the limit of the safety search, so a constrained path can only get
        // quieter, never louder than a mannerism.
        const double ceiling = dialogue
                                   ? (skill == "A01" ? kDialogueNodIntensity : kDialogueIntensity)
                                   : 1.;
        plan.scale = ceiling;
        if (!safe(ceiling)) {
            if (skill == "A09" || skill == "A10" || skill == "A15" || !safe(0))
                throw std::runtime_error("Calibrated route/motion blocked");
            double lo = 0, hi = ceiling;
            for (int i = 0; i < 12; i++) {
                const double mid = (lo + hi) / 2;
                if (safe(mid))
                    lo = mid;
                else
                    hi = mid;
            }
            plan.scale = lo * .995;
            if (plan.scale < .02)
                throw std::runtime_error("Insufficient mode motion margin");
            plan.limited = true;
        }
        if (end_mode == "return_to_reference" && std::isfinite(plan.Duration())) {
            auto last = Blend(ref, plan.phases.back().At(1), plan.scale);
            auto p = Move(last, current, .4);
            p.expressive = false;
            plan.phases.push_back(p);
        }
        if (plan.phases.size() > 32 || (std::isfinite(plan.Duration()) && plan.Duration() > 60))
            throw std::runtime_error("Plan exceeds bounded capacity/duration");
        plan.ok = true;
    } catch (const std::exception& e) {
        plan.ok = false;
        plan.error = e.what();
        plan.phases.clear();
    }
    return plan;
}
std::expected<std::array<int, 5>, std::string> ToTicks(const Configuration& config, const Pose& q) {
    if (!InUrdf(q))
        return std::unexpected("Refusing out-of-limit target");
    std::array<int, 5> ticks;
    const auto* servos = Item(Item(config.source.get(), "hardware"), "servos");
    for (int j = 0; j < 5; j++) {
        const auto* s = cJSON_GetArrayItem(servos, j);
        const double zero = Number(s, "zero_ticks"),
                     factor = Number(s, "direction") / (Number(s, "degrees_per_tick") * kDeg);
        const double a = zero + kJoints[j].lower * factor, b = zero + kJoints[j].upper * factor;
        const int lo = std::max(0, int(std::ceil(std::min(a, b)))),
                  hi = std::min(1000, int(std::floor(std::max(a, b))));
        if (lo > hi)
            return std::unexpected("Mapping has no representable range");
        const double ideal = zero + q[j] * factor;
        if (ideal < -1e-8 || ideal > 1000 + 1e-8)
            return std::unexpected(
                "Target outside physical servo travel after installation offset");
        ticks[j] = std::clamp(int(std::round(zero + q[j] * factor)), lo, hi);
    }
    return ticks;
}
}  // namespace lamp::pose
#endif
