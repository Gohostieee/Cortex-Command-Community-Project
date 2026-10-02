#include "MultiplayerTransport.h"

#include "RakPeerInterface.h"
#include "RakNetStatistics.h"
#include "MessageIdentifiers.h"
#include "MultiplayerProtocol.h"
#include "MultiplayerRelay.h"
#include <chrono>

namespace RTE::MP {
namespace { RakNet::SystemAddress ParseAddress(const std::string& text) { RakNet::SystemAddress address; address.FromString(text.c_str(), ':', 4); return address; } }
namespace { uint64_t Milliseconds() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); } }
struct Transport::Impl {
	RakNet::RakPeerInterface* Peer = nullptr;
	bool Relay = false, HostRoom = false, Ready = false, Retrying = false;
	std::string Service, Central, Code, Token, Password;
	uint16_t Port = 8001;
	int Slot = -1;
	uint64_t Started = 0, RetryUntil = 0, NextRetry = 0;
	std::array<bool, 4> Peers{};
	void Request() {
		Relay::Writer w(HostRoom ? Relay::Kind::Create : Relay::Kind::Join); w.Text(Code, Relay::CodeLength); w.Text(Password, 63); w.Text(Token, Relay::TokenLength);
		Peer->Send(reinterpret_cast<const char*>(w.Data.data()), static_cast<int>(w.Data.size()), HIGH_PRIORITY, RELIABLE_ORDERED, 0, ParseAddress(Central), false);
	}
};
Transport::Transport(): m_Impl(std::make_unique<Impl>()) {}
Transport::~Transport() { Stop(); }

