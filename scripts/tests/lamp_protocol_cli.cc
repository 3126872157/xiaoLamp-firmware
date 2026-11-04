#include <iostream>
#include <iterator>
#include "lamp_protocol.h"

int main(int argc, char** argv) {
    if (argc != 2)
        return 2;
    const std::string input{std::istreambuf_iterator<char>(std::cin), {}};
    const std::string mode = argv[1];
    if (mode == "summary") {
        CJsonUniquePtr root(cJSON_Parse(input.c_str()));
        if (!root)
            return 1;
        std::cout << lamp::Summarize(root.get()) << '\n';
        return 0;
    }
    if (mode == "catalog") {
        // Page 0 is the capability summary; pages 1..13 are one primitive each.
        for (int i = 0; i <= 13; ++i)
            std::cout << lamp::Catalog(i) << '\n';
        return 0;
    }
    auto result = mode == "sequence" ? lamp::Sequence(input) : lamp::Command(input);
    if (!result) {
        std::cerr << result.error() << '\n';
        return 1;
    }
    std::cout << *result << '\n';
}
