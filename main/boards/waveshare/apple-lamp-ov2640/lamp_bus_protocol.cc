#include "lamp_bus_protocol.h"
namespace lamp::bus {
std::expected<std::vector<uint8_t>, std::string> WriteRequest(uint8_t id, uint8_t cmd,
                                                              std::span<const uint8_t> p) {
    if (id < 1 || id > 5 ||
        !((cmd == 1 && p.size() == 4 && Word(p) <= 1000 && Word(p.subspan(2)) <= 30000) ||
          (cmd == 12 && p.empty()) ||
          (cmd == 29 && p.size() == 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 0) ||
          (cmd == 31 && p.size() == 1 && p[0] <= 1)))
        return std::unexpected("Unsupported hardware write; no presets/broadcast/motor mode");
    std::vector<uint8_t> bytes = {0x55, 0x55, id, static_cast<uint8_t>(p.size() + 3), cmd};
    bytes.insert(bytes.end(), p.begin(), p.end());
    uint8_t sum = 0;
    for (size_t i = 2; i < bytes.size(); i++)
        sum += bytes[i];
    bytes.push_back(static_cast<uint8_t>(~sum));
    return bytes;
}
size_t ReadLength(uint8_t command) {
    switch (command) {
        case 14:
        case 26:
        case 32:
            return 1;
        case 27:
        case 28:
            return 2;
        case 21:
        case 30:
            return 4;
        default:
            return 0;
    }
}
std::expected<std::vector<uint8_t>, std::string> ReadRequest(uint8_t id, uint8_t command) {
    if (id > 253 || !ReadLength(command))
        return std::unexpected(
            "Read-only probe refuses write, preset, start, broadcast or unknown command");
    const auto sum = static_cast<uint8_t>(id + 3 + command);
    return std::vector<uint8_t>{0x55, 0x55, id, 3, command, static_cast<uint8_t>(~sum)};
}
std::vector<Packet> Parser::Push(std::span<const uint8_t> chunk) {
    std::vector<Packet> result;
    for (const uint8_t byte : chunk) {
        bytes_.push_back(byte);
        while (bytes_.size() >= 6) {
            if (bytes_[0] != 0x55 || bytes_[1] != 0x55 || bytes_[2] > 253 || bytes_[3] < 3 ||
                bytes_[3] > 32) {
                bytes_.pop_front();
                continue;
            }
            const size_t count = bytes_[3] + 3;
            if (bytes_.size() < count)
                break;
            uint8_t sum = 0;
            for (size_t i = 2; i < count; i++)
                sum += bytes_[i];
            if (sum != 0xff) {
                bytes_.pop_front();
                continue;
            }
            Packet packet{bytes_[2], bytes_[4], {}};
            for (size_t i = 5; i + 1 < count; i++)
                packet.parameters.push_back(bytes_[i]);
            result.push_back(std::move(packet));
            for (size_t i = 0; i < count; i++)
                bytes_.pop_front();
        }
    }
    return result;
}
uint16_t Word(std::span<const uint8_t> p) {
    return p.size() >= 2 ? p[0] | (static_cast<uint16_t>(p[1]) << 8) : 0;
}
int16_t SignedWord(std::span<const uint8_t> p) { return static_cast<int16_t>(Word(p)); }
}  // namespace lamp::bus