bool Transport::Start(bool host, uint16_t port, const std::string& password, std::string& error) {
	Stop(); m_Impl->Peer = RakNet::RakPeerInterface::GetInstance();
	RakNet::SocketDescriptor socket(host ? port : 0, nullptr); socket.socketFamily = AF_INET;
	if (m_Impl->Peer->Startup(8, &socket, 1) != RakNet::RAKNET_STARTED) { error = "Unable to open the UDP port. Another host may already be using it."; Stop(); return false; }
	m_Impl->Peer->SetMaximumIncomingConnections(host ? 3 : 0);
	m_Impl->Peer->SetIncomingPassword(password.data(), static_cast<int>(password.size()));
	m_Impl->Peer->SetOccasionalPing(true); m_Impl->Peer->SetTimeoutTime(5000, RakNet::UNASSIGNED_SYSTEM_ADDRESS);
	m_Impl->Peer->SetUnreliableTimeout(0); return true;
}
bool Transport::Connect(const std::string& host, uint16_t port, const std::string& password, std::string& error) {
	if (!m_Impl->Peer || m_Impl->Peer->Connect(host.c_str(), port, password.data(), static_cast<int>(password.size())) != RakNet::CONNECTION_ATTEMPT_STARTED) { error = "Could not connect. Check the host address and port."; return false; }
	return true;
}
bool Transport::ConnectRelay(bool hostRoom, const std::string& service, uint16_t port, const std::string& code, const std::string& password, std::string& error) {
	auto& impl = *m_Impl;
	if (!impl.Peer || (!hostRoom && Relay::NormalizeCode(code).empty())) { error = "Enter the ten-character room code."; return false; }
	impl.Relay = true; impl.HostRoom = hostRoom; impl.Ready = false; impl.Service = service; impl.Port = port; impl.Code = hostRoom ? "" : Relay::NormalizeCode(code); impl.Token.clear(); impl.Password = password; impl.Started = Milliseconds();
	return Connect(service, port, "", error);
}
bool Transport::ReconnectRelay(std::string& error) {
	auto& impl = *m_Impl; if (!impl.Relay) return false;
	if (impl.Peer) { impl.Peer->Shutdown(0); RakNet::RakPeerInterface::DestroyInstance(impl.Peer); }
	impl.Peer = RakNet::RakPeerInterface::GetInstance(); impl.Central.clear(); impl.Ready = false; impl.Peers = {}; impl.Started = Milliseconds();
	RakNet::SocketDescriptor socket(0, nullptr); socket.socketFamily = AF_INET;
	if (impl.Peer->Startup(8, &socket, 1) != RakNet::RAKNET_STARTED) { error = "Unable to open a connection to the room service."; return false; }
	impl.Peer->SetTimeoutTime(5000, RakNet::UNASSIGNED_SYSTEM_ADDRESS); impl.Peer->SetUnreliableTimeout(0); impl.Peer->SetOccasionalPing(true);
	return Connect(impl.Service, impl.Port, "", error);
}
bool Transport::IsRelayReady() const { return m_Impl->Relay && m_Impl->Ready; }
int Transport::RelaySlot() const { return m_Impl->Relay && m_Impl->Ready ? m_Impl->Slot : -1; }
std::string Transport::RoomCode() const { return m_Impl->Code; }
void Transport::Stop() {
	if (m_Impl->Peer) {
		if (m_Impl->Relay && m_Impl->Ready) { Relay::Writer w(Relay::Kind::Leave); m_Impl->Peer->Send(reinterpret_cast<const char*>(w.Data.data()), static_cast<int>(w.Data.size()), HIGH_PRIORITY, RELIABLE_ORDERED, 0, ParseAddress(m_Impl->Central), false); }
		m_Impl->Peer->Shutdown(100); RakNet::RakPeerInterface::DestroyInstance(m_Impl->Peer); m_Impl->Peer = nullptr;
	}
	m_Impl->Relay = m_Impl->Ready = m_Impl->Retrying = false; m_Impl->Code.clear(); m_Impl->Token.clear(); m_Impl->Peers = {};
}
std::vector<TransportEvent> Transport::Poll(size_t budget) {
	std::vector<TransportEvent> events;
	if (!m_Impl->Peer) return events;
	auto& impl = *m_Impl; const auto now = Milliseconds();
	if (impl.Relay && impl.HostRoom && impl.Retrying) {
		if (now > impl.RetryUntil) { events.push_back({TransportEvent::Type::Failed, {}, "The room service is unavailable. Create a new room when it returns.", {}}); impl.Retrying = false; return events; }
		if (now >= impl.NextRetry) { std::string error; ReconnectRelay(error); impl.NextRetry = now + 4000; }
	}
	for (size_t i = 0; i < budget; ++i) {
		RakNet::Packet* packet = m_Impl->Peer->Receive(); if (!packet) break;
		TransportEvent event; event.Address = packet->systemAddress.ToString(true, ':'); bool deliver = true;
		if (packet->length < 1) deliver = false;
		else switch (packet->data[0]) {
			case ID_NEW_INCOMING_CONNECTION: case ID_CONNECTION_REQUEST_ACCEPTED: event.Kind = TransportEvent::Type::Connected; break;
			case ID_DISCONNECTION_NOTIFICATION: case ID_CONNECTION_LOST: event.Kind = TransportEvent::Type::Disconnected; break;
			case ID_CONNECTION_ATTEMPT_FAILED: event.Kind = TransportEvent::Type::Failed; event.Error = "Host did not respond. Check the address, UDP port forwarding, and firewall."; break;
			case ID_INVALID_PASSWORD: event.Kind = TransportEvent::Type::Failed; event.Error = "Incorrect room password."; break;
			case ID_NO_FREE_INCOMING_CONNECTIONS: event.Kind = TransportEvent::Type::Failed; event.Error = "This room is full."; break;
			case ID_INCOMPATIBLE_PROTOCOL_VERSION: event.Kind = TransportEvent::Type::Failed; event.Error = "Host uses an incompatible networking version."; break;
			case ID_CONNECTION_BANNED: event.Kind = TransportEvent::Type::Failed; event.Error = "The host refused this connection."; break;
			case ID_UNCONNECTED_PONG: {
				constexpr size_t offset = 1 + sizeof(RakNet::TimeMS);
				if (packet->length > offset && packet->length <= offset + 512) { event.Kind = TransportEvent::Type::Discovered; event.Data.assign(packet->data + offset, packet->data + packet->length); } else deliver = false;
				break;
			}
			case PacketID: if (packet->length <= 1400) { event.Kind = TransportEvent::Type::Data; event.Data.assign(packet->data, packet->data + packet->length); } else deliver = false; break;
			default: deliver = false; break;
		}
		if (impl.Relay && packet->length) {
			if (packet->data[0] == ID_CONNECTION_REQUEST_ACCEPTED) { impl.Central = event.Address; impl.Request(); deliver = false; }
			else if (packet->data[0] == Relay::PacketID && event.Address == impl.Central && packet->length <= Relay::MaxPacket) {
				deliver = false; Reader reader({packet->data, packet->length}); Relay::Kind kind;
				if (Relay::Header(reader, kind)) {
					if (kind == Relay::Kind::Accepted) {
						std::string code, token; uint8_t slot;
						if (reader.Text(code, Relay::CodeLength) && !Relay::NormalizeCode(code).empty() && reader.Text(token, Relay::TokenLength) && token.size() == Relay::TokenLength && reader.U8(slot) && slot <= 3 && (impl.HostRoom ? slot == 0 : slot > 0) && reader.Done()) { impl.Code = code; impl.Token = token; impl.Slot = slot; impl.Ready = true; impl.Retrying = false; events.push_back({TransportEvent::Type::RoomCode, code, {}, {}}); }
					} else if (kind == Relay::Kind::PeerUp || kind == Relay::Kind::PeerDown) {
						uint8_t slot;
						if (impl.Ready && reader.U8(slot) && slot <= 3 && (impl.HostRoom ? slot > 0 : slot == 0) && reader.Done()) { const bool up = kind == Relay::Kind::PeerUp; if (impl.Peers[slot] != up) { impl.Peers[slot] = up; events.push_back({up ? TransportEvent::Type::Connected : TransportEvent::Type::Disconnected, Relay::PeerAddress(slot), {}, {}}); } }
					} else if (kind == Relay::Kind::Route) {
						uint8_t slot, delivery;
						if (impl.Ready && reader.U8(slot) && slot <= 3 && impl.Peers[slot] && reader.U8(delivery) && delivery <= 3 && reader.Remaining() <= 1400 && reader.Remaining() >= 20 && reader.Rest()[0] == MP::PacketID) { TransportEvent data{TransportEvent::Type::Data, Relay::PeerAddress(slot), {}, {}}; data.Data.assign(reader.Rest().begin(), reader.Rest().end()); events.push_back(std::move(data)); }
					} else if (kind == Relay::Kind::Error) { std::string message; if (reader.Text(message) && reader.Done()) events.push_back({TransportEvent::Type::Failed, impl.Central, message, {}}); }
				}
			} else if (event.Kind == TransportEvent::Type::Disconnected && event.Address == impl.Central) {
				for (uint8_t slot = 0; slot < 4; ++slot) if (impl.Peers[slot]) events.push_back({TransportEvent::Type::Disconnected, Relay::PeerAddress(slot), {}, {}});
				impl.Peers = {}; impl.Ready = false; deliver = false;
				if (impl.HostRoom) { impl.Retrying = true; impl.RetryUntil = now + 20000; impl.NextRetry = now + 1000; events.push_back({TransportEvent::Type::ServiceStatus, {}, "Room service interrupted. Reconnecting...", {}}); }
			} else if (event.Kind == TransportEvent::Type::Failed) { event.Error = "Cannot reach the room service. Check the service address and your connection."; if (impl.HostRoom && impl.Retrying) deliver = false; }
			else deliver = false;
		}
		m_Impl->Peer->DeallocatePacket(packet); if (deliver) events.push_back(std::move(event));
	}
	if (impl.Relay && !impl.Ready && !impl.Retrying && now - impl.Started > 10000) { events.push_back({TransportEvent::Type::Failed, {}, "Room service connection timed out.", {}}); impl.Started = now; }
	return events;
}
bool Transport::Send(const std::string& address, std::span<const uint8_t> data, Delivery delivery) {
	if (!m_Impl->Peer || data.empty() || data.size() > 1400) return false;
	std::vector<uint8_t> routed;
	int slot = Relay::PeerSlot(address);
	if (slot >= 0) { if (!m_Impl->Ready || !m_Impl->Peers[slot]) return false; Relay::Writer w(Relay::Kind::Route); w.U8(static_cast<uint8_t>(slot)); w.U8(static_cast<uint8_t>(delivery)); w.Bytes(data); routed = std::move(w.Data); data = routed; }
	PacketReliability reliability = RELIABLE_ORDERED; PacketPriority priority = HIGH_PRIORITY; char channel = 0;
	if (delivery == Delivery::Input) { reliability = UNRELIABLE_SEQUENCED; priority = IMMEDIATE_PRIORITY; channel = 2; }
	else if (delivery == Delivery::Frame) { reliability = UNRELIABLE; priority = MEDIUM_PRIORITY; channel = 1; }
	else if (delivery == Delivery::Audio) { priority = MEDIUM_PRIORITY; channel = 3; }
	if (slot >= 0) channel = delivery == Delivery::Control ? 0 : static_cast<char>(static_cast<int>(delivery) * 4 + slot);
	return m_Impl->Peer->Send(reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), priority, reliability, channel, ParseAddress(slot >= 0 ? m_Impl->Central : address), false) != 0;
}
void Transport::Close(const std::string& address) {
	if (!m_Impl->Peer) return; const int slot = Relay::PeerSlot(address);
	if (slot >= 0 && m_Impl->Relay) { if (m_Impl->HostRoom && slot) { Relay::Writer w(Relay::Kind::Drop); w.U8(static_cast<uint8_t>(slot)); m_Impl->Peer->Send(reinterpret_cast<const char*>(w.Data.data()), static_cast<int>(w.Data.size()), HIGH_PRIORITY, RELIABLE_ORDERED, 0, ParseAddress(m_Impl->Central), false); } }
	else m_Impl->Peer->CloseConnection(ParseAddress(address), true);
}
void Transport::Advertise(std::span<const uint8_t> data) { if (m_Impl->Peer && data.size() <= 512) m_Impl->Peer->SetOfflinePingResponse(reinterpret_cast<const char*>(data.data()), static_cast<unsigned>(data.size())); }
void Transport::Discover(uint16_t port, const std::string& host) { if (m_Impl->Peer) m_Impl->Peer->Ping(host.c_str(), port, true); }
int Transport::Ping(const std::string& address) const { return m_Impl->Peer ? m_Impl->Peer->GetAveragePing(ParseAddress(Relay::PeerSlot(address) >= 0 ? m_Impl->Central : address)) : -1; }
std::vector<std::string> Transport::LocalAddresses(uint16_t port) const {
	std::vector<std::string> addresses; if (!m_Impl->Peer) return addresses;
	for (unsigned i = 0; i < m_Impl->Peer->GetNumberOfAddresses(); ++i) { const std::string ip = m_Impl->Peer->GetLocalIP(i); if (ip != "127.0.0.1" && ip.find(':') == std::string::npos) addresses.push_back(ip + ":" + std::to_string(port)); }
	return addresses;
}
uint64_t Transport::QueuedBytes(const std::string& address) const {
	if (!m_Impl->Peer) return 0;
	RakNet::RakNetStatistics stats{};
	if (!m_Impl->Peer->GetStatistics(ParseAddress(Relay::PeerSlot(address) >= 0 ? m_Impl->Central : address), &stats)) return 0;
	uint64_t bytes = 0; for (auto size: stats.bytesInSendBuffer) bytes += static_cast<uint64_t>(size);
	// RakNet reports the entire service link. Game budgets are per guest; use
	// each stream's share, while the service bounds each guest's actual queue.
	if (m_Impl->Relay && m_Impl->HostRoom && Relay::PeerSlot(address) >= 0) { const auto peers = std::count(m_Impl->Peers.begin() + 1, m_Impl->Peers.end(), true); if (peers > 0) bytes /= static_cast<uint64_t>(peers); }
	return bytes;
}
std::string Transport::Diagnostics(const std::string& address) const {
	RakNet::RakNetStatistics stats{}; if (!m_Impl->Peer || !m_Impl->Peer->GetStatistics(ParseAddress(Relay::PeerSlot(address) >= 0 ? m_Impl->Central : address), &stats)) return "no statistics";
	return "sent=" + std::to_string(stats.runningTotal[RakNet::USER_MESSAGE_BYTES_SENT]) + " received=" + std::to_string(stats.runningTotal[RakNet::USER_MESSAGE_BYTES_RECEIVED_PROCESSED]) + " queued=" + std::to_string(QueuedBytes(address)) + " congestion=" + std::to_string(stats.BPSLimitByCongestionControl) + " loss=" + std::to_string(stats.packetlossLastSecond);
}
} // namespace RTE::MP
