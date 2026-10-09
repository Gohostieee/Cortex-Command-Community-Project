#include "MultiplayerTransport.h"
#include <chrono>
#include <iostream>
#include <thread>

int main(int argc, char** argv) {
    if (argc != 2) return 4;
    RTE::MP::Transport transport;
    std::string error;
    if (!transport.Start(false, 0, "", error) || !transport.ConnectRelay(true, argv[1], 8001, "", "", error, true, "Capacity benchmark")) {
        std::cout << "FAIL: " << error << '\n';
        return 4;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto& event : transport.Poll()) {
            if (event.Kind == RTE::MP::TransportEvent::Type::HostedRoom) {
                std::cout << "FAIL: third hosted room was admitted while both workers were occupied\n";
                return 1;
            }
            if (event.Kind == RTE::MP::TransportEvent::Type::Failed) {
                const bool busy = event.Error == "All AWS match servers are busy. Try again shortly.";
                std::cout << (busy ? "PASS: " : "FAIL: ") << event.Error << '\n';
                return busy ? 0 : 2;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::cout << "FAIL: capacity request timed out\n";
    return 3;
}
