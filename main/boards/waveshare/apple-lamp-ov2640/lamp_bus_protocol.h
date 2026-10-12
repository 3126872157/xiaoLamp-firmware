#pragma once
#include <cstdint>
#include <deque>
#include <expected>
#include <span>
#include <string>
#include <vector>
namespace lamp::bus {
struct Packet {
    uint8_t id, command;
    std::vector<uint8_t> parameters;
};
size_t ReadLength(uint8_t command);
std::expected<std::vector<uint8_t>, std::string> ReadRequest(uint8_t id, uint8_t command);
std::expected<std::vector<uint8_t>, std::string> WriteRequest(uint8_t id, uint8_t command,
                                                              std::span<const uint8_t> parameters);
class Parser {
    std::deque<uint8_t> bytes_;

public:
    std::vector<Packet> Push(std::span<const uint8_t> chunk);
    void Reset() { bytes_.clear(); }
};
int16_t SignedWord(std::span<const uint8_t> parameters);
uint16_t Word(std::span<const uint8_t> parameters);
}  // namespace lamp::bus
