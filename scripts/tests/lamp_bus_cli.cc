#include <cassert>
#include "lamp_bus_protocol.h"
using namespace lamp::bus;
int main() {
    const auto request = ReadRequest(1, 28);
    assert(request && *request == std::vector<uint8_t>({0x55, 0x55, 1, 3, 28, 0xdf}));
    for (unsigned cmd = 0; cmd < 256; cmd++) {
        auto frame = ReadRequest(1, cmd);
        assert(static_cast<bool>(frame) == (ReadLength(cmd) != 0));
    }
    assert(!ReadRequest(254, 28));
    assert(!ReadRequest(255, 28));
    Parser parser;
    const std::vector<uint8_t> packet = {0x55, 0x55, 1, 5, 28, 0xf4, 1, 0xe8};
    assert(parser.Push(std::span(packet).first(4)).empty());
    auto decoded = parser.Push(std::span(packet).subspan(4));
    assert(decoded.size() == 1 && decoded[0].id == 1 && decoded[0].command == 28);
    assert(Word(decoded[0].parameters) == 500);
    const uint8_t negative[] = {0xff, 0xff};
    assert(SignedWord(negative) == -1);
    auto bad = packet;
    bad.back()++;
    assert(parser.Push(bad).empty());
    const uint8_t noise[] = {0, 0xff, 0x55, 0};
    parser.Push(noise);
    decoded = parser.Push(packet);
    assert(decoded.size() == 1);
    std::vector<uint8_t> stream;
    for (unsigned i = 0; i < 200; i++)
        stream.insert(stream.end(), packet.begin(), packet.end());
    decoded = parser.Push(stream);
    assert(decoded.size() == 200);
    auto echo = parser.Push(*request);
    assert(echo.size() == 1 && echo[0].parameters.empty());
}
