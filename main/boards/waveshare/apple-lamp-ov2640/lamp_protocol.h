#pragma once

#include "cjson_utils.h"

#include <expected>
#include <string>

namespace lamp {
inline constexpr const char* kProtocol = "lamp-action/2";
inline constexpr size_t kMaxCommandBytes = 24576;

std::string Encode(const cJSON* value);
std::string Text(const cJSON* object, const char* key);
bool ValidId(const std::string& id);
std::string UrlEncode(const std::string& value);
// Validates semantic input against the imported Web catalog. Never resolves joint parameters.
std::expected<std::string, std::string> Sequence(const std::string& input);
std::expected<std::string, std::string> Command(const std::string& input);
std::string Query(const char* operation, const std::string& request_id,
                  const std::string& task_id = "");
std::string Catalog(int cursor, const std::string& skill_id = "");
std::string Summarize(const cJSON* response);
}  // namespace lamp
