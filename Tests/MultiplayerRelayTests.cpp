#include "MultiplayerTransport.h"
#include "MultiplayerRelay.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <random>
using namespace RTE::MP;
void Check(bool value, const char* what) { if (!value) throw std::runtime_error(what); }
uint64_t Time() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
template<class F> void Until(F fn, const char* what, uint64_t ms = 10000) { const auto deadline = Time() + ms; while (Time() < deadline) { if (fn()) return; std::this_thread::sleep_for(std::chrono::milliseconds(2)); } throw std::runtime_error(what); }
int main(int argc, char** argv) { try {
 const auto port = static_cast<uint16_t>(argc > 1 ? std::stoi(argv[1]) : 38997);
 const std::string endpoint = argc > 2 ? argv[2] : "127.0.0.1";
 Check(Relay::NormalizeCode("abcde-f2345") == "ABCDEF2345", "code normalization");
 Check(Relay::NormalizeCode("ABCDEFGHIJ").empty(), "ambiguous character rejected");
 std::mt19937 random(45);
 for (int i = 0; i < 10000; ++i) { std::vector<uint8_t> bytes(random() % 100); for (auto& b: bytes) b = static_cast<uint8_t>(random()); Reader reader(bytes); Relay::Kind kind; if (Relay::Header(reader, kind)) Check(bytes.size() >= 8, "short relay header accepted"); }
 Transport host; std::array<Transport, 3> guests; std::string error, code;
 // Every socket is outgoing-only; none of the four game peers accepts incoming connections.
 Check(host.Start(false, 0, "", error) && host.ConnectRelay(true, endpoint, port, "", "test-secret", error), "host service connection");
 Until([&] { for (auto& event: host.Poll()) if (event.Kind == TransportEvent::Type::RoomCode) code = event.Address; return !code.empty(); }, "room code not issued");
 Check(Relay::NormalizeCode(code) == code, "server issued invalid code");
 auto denied = [&](const std::string& room, const std::string& password) { Transport bad; Check(bad.Start(false, 0, "", error) && bad.ConnectRelay(false, endpoint, port, room, password, error), "negative connection start"); bool failed = false; Until([&] { host.Poll(); for (auto& event: bad.Poll()) if (event.Kind == TransportEvent::Type::Failed) failed = true; return failed; }, "unauthorized join accepted"); };
 denied(code, "wrong-password"); denied("AAAAAAAAAA", "test-secret");
 std::array<bool, 3> connected{}; std::array<bool, 4> hostConnected{};
 for (auto& guest: guests) Check(guest.Start(false, 0, "", error) && guest.ConnectRelay(false, endpoint, port, Relay::DisplayCode(code), "test-secret", error), "guest service connection");
 Until([&] { for (auto& event: host.Poll()) if (event.Kind == TransportEvent::Type::Connected) hostConnected[Relay::PeerSlot(event.Address)] = true; for (int i = 0; i < 3; ++i) for (auto& event: guests[i].Poll()) if (event.Kind == TransportEvent::Type::Connected) connected[i] = true; return std::all_of(connected.begin(), connected.end(), [](bool v) { return v; }) && hostConnected[1] && hostConnected[2] && hostConnected[3]; }, "four-player room failed");
 denied(code, "test-secret");
 for (int i = 0; i < 3; ++i) { Writer input(Kind::Input, 123, 1); input.U32(99 + guests[i].RelaySlot()); Check(guests[i].Send("relay:0", input.Data, Delivery::Input), "guest input send"); }
 std::array<bool, 4> inputs{};
 Until([&] { for (auto& event: host.Poll()) if (event.Kind == TransportEvent::Type::Data) { const int slot = Relay::PeerSlot(event.Address); Reader reader(event.Data); Header header; uint32_t value; Check(ReadHeader(reader, header) && reader.U32(value) && reader.Done() && value == 99 + slot, "input crossed player channels"); inputs[slot] = true; } for (auto& guest: guests) guest.Poll(); return inputs[1] && inputs[2] && inputs[3]; }, "simultaneous sequenced input lost");
 // Same reliable control and frame data traverses the relay unchanged, including payloads near MTU.
 std::array<size_t, 3> controlBytes{};
 for (int i = 0; i < 3; ++i) { Writer data(Kind::Chat, 123, 1); std::vector<uint8_t> payload(1300, static_cast<uint8_t>(40 + i)); data.Bytes(payload); Check(host.Send(Relay::PeerAddress(static_cast<uint8_t>(guests[i].RelaySlot())), data.Data, Delivery::Control), "host send"); }
 Until([&] { host.Poll(); for (int i = 0; i < 3; ++i) for (auto& event: guests[i].Poll()) if (event.Kind == TransportEvent::Type::Data) { Check(event.Data.size() == 1320 && event.Data.back() == 40 + i && event.Address == "relay:0", "relay modified or crossed payload"); controlBytes[i] += event.Data.size(); } return controlBytes[0] && controlBytes[1] && controlBytes[2]; }, "relay reliable delivery");
 std::array<size_t, 3> frameBytes{}; const auto began = Time();
 Until([&] { host.Poll(); if (Time() - began < 3000) for (int i = 0; i < 3; ++i) { Writer frame(Kind::Frame, 123, 1); std::vector<uint8_t> payload(1100, static_cast<uint8_t>(60 + i)); frame.Bytes(payload); host.Send(Relay::PeerAddress(static_cast<uint8_t>(guests[i].RelaySlot())), frame.Data, Delivery::Frame); } for (int i = 0; i < 3; ++i) for (auto& event: guests[i].Poll()) if (event.Kind == TransportEvent::Type::Data) { Check(event.Data.back() == 60 + i, "frame crossed rooms/players"); frameBytes[i] += event.Data.size(); } return frameBytes[0] >= 100000 && frameBytes[1] >= 100000 && frameBytes[2] >= 100000; }, "relay frame delivery", 5000);
 const auto guestAddress = Relay::PeerAddress(static_cast<uint8_t>(guests[0].RelaySlot())); bool down = false; host.Close(guestAddress);
 Until([&] { host.Poll(); for (auto& event: guests[0].Poll()) if (event.Kind == TransportEvent::Type::Disconnected) down = true; guests[1].Poll(); guests[2].Poll(); return down; }, "guest disconnect not forwarded");
 Check(guests[0].ReconnectRelay(error), "guest reconnect start"); bool rejoined = false, hostRejoined = false;
 Until([&] { for (auto& event: host.Poll()) if (event.Kind == TransportEvent::Type::Connected && event.Address == guestAddress) hostRejoined = true; for (auto& event: guests[0].Poll()) if (event.Kind == TransportEvent::Type::Connected) rejoined = true; guests[1].Poll(); guests[2].Poll(); return rejoined && hostRejoined; }, "reserved relay player did not reconnect");
 Check(guests[0].RoomCode() == code, "room code changed on reconnect");
 // A host can resume with its secret before the old socket has timed out.
 Check(host.ReconnectRelay(error), "host resume start"); std::array<bool, 3> hostResume{};
 Until([&] { for (auto& event: host.Poll()) if (event.Kind == TransportEvent::Type::Connected) { const int slot = Relay::PeerSlot(event.Address); if (slot > 0) hostResume[slot - 1] = true; } for (auto& guest: guests) guest.Poll(); return host.IsRelayReady() && hostResume[0] && hostResume[1] && hostResume[2]; }, "host room did not resume");
 Check(host.RoomCode() == code, "host resume changed invitation code");
 // Another room cannot route to this room even though its logical player IDs match.
 Transport otherHost, otherGuest; Check(otherHost.Start(false, 0, "", error) && otherHost.ConnectRelay(true, endpoint, port, "", "", error), "second host start");
 Until([&] { otherHost.Poll(); host.Poll(); for (auto& guest: guests) guest.Poll(); return otherHost.IsRelayReady(); }, "second room creation");
 Check(otherGuest.Start(false, 0, "", error) && otherGuest.ConnectRelay(false, endpoint, port, otherHost.RoomCode(), "", error), "second guest start"); bool otherConnected = false;
 Until([&] { otherHost.Poll(); for (auto& event: otherGuest.Poll()) if (event.Kind == TransportEvent::Type::Connected) otherConnected = true; host.Poll(); for (auto& guest: guests) guest.Poll(); return otherConnected; }, "second room join");
 Writer marker(Kind::Chat); marker.Text("isolated room"); otherGuest.Send("relay:0", marker.Data, Delivery::Control); bool isolated = false;
 auto IsMarker = [&](const TransportEvent& event) { return event.Kind == TransportEvent::Type::Data && event.Data == marker.Data; };
 Until([&] { for (auto& event: otherHost.Poll()) if (IsMarker(event)) isolated = true; for (auto& event: host.Poll()) Check(!IsMarker(event), "cross-room relay leak"); for (auto& guest: guests) for (auto& event: guest.Poll()) Check(!IsMarker(event), "cross-room guest leak"); return isolated; }, "isolated route missing");
 otherGuest.Stop(); otherHost.Stop();
 host.Stop(); std::array<bool, 3> closed{};
 Until([&] { for (int i = 0; i < 3; ++i) for (auto& event: guests[i].Poll()) if (event.Kind == TransportEvent::Type::Failed || event.Kind == TransportEvent::Type::Disconnected) closed[i] = true; return closed[0] && closed[1] && closed[2]; }, "host close did not close room");
 for (auto& guest: guests) guest.Stop(); denied(code, "test-secret");
 std::cout << "PASS: invitation codes, passwords, capacity, three independent inputs and streams, exact payloads, reconnect, room isolation, and host closure; all game sockets outgoing-only\n";
 return 0;
 } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
