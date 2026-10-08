#pragma once
#include <map>
#include <string>
class Settings {
public:
    static inline std::map<std::string, std::map<std::string, std::string>> data;
    Settings(const std::string& ns, bool = false) : ns_(ns) {}
    std::string GetString(const std::string& k) { return data[ns_][k]; }
    void SetString(const std::string& k, const std::string& v) { data[ns_][k] = v; }
    void EraseAll() { data[ns_].clear(); }

private:
    std::string ns_;
};
