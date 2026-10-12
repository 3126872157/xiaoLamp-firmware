#pragma once
#include <string>

// Compile-time vocabulary of the Omni lamp tools, shared by the Omni realtime protocol
// and the device-side tool table. A stored tool table may add names, but only by
// referring to an operation declared here; nothing in this file is configurable at runtime.
namespace lamp {
inline bool KnownOperation(const std::string& operation) {
    return operation == "execute_group" || operation == "cancel" || operation == "get_task" ||
           operation == "set_torque" || operation == "record_calibration" || operation == "rest" ||
           operation == "get_state";
}
inline std::string ToolOperation(const std::string& name) {
    if (name == "lamp_run_sequence" || name == "lamp_set_mode" ||
        name == "lamp_prepare_expression")
        return "execute_group";
    if (name == "lamp_stop")
        return "cancel";
    if (name == "lamp_get_task")
        return "get_task";
    if (name == "lamp_set_torque")
        return "set_torque";
    if (name == "lamp_record_calibration")
        return "record_calibration";
    if (name == "lamp_rest")
        return "rest";
    if (name == "lamp_get_state")
        return "get_state";
    return "";
}
// Expression tools have playback-synchronised special cases in the runtime and protocol.
inline bool ExpressionTool(const std::string& name) {
    return name == "lamp_express" || name == "lamp_prepare_expression";
}
// Operations that move the mechanism or hold/release torque.
inline bool MotionOperation(const std::string& operation) {
    return operation == "execute_group" || operation == "rest";
}
// Motion plus the operations a single-response stop must suppress.
inline bool ControlOperation(const std::string& operation) {
    return MotionOperation(operation) || operation == "record_calibration" ||
           operation == "set_torque";
}
}  // namespace lamp
