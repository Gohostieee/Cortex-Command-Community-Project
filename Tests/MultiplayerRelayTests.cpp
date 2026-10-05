#include "MultiplayerTransport.h"
#include "MultiplayerRelay.h"
#include "MultiplayerWorldProtocol.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <random>
#include <filesystem>
#include <fstream>
#include <map>
#ifdef _WIN32
#include <process.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <signal.h>
#include <cerrno>
#include <unistd.h>
#endif
using namespace RTE::MP;
void Check(bool value, const char* what) { if (!value) throw std::runtime_error(what); }
uint64_t Time() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
template<class F> void Until(F fn, const char* what, uint64_t ms = 10000) { const auto deadline = Time() + ms; while (Time() < deadline) { if (fn()) return; std::this_thread::sleep_for(std::chrono::milliseconds(2)); } throw std::runtime_error(what); }
uint64_t ProcessID() {
#ifdef _WIN32
 return _getpid();
#else
 return getpid();
#endif
}
bool TestProcessAlive(uint64_t processID) {
#ifdef _WIN32
 const auto process = OpenProcess(SYNCHRONIZE, FALSE, DWORD(processID)); if (!process) return false;
 const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT; CloseHandle(process); return alive;
#else
 return kill(pid_t(processID), 0) == 0 || errno == EPERM;
#endif
}
std::map<std::string, std::string> ReadWorkerConfig(const std::filesystem::path& path) {
 std::ifstream file(path); Check(bool(file), "private worker config missing");
 std::map<std::string, std::string> values;
 for (std::string line; std::getline(file, line);) {
  if (!line.empty() && line.back() == '\r') line.pop_back();
  const auto separator = line.find('=');
  Check(separator != std::string::npos && values.emplace(line.substr(0, separator), line.substr(separator + 1)).second, "invalid private worker config");
 }
 return values;
}
// The broker launches this native child through the production worker path.
// It binds a real game socket and echoes packet bytes, without any game assets.
int FakeDedicatedWorker(const std::filesystem::path& path) {
#ifdef __linux__
 // Before opening the game socket, the child must not own the broker's socket.
 for (const auto& descriptor: std::filesystem::directory_iterator("/proc/self/fd")) {
  std::error_code linkError; const auto target = std::filesystem::read_symlink(descriptor.path(), linkError);
  if (!linkError) Check(target.string().find("socket:[") != 0, "worker inherited a broker network descriptor");
 }
#endif
 const auto values = ReadWorkerConfig(path);
 const auto port = static_cast<uint16_t>(std::stoi(values.at("port")));
 Check(values.at("owner_token").size() == Relay::TokenLength, "worker owner authorization missing");
 Check(!values.at("room_name").empty(), "worker room name missing");
 Check(std::stoi(values.at("idle_seconds")) > 0, "worker idle lifetime missing");
 const auto status = std::filesystem::path(values.at("status_file"));
 const auto stop = path.parent_path() / "exit-test-worker";
 std::error_code error; std::filesystem::remove(stop, error);
 std::ofstream(path.parent_path() / "test-process.txt", std::ios::trunc) << ProcessID();
 const auto started = Time();
 if (values.at("room_name") == "Startup timeout fixture") {
  while (Time() - started < 90000) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  return 0;
 }
 // Loading real assets takes time. The broker must wait for readiness before
 // telling a player to connect to this socket.
 std::this_thread::sleep_for(std::chrono::milliseconds(250));
 Transport game; std::string networkError;
 Check(game.Start(true, port, values.at("password"), networkError), "fake dedicated socket startup");
 std::ofstream(status, std::ios::trunc) << "ready=1\nconnected=0\nplaying=0\n";
 uint64_t lastHeartbeat = 0, ticks = 0;
 while (!std::filesystem::exists(stop) && Time() - started < 90000) {
  for (const auto& event: game.Poll()) if (event.Kind == TransportEvent::Type::Data) Check(game.Send(event.Address, event.Data, Delivery::Control), "fake dedicated echo");
  if (Time() - lastHeartbeat >= 20) { lastHeartbeat = Time(); std::ofstream(path.parent_path() / "test-heartbeat.txt", std::ios::trunc) << ++ticks; }
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
 }
 game.Stop(); return 0;
}
struct HostedHandoff { std::string Code, Endpoint; std::vector<uint8_t> Owner; std::string Error; };
HostedHandoff HostedRequest(uint16_t port, const std::string& endpoint, bool create, const std::string& code, const std::string& password, const std::string& name, bool accepted) {
 Transport connection; std::string error;
 Check(connection.Start(false, 0, "", error) && connection.ConnectRelay(create, endpoint, port, code, password, error, create, name), "hosted broker connection start");
 HostedHandoff result; bool done = false;
 Until([&] {
  for (const auto& event: connection.Poll()) {
   if (event.Kind == TransportEvent::Type::HostedRoom) { Check(accepted, "unauthorized hosted request accepted"); result = {connection.RoomCode(), event.Address, event.Data}; done = true; }
   else if (event.Kind == TransportEvent::Type::Failed) { Check(!accepted, "hosted broker rejected valid request"); result.Error = event.Error; done = true; }
  }
  return done;
 }, "hosted broker did not answer", 20000);
 if (accepted) Check(!Relay::NormalizeCode(result.Code).empty() && !result.Endpoint.empty(), "invalid hosted endpoint handoff");
 return result;
}
std::filesystem::path FindWorkerConfig(const std::filesystem::path& root) {
 if (!std::filesystem::is_directory(root)) return {};
 for (const auto& directory: std::filesystem::directory_iterator(root)) {
  if (directory.is_directory() && std::filesystem::exists(directory.path() / "server.ini")) return directory.path() / "server.ini";
 }
 return {};
}
void HostedBrokerTests(uint16_t port, const std::string& endpoint, const std::filesystem::path& stateDirectory) {
 const std::string password = "native fixture;$(unused)", name = "Native room; $(unused) & friends";
 HostedRequest(port, endpoint, true, "", "", "Invalid\nroom", false);
 auto creator = HostedRequest(port, endpoint, true, "", password, name, true);
 Check(creator.Owner.size() == Relay::TokenLength, "creator owner credential missing");
 const auto config = FindWorkerConfig(stateDirectory); Check(!config.empty(), "broker did not spawn a native worker");
 const auto values = ReadWorkerConfig(config);
 { std::ifstream status(values.at("status_file")); std::string readiness; std::getline(status, readiness); Check(readiness == "ready=1", "broker handed off a dedicated worker before it was ready"); }
 Check(values.at("room_name") == name && values.at("password") == password && values.at("owner_token") == std::string(creator.Owner.begin(), creator.Owner.end()), "broker altered private worker configuration");
 Check(creator.Endpoint == "127.0.0.1:" + values.at("port"), "broker handed off wrong worker endpoint");
#ifndef _WIN32
 const auto permissions = std::filesystem::status(config).permissions();
 Check((permissions & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) == std::filesystem::perms::none, "worker credentials are not private");
#endif
 HostedRequest(port, endpoint, false, creator.Code, "wrong-password", "", false);
 for (int i = 0; i < 3; ++i) {
  const auto guest = HostedRequest(port, endpoint, false, Relay::DisplayCode(creator.Code), password, "", true);
  Check(guest.Endpoint == creator.Endpoint && guest.Code == creator.Code && guest.Owner.empty(), "guest received owner authorization or wrong endpoint");
 }
 HostedRequest(port, endpoint, true, "", "", "Capacity fixture", false);
 // Hosted admission does not consume or interfere with legacy relay rooms.
 Transport legacy; std::string error;
 Check(legacy.Start(false, 0, "", error) && legacy.ConnectRelay(true, endpoint, port, "", "", error), "legacy compatibility connection");
 Until([&] { legacy.Poll(); return legacy.IsRelayReady(); }, "hosted capacity blocked legacy relay"); legacy.Stop();
 const auto separator = creator.Endpoint.rfind(':');
 Check(separator != std::string::npos, "hosted endpoint port missing");
 const auto gamePort = static_cast<uint16_t>(std::stoi(creator.Endpoint.substr(separator + 1)));
 const auto gameHost = creator.Endpoint.substr(0, separator);
 { Transport unauthorized; Check(unauthorized.Start(false, 0, "", error) && unauthorized.Connect(gameHost, gamePort, "wrong-password", error), "dedicated password rejection connection"); bool denied = false; Until([&] { for (const auto& event: unauthorized.Poll()) if (event.Kind == TransportEvent::Type::Failed) denied = true; return denied; }, "dedicated worker accepted wrong password"); }
 std::array<Transport, 4> players; std::array<bool, 4> connected{};
 for (auto& player: players) Check(player.Start(false, 0, "", error) && player.Connect(gameHost, gamePort, password, error), "direct hosted player connection");
 Until([&] { for (int i = 0; i < 4; ++i) for (const auto& event: players[i].Poll()) if (event.Kind == TransportEvent::Type::Connected) connected[i] = true; return std::all_of(connected.begin(), connected.end(), [](bool value) { return value; }); }, "dedicated worker did not admit four humans");
 std::array<std::vector<uint8_t>, 4> inputs; std::array<bool, 4> echoed{};
 for (int i = 0; i < 4; ++i) { Writer marker(Kind::Input, 345, 1); marker.U32(700 + i); inputs[i] = marker.Data; Check(players[i].Send(creator.Endpoint, inputs[i], Delivery::Control), "hosted input send"); }
 Until([&] { for (int i = 0; i < 4; ++i) for (const auto& event: players[i].Poll()) if (event.Kind == TransportEvent::Type::Data) { Check(event.Data == inputs[i], "dedicated worker crossed human inputs"); echoed[i] = true; } return std::all_of(echoed.begin(), echoed.end(), [](bool value) { return value; }); }, "dedicated worker did not process all four human inputs");
 players[0].Stop();
 // The room creator has no authority over the child process's lifetime.
 const auto afterLeave = HostedRequest(port, endpoint, false, creator.Code, password, "", true);
 Check(afterLeave.Endpoint == creator.Endpoint && afterLeave.Owner.empty(), "creator disconnect destroyed hosted invitation");
 Writer continued(Kind::Input, 345, 1); continued.U32(999);
 Check(players[1].Send(creator.Endpoint, continued.Data, Delivery::Control), "hosted survivor input send");
 bool survived = false;
 Until([&] { for (const auto& event: players[1].Poll()) if (event.Kind == TransportEvent::Type::Data && event.Data == continued.Data) survived = true; players[2].Poll(); players[3].Poll(); return survived; }, "match logic stopped after creator disconnect");
 for (auto& player: players) player.Stop();
 std::ofstream(config.parent_path() / "exit-test-worker") << "exit\n";
 Until([&] { return !std::filesystem::exists(config); }, "exited worker private credentials were retained");
 HostedRequest(port, endpoint, false, creator.Code, password, "", false);
 const auto replacement = HostedRequest(port, endpoint, true, "", "", "Replacement fixture", true);
 Check(replacement.Code != creator.Code, "exited room invitation code reused");
 Check(replacement.Owner.size() == Relay::TokenLength && replacement.Owner != creator.Owner, "replacement worker reused owner authorization");
 const auto replacementConfig = FindWorkerConfig(stateDirectory); Check(!replacementConfig.empty(), "worker capacity did not recover");
 std::ofstream(replacementConfig.parent_path() / "exit-test-worker") << "exit\n";
 Until([&] { return !std::filesystem::exists(replacementConfig); }, "replacement worker credentials retained");
 std::cout << "PASS: actual dedicated child spawn, private configuration, password and owner handoff, hosted capacity, legacy compatibility, four remote humans, creator disconnect survival, worker exit cleanup and capacity recovery\n";
}
void HostedUnavailableTests(uint16_t port, const std::string& endpoint, const std::filesystem::path& stateDirectory) {
 const auto failed = HostedRequest(port, endpoint, true, "", "native unavailable fixture", "Unavailable fixture", false);
 Check(failed.Error == "All AWS match servers are busy. Try again shortly.", "failed worker launch did not receive the broker's launch rejection");
 Until([&] { return FindWorkerConfig(stateDirectory).empty(); }, "failed worker launch retained private credentials");
 std::cout << "PASS: missing dedicated executable is rejected and private launch credentials are removed\n";
}
void HostedLifetimeFixture(uint16_t port, const std::string& endpoint, const std::filesystem::path& stateDirectory) {
 HostedRequest(port, endpoint, true, "", "", "Process lifetime fixture", true);
 const auto config = FindWorkerConfig(stateDirectory); Check(!config.empty(), "lifetime worker configuration missing");
 std::ifstream process(config.parent_path() / "test-process.txt"); uint64_t processId = 0; process >> processId;
 Check(processId != 0, "lifetime worker process identifier missing");
 std::ofstream(stateDirectory / "test-process.txt", std::ios::trunc) << processId;
 std::cout << "PASS: active dedicated worker prepared for abrupt broker shutdown verification\n";
}
void HostedStartupTimeoutTests(uint16_t port, const std::string& endpoint, const std::filesystem::path& stateDirectory) {
 const auto failed = HostedRequest(port, endpoint, true, "", "", "Startup timeout fixture", false);
 Check(failed.Error == "AWS match server could not start. Please create a new room.", "stalled startup did not receive the broker's worker startup failure");
 Until([&] { return FindWorkerConfig(stateDirectory).empty(); }, "stalled startup retained private credentials");
 uint64_t processID = 0;
 for (const auto& directory: std::filesystem::directory_iterator(stateDirectory)) if (directory.is_directory()) { std::ifstream process(directory.path() / "test-process.txt"); if (process >> processID) break; }
 Check(processID != 0, "stalled startup did not launch its fixture child");
 Until([&] { return !TestProcessAlive(processID); }, "stalled startup child process was not terminated");
 HostedRequest(port, endpoint, true, "", "", "Replacement after timeout", true);
 const auto config = FindWorkerConfig(stateDirectory); Check(!config.empty(), "stalled startup permanently consumed worker capacity");
 std::ofstream(config.parent_path() / "exit-test-worker") << "exit\n";
 Until([&] { return !std::filesystem::exists(config); }, "post-timeout replacement retained private credentials");
 std::cout << "PASS: stalled dedicated startup terminates its child, invalidates the room, removes private credentials and restores worker capacity\n";
}
int main(int argc, char** argv) { try {
 if (argc == 3 && std::string(argv[1]) == "-mp-dedicated") return FakeDedicatedWorker(argv[2]);
 if (argc == 5 && std::string(argv[1]) == "--hosted") { HostedBrokerTests(static_cast<uint16_t>(std::stoi(argv[2])), argv[3], argv[4]); return 0; }
 if (argc == 5 && std::string(argv[1]) == "--hosted-unavailable") { HostedUnavailableTests(static_cast<uint16_t>(std::stoi(argv[2])), argv[3], argv[4]); return 0; }
 if (argc == 5 && std::string(argv[1]) == "--hosted-lifetime") { HostedLifetimeFixture(static_cast<uint16_t>(std::stoi(argv[2])), argv[3], argv[4]); return 0; }
 if (argc == 5 && std::string(argv[1]) == "--hosted-startup-timeout") { HostedStartupTimeoutTests(static_cast<uint16_t>(std::stoi(argv[2])), argv[3], argv[4]); return 0; }
 const auto port = static_cast<uint16_t>(argc > 1 ? std::stoi(argv[1]) : 38997);
 const std::string endpoint = argc > 2 ? argv[2] : "127.0.0.1";
 Check(Relay::NormalizeCode("abcde-f2345") == "ABCDEF2345", "code normalization");
 Check(Relay::NormalizeCode("ABCDEFGHIJ").empty(), "ambiguous character rejected");
 Writer current(Kind::WorldSnapshot, 123, 1); current.Data.resize(Relay::MaxPayload - Relay::CarrierHeaderBytes, 42);
 auto carrier = Relay::WrapPayload(current.Data); auto unwrapped = Relay::UnwrapPayload(carrier);
 Check(carrier.size() == Relay::MaxPayload && carrier[5] == 0 && carrier[6] == 2 && carrier[7] == uint8_t(Kind::Hello), "carrier rejected by deployed v1 broker");
 Check(std::equal(unwrapped.begin(), unwrapped.end(), current.Data.begin(), current.Data.end()), "relay carrier changed native state");
 for (size_t size = 0; size < Relay::CarrierHeaderBytes + 20; ++size) Check(Relay::UnwrapPayload(std::span(carrier).first(size)).empty(), "truncated carrier accepted");
 current.Data.push_back(42); Check(Relay::WrapPayload(current.Data).empty(), "oversized relay carrier accepted");
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
 // Reliable control and native state traverse the relay unchanged, including payloads near MTU.
 std::array<size_t, 3> controlBytes{};
 for (int i = 0; i < 3; ++i) { Writer data(Kind::Chat, 123, 1); std::vector<uint8_t> payload(1300, static_cast<uint8_t>(40 + i)); data.Bytes(payload); Check(host.Send(Relay::PeerAddress(static_cast<uint8_t>(guests[i].RelaySlot())), data.Data, Delivery::Control), "host send"); }
 Until([&] { host.Poll(); for (int i = 0; i < 3; ++i) for (auto& event: guests[i].Poll()) if (event.Kind == TransportEvent::Type::Data) { Check(event.Data.size() == 1320 && event.Data.back() == 40 + i && event.Address == "relay:0", "relay modified or crossed payload"); controlBytes[i] += event.Data.size(); } return controlBytes[0] && controlBytes[1] && controlBytes[2]; }, "relay reliable delivery");
 std::array<size_t, 3> frameBytes{}; const auto began = Time();
 Until([&] { host.Poll(); if (Time() - began < 3000) for (int i = 0; i < 3; ++i) { Writer frame(Kind::WorldSnapshot, 123, 1); std::vector<uint8_t> payload(1100, static_cast<uint8_t>(60 + i)); frame.Bytes(payload); host.Send(Relay::PeerAddress(static_cast<uint8_t>(guests[i].RelaySlot())), frame.Data, Delivery::State); } for (int i = 0; i < 3; ++i) for (auto& event: guests[i].Poll()) if (event.Kind == TransportEvent::Type::Data) { Check(event.Data.back() == 60 + i, "state crossed rooms/players"); frameBytes[i] += event.Data.size(); } return frameBytes[0] >= 100000 && frameBytes[1] >= 100000 && frameBytes[2] >= 100000; }, "relay state delivery", 5000);
 // Initial terrain and sprite resources use the v1 service's reliable lane.
 std::vector<uint8_t> resource(135983, 77); std::array<World::Assembler, 3> assemblers; std::array<bool, 3> resources{};
 for (int i = 0; i < 3; ++i) for (size_t offset = 0; offset < resource.size(); offset += ChunkBytes) {
  Writer packet(Kind::WorldResource, 123, 1); World::WriteChunk(packet, {1, uint32_t(resource.size()), uint16_t(offset / ChunkBytes), std::span(resource).subspan(offset, std::min<size_t>(ChunkBytes, resource.size() - offset))});
  Check(host.Send(Relay::PeerAddress(uint8_t(guests[i].RelaySlot())), packet.Data, Delivery::WorldResource), "relay resource send");
 }
 Until([&] { host.Poll(); for (int i = 0; i < 3; ++i) for (const auto& event : guests[i].Poll()) if (event.Kind == TransportEvent::Type::Data) {
  Reader reader(event.Data); Header header; World::Chunk chunk;
  if (ReadHeader(reader, header) && header.Type == Kind::WorldResource && World::ReadChunk(reader, chunk)) if (auto complete = assemblers[i].Push(chunk, Time())) resources[i] = *complete == resource;
 } return resources[0] && resources[1] && resources[2]; }, "relay retained resource delivery");
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
