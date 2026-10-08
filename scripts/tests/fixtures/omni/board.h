#pragma once
#include <memory>
#include "web_socket.h"
struct FakeNetwork {
    std::unique_ptr<WebSocket> CreateWebSocket(int) { return std::make_unique<WebSocket>(); }
};
class Board {
public:
    static Board& GetInstance() {
        static Board b;
        return b;
    }
    FakeNetwork* GetNetwork() { return &network; }
    void RecordDiagnostic(const std::string&, const std::string&, const std::string&) {}
    FakeNetwork network;
};
