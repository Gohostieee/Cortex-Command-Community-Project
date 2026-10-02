// Standalone invitation-code broker and bounded room relay. No game assets or GPU.
#include "MultiplayerRelay.h"
#include "RakPeerInterface.h"
#include "RakNetStatistics.h"
#include "MessageIdentifiers.h"
#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <thread>
#include <unordered_map>

using namespace RTE::MP;
namespace R = RTE::MP::Relay;
using Clock = std::chrono::steady_clock;
uint64_t Now() { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count(); }
volatile std::sig_atomic_t Running = 1;
void Stop(int) { Running = 0; }
struct Bucket {
 double Credit = 0; uint64_t Last = 0;
 bool Take(double amount, double rate, uint64_t now) {
  if (!Last) Credit = rate;
  else Credit = std::min(rate, Credit + (now - Last) * rate / 1000.0);
  Last = now; if (Credit < amount) return false; Credit -= amount; return true;
 }
};
struct Member { std::string Address, Token; uint64_t Until = 0; };
struct Room { std::array<Member, 4> Members; std::string Password, OwnerIP; };
struct Connection { std::string IP, Code; uint8_t Slot = 255; uint64_t Since = Now(); Bucket Control, Bytes; };
class Service {
 RakNet::RakPeerInterface* Peer = RakNet::RakPeerInterface::GetInstance();
 std::map<std::string, Room> Rooms;
 std::unordered_map<std::string, Connection> Connections;
 struct IPLimit { Bucket Attempts; uint64_t Last = 0; };
 std::unordered_map<std::string, IPLimit> IPs;
 std::random_device Random;
 Bucket Global;
 size_t MaxRooms = 100;
 double BytesPerSecond = 100000000;
 uint64_t Relayed = 0, Dropped = 0, NextMetrics = 0;
 std::string MetricsPath;
 RakNet::SystemAddress Address(const std::string& value) { RakNet::SystemAddress a; a.FromString(value.c_str(), ':', 4); return a; }
 std::string RandomText(size_t size, const char* alphabet) { std::string result; const size_t count = std::char_traits<char>::length(alphabet); for (size_t i = 0; i < size; ++i) result += alphabet[Random() % count]; return result; }
 uint64_t Queued(const std::string& address) { RakNet::RakNetStatistics stats{}; if (!Peer->GetStatistics(Address(address), &stats)) return 0; uint64_t size = 0; for (auto value: stats.bytesInSendBuffer) size += static_cast<uint64_t>(value); return size; }
 bool Send(const std::string& address, const std::vector<uint8_t>& data, uint8_t delivery = 0, uint8_t lane = 0) {
  if (address.empty()) return false;
  const auto queued = Queued(address);
  if (delivery == 2 && queued > 262144) { ++Dropped; return false; }
  if (queued > 2097152) { Peer->CloseConnection(Address(address), true); ++Dropped; return false; }
  PacketReliability reliability = RELIABLE_ORDERED; PacketPriority priority = HIGH_PRIORITY;
  // Independent ordered/sequenced lanes for every routed peer. A host has three
  // streams on a single service connection; sharing a sequenced lane loses input.
  char channel = delivery == 0 ? 0 : static_cast<char>(delivery * 4 + lane);
  if (delivery == 1) { reliability = UNRELIABLE_SEQUENCED; priority = IMMEDIATE_PRIORITY; }
  else if (delivery == 2) { reliability = UNRELIABLE; priority = MEDIUM_PRIORITY; }
  else if (delivery == 3) priority = MEDIUM_PRIORITY;
  return Peer->Send(reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), priority, reliability, channel, Address(address), false) != 0;
 }
 void Error(const std::string& address, const char* message) { R::Writer w(R::Kind::Error); w.Text(message); Send(address, w.Data); }
 void Notify(const std::string& address, R::Kind kind, uint8_t slot) { R::Writer w(kind); w.U8(slot); Send(address, w.Data); }
 void EraseRoom(const std::string& code) {
  auto it = Rooms.find(code); if (it == Rooms.end()) return;
  for (auto& member: it->second.Members) if (!member.Address.empty()) { auto connection = Connections.find(member.Address); if (connection != Connections.end()) { connection->second.Code.clear(); connection->second.Slot = 255; } Error(member.Address, "The host closed this room."); Peer->CloseConnection(Address(member.Address), true); }
  Rooms.erase(it);
 }
 void Detach(const std::string& address, bool retain) {
  auto it = Connections.find(address); if (it == Connections.end() || it->second.Code.empty()) return;
  const auto code = it->second.Code; const auto slot = it->second.Slot;
  it->second.Code.clear(); it->second.Slot = 255;
  auto roomIt = Rooms.find(code); if (roomIt == Rooms.end()) return;
  if (!slot && !retain) { EraseRoom(code); return; }
  auto& room = roomIt->second; auto& member = room.Members[slot];
  member.Address.clear(); member.Until = Now() + 60000;
  if (!retain) member = {};
  if (!slot) { for (int i = 1; i < 4; ++i) Notify(room.Members[i].Address, R::Kind::PeerDown, 0); }
  else Notify(room.Members[0].Address, R::Kind::PeerDown, slot);
 }
 void Accept(const std::string& address, Connection& connection, const std::string& code, uint8_t slot) {
  auto& room = Rooms.at(code); auto& member = room.Members[slot];
  member.Address = address; member.Until = 0;
  if (member.Token.empty()) member.Token = RandomText(R::TokenLength, "0123456789abcdef");
  connection.Code = code; connection.Slot = slot;
  R::Writer accepted(R::Kind::Accepted); accepted.Text(code, R::CodeLength); accepted.Text(member.Token, R::TokenLength); accepted.U8(slot); Send(address, accepted.Data);
  if (!slot) { for (uint8_t i = 1; i < 4; ++i) if (!room.Members[i].Address.empty()) { Notify(address, R::Kind::PeerUp, i); Notify(room.Members[i].Address, R::Kind::PeerUp, 0); } }
  else { Notify(room.Members[0].Address, R::Kind::PeerUp, slot); Notify(address, R::Kind::PeerUp, 0); }
 }
 void Receive(const std::string& address, std::span<const uint8_t> bytes) {
  auto it = Connections.find(address); if (it == Connections.end() || bytes.size() > R::MaxPacket) return;
  auto& connection = it->second; Reader reader(bytes); R::Kind kind;
  if (!R::Header(reader, kind)) { ++Dropped; return; }
  const auto now = Now();
  if (kind == R::Kind::Route) {
   uint8_t destination, delivery;
   if (connection.Code.empty() || !reader.U8(destination) || destination > 3 || !reader.U8(delivery) || delivery > 3 || reader.Remaining() < 20 || reader.Remaining() > 1400 || (connection.Slot == 0 ? destination == 0 : destination != 0)) { ++Dropped; return; }
   Reader payload(reader.Rest()); Header header;
   if (!ReadHeader(payload, header)) { ++Dropped; return; }
   auto room = Rooms.find(connection.Code); if (room == Rooms.end()) return;
   const auto& target = room->second.Members[destination].Address;
   if (target.empty()) return;
   if (!connection.Bytes.Take(bytes.size(), 20000000, now) || !Global.Take(bytes.size(), BytesPerSecond, now)) { ++Dropped; return; }
   R::Writer routed(R::Kind::Route); routed.U8(connection.Slot); routed.U8(delivery); routed.Bytes(reader.Rest());
   if (Send(target, routed.Data, delivery, connection.Slot ? connection.Slot : destination)) Relayed += reader.Remaining();
   return;
  }
  if (!connection.Control.Take(1, 10, now)) { ++Dropped; return; }
  if (kind == R::Kind::Create || kind == R::Kind::Join) {
   std::string code, password, token;
   if (!reader.Text(code, R::CodeLength) || !reader.Text(password, 63) || !reader.Text(token, R::TokenLength) || !reader.Done() || !connection.Code.empty()) { Error(address, "Invalid room request."); return; }
   auto& limit = IPs[connection.IP]; limit.Last = now;
   if (!limit.Attempts.Take(1, 8, now)) { Error(address, "Too many requests. Try again shortly."); return; }
   if (kind == R::Kind::Create) {
    if (!code.empty() || !token.empty()) {
     auto room = Rooms.find(code);
     if (token.size() != R::TokenLength || room == Rooms.end() || room->second.Members[0].Token != token) { Error(address, "The room expired. Create a new room."); return; }
     // A valid resume secret supersedes a stale socket immediately, including
     // the interval before RakNet detects a network outage on the old address.
     if (!room->second.Members[0].Address.empty()) { const auto old = room->second.Members[0].Address; Detach(old, true); Peer->CloseConnection(Address(old), true); }
    } else {
     size_t owned = 0; for (const auto& [key, room]: Rooms) if (room.OwnerIP == connection.IP) ++owned;
     if (Rooms.size() >= MaxRooms || owned >= 5) { Error(address, "The service is at room capacity. Try again later."); return; }
     do code = RandomText(R::CodeLength, R::Alphabet); while (Rooms.contains(code));
     Rooms[code].Password = password; Rooms[code].OwnerIP = connection.IP;
    }
    Accept(address, connection, code, 0);
   } else {
    auto roomIt = Rooms.find(code);
    if (roomIt == Rooms.end() || roomIt->second.Password != password) { Error(address, "Room code or password is incorrect, or the room has expired."); return; }
    auto& room = roomIt->second;
    if (room.Members[0].Address.empty()) { Error(address, "The host is reconnecting. Try again shortly."); return; }
    uint8_t slot = 255;
    if (!token.empty()) for (uint8_t i = 1; i < 4; ++i) if (room.Members[i].Token == token) { slot = i; if (!room.Members[i].Address.empty()) { const auto old = room.Members[i].Address; Detach(old, true); Peer->CloseConnection(Address(old), true); } break; }
    if (slot == 255) for (uint8_t i = 1; i < 4; ++i) if (room.Members[i].Token.empty()) { slot = i; break; }
    if (slot == 255) { Error(address, "This room is full. Disconnected slots are reserved for 60 seconds."); return; }
    Accept(address, connection, code, slot);
   }
  } else if (kind == R::Kind::Leave && reader.Done()) { Detach(address, false); Peer->CloseConnection(Address(address), true); }
  else if (kind == R::Kind::Drop) {
   uint8_t slot;
   if (connection.Slot != 0 || !reader.U8(slot) || slot < 1 || slot > 3 || !reader.Done() || !Rooms.contains(connection.Code)) return;
   auto& member = Rooms.at(connection.Code).Members[slot];
   if (!member.Address.empty()) { const auto guest = member.Address; Detach(guest, true); Peer->CloseConnection(Address(guest), true); }
   else member = {}; // Releasing an already disconnected lobby slot.
  }
 }
