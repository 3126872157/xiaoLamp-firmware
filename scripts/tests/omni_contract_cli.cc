#include <iostream>
#include <iterator>
#include "omni_contract.h"
int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "session") {
        std::cout << omni::Session();
        return 0;
    }
    std::string input(std::istreambuf_iterator<char>(std::cin), {});
    cJSON* j = cJSON_Parse(input.c_str());
    auto str = [&](const char* key) {
        auto v = cJSON_GetObjectItem(j, key);
        return std::string(cJSON_IsString(v) ? v->valuestring : "");
    };
    bool valid = argc == 2 && std::string(argv[1]) == "credentials"
                     ? omni::Credentials(str("workspace"), str("key"), str("region"))
                     : omni::ToolArguments(str("name"), str("arguments"));
    cJSON_Delete(j);
    std::cout << (valid ? "true" : "false");
}
