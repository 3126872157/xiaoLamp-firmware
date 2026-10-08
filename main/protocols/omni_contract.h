#pragma once
#include <cJSON.h>
#include <cctype>
#include <memory>
#include <string>

namespace omni {
inline bool Identifier(const std::string& value) {
    if (value.empty() || value.size() > 128)
        return false;
    for (unsigned char c : value)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_'))
            return false;
    return true;
}
inline bool Credentials(const std::string& workspace, const std::string& key,
                        const std::string& region) {
    return Identifier(workspace) && key.size() >= 8 && key.size() <= 512 &&
           key.find_first_of("\r\n\t ") == std::string::npos &&
           (region == "cn-beijing" || region == "ap-southeast-1");
}
inline bool StateArguments(const std::string& name, const std::string& arguments) {
    if (name != "lamp_get_state" || arguments.size() > 256)
        return false;
    const char* end = nullptr;
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> j(
        cJSON_ParseWithOpts(arguments.c_str(), &end, true), cJSON_Delete);
    return cJSON_IsObject(j.get()) && !j->child;
}
// Current 3.8 protocol, not the old Opus/JSON XiaoZhi hello protocol.
inline std::string Session() {
    return R"({"type":"session.update","session":{"modalities":["text","audio"],"input_audio_transcription":null,"turn_detection":null,"audio":{"input":{"format":{"type":"pcm","sample_rate":16000,"sample_format":"s16le","channels":1,"packing":"interleaved","channel_layout":"mono"}},"output":{"voice":"Tina","format":{"type":"pcm","sample_rate":24000}}},"instructions":"你是台灯机器人的语音助手。用简短自然的中文回答。当前是语音接入实验，仅允许查询状态，不具备动作调用能力，不得声称已经运动。询问台灯状态时调用 lamp_get_state。不要反复查询。","tools":[{"type":"function","function":{"name":"lamp_get_state","description":"读取本地台灯缓存状态；不运动、不使能。","parameters":{"type":"object","properties":{},"additionalProperties":false}}}]}})";
}
}  // namespace omni