public:
 ~Service() { Peer->Shutdown(200); RakNet::RakPeerInterface::DestroyInstance(Peer); }
 bool Start(uint16_t port, const std::string& bind, size_t maxRooms, double mbps, const std::string& metrics) {
  MaxRooms = maxRooms; BytesPerSecond = mbps * 125000; MetricsPath = metrics;
  RakNet::SocketDescriptor socket(port, bind.c_str()); socket.socketFamily = AF_INET;
  const auto maxConnections = static_cast<unsigned>(std::min<size_t>(4096, MaxRooms * 4 + 32));
  if (Peer->Startup(maxConnections, &socket, 1) != RakNet::RAKNET_STARTED) return false;
  Peer->SetMaximumIncomingConnections(static_cast<unsigned short>(maxConnections)); Peer->SetTimeoutTime(5000, RakNet::UNASSIGNED_SYSTEM_ADDRESS); Peer->SetUnreliableTimeout(0); Peer->SetOccasionalPing(true);
  std::cout << "Room service ready on UDP " << port << "; max_rooms=" << MaxRooms << "; relay_mbps=" << mbps << std::endl; return true;
 }
 void Tick() {
  for (size_t i = 0; i < 8192; ++i) {
   auto* packet = Peer->Receive(); if (!packet) break;
   const std::string address = packet->systemAddress.ToString(true, ':');
   if (packet->length) switch (packet->data[0]) {
    case ID_NEW_INCOMING_CONNECTION: {
     const std::string ip = packet->systemAddress.ToString(false); size_t count = 0;
     for (const auto& [key, connection]: Connections) if (connection.IP == ip) ++count;
     if (count >= 24 || (IPs.size() >= 8192 && !IPs.contains(ip))) Peer->CloseConnection(packet->systemAddress, true);
     else { Connections[address] = {}; Connections[address].IP = ip; }
     break;
    }
    case ID_DISCONNECTION_NOTIFICATION: case ID_CONNECTION_LOST: Detach(address, true); Connections.erase(address); break;
    case R::PacketID: Receive(address, {packet->data, packet->length}); break;
    default: break;
   }
   Peer->DeallocatePacket(packet);
  }
  const auto now = Now();
  // A connected but unregistered socket gets ten seconds, preventing idle peers
  // from occupying all slots indefinitely. Rooms survive host outages for a minute.
  for (auto it = Connections.begin(); it != Connections.end();) { if (it->second.Code.empty() && now - it->second.Since > 10000) { Peer->CloseConnection(Address(it->first), true); it = Connections.erase(it); } else ++it; }
  std::vector<std::string> expired;
  for (auto& [code, room]: Rooms) { if (room.Members[0].Address.empty() && now >= room.Members[0].Until) expired.push_back(code); else for (int i = 1; i < 4; ++i) if (room.Members[i].Address.empty() && now >= room.Members[i].Until) room.Members[i] = {}; }
  for (const auto& code: expired) EraseRoom(code);
  std::erase_if(IPs, [now](const auto& item) { return now - item.second.Last > 60000; });
  if (now >= NextMetrics) {
   NextMetrics = now + 10000;
   const std::string json = "{\"rooms\":" + std::to_string(Rooms.size()) + ",\"connections\":" + std::to_string(Connections.size()) + ",\"relayed_bytes\":" + std::to_string(Relayed) + ",\"dropped_packets\":" + std::to_string(Dropped) + "}";
   std::cout << json << std::endl;
   if (!MetricsPath.empty()) { std::ofstream file(MetricsPath, std::ios::trunc); file << json << '\n'; }
  }
 }
};
int main(int argc, char** argv) {
 uint16_t port = 8001; std::string bind = "0.0.0.0", metrics; size_t rooms = 100; double mbps = 800;
 try { for (int i = 1; i < argc; ++i) {
  const std::string arg = argv[i]; if (arg == "--help") { std::cout << "cc-room-service [--port 8001] [--bind 0.0.0.0] [--max-rooms 100] [--max-mbps 800] [--metrics-file path]\n"; return 0; }
  if (i + 1 == argc) throw std::runtime_error("Missing option value"); const std::string value = argv[++i];
  if (arg == "--port") { const int n = std::stoi(value); if (n < 1 || n > 65535) throw std::runtime_error("Invalid port"); port = static_cast<uint16_t>(n); }
  else if (arg == "--bind") bind = value;
  else if (arg == "--max-rooms") { rooms = std::stoul(value); if (!rooms || rooms > 1000) throw std::runtime_error("Invalid room limit"); }
  else if (arg == "--max-mbps") { mbps = std::stod(value); if (!std::isfinite(mbps) || mbps < 1 || mbps > 10000) throw std::runtime_error("Invalid bandwidth limit"); }
  else if (arg == "--metrics-file") metrics = value;
  else throw std::runtime_error("Unknown option: " + arg);
 } } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 2; }
 std::signal(SIGINT, Stop); std::signal(SIGTERM, Stop);
 Service service; if (!service.Start(port, bind, rooms, mbps, metrics)) { std::cerr << "Unable to bind UDP service port.\n"; return 1; }
 while (Running) { service.Tick(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
 std::cout << "Room service stopped.\n"; return 0;
}
