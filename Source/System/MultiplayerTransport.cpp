#include "MultiplayerTransport.h"

#include "RakPeerInterface.h"
#include "RakNetStatistics.h"
#include "MessageIdentifiers.h"
#include "MultiplayerProtocol.h"

namespace RTE::MP {
namespace { RakNet::SystemAddress ParseAddress(const std::string& text) { RakNet::SystemAddress address; address.FromString(text.c_str(), ':', 4); return address; } }
struct Transport::Impl { RakNet::RakPeerInterface* Peer = nullptr; };
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
void Transport::Stop() {
	if (m_Impl->Peer) { m_Impl->Peer->Shutdown(100); RakNet::RakPeerInterface::DestroyInstance(m_Impl->Peer); m_Impl->Peer = nullptr; }
}
std::vector<TransportEvent> Transport::Poll(size_t budget) {
	std::vector<TransportEvent> events;
	if (!m_Impl->Peer) return events;
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
		m_Impl->Peer->DeallocatePacket(packet); if (deliver) events.push_back(std::move(event));
	}
	return events;
}
bool Transport::Send(const std::string& address, std::span<const uint8_t> data, Delivery delivery) {
	if (!m_Impl->Peer || data.empty() || data.size() > 1400) return false;
	PacketReliability reliability = RELIABLE_ORDERED; PacketPriority priority = HIGH_PRIORITY; char channel = 0;
	if (delivery == Delivery::Input) { reliability = UNRELIABLE_SEQUENCED; priority = IMMEDIATE_PRIORITY; channel = 2; }
	else if (delivery == Delivery::Frame) { reliability = UNRELIABLE; priority = MEDIUM_PRIORITY; channel = 1; }
	else if (delivery == Delivery::Audio) { priority = MEDIUM_PRIORITY; channel = 3; }
	return m_Impl->Peer->Send(reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), priority, reliability, channel, ParseAddress(address), false) != 0;
}
void Transport::Close(const std::string& address) { if (m_Impl->Peer) m_Impl->Peer->CloseConnection(ParseAddress(address), true); }
void Transport::Advertise(std::span<const uint8_t> data) { if (m_Impl->Peer && data.size() <= 512) m_Impl->Peer->SetOfflinePingResponse(reinterpret_cast<const char*>(data.data()), static_cast<unsigned>(data.size())); }
void Transport::Discover(uint16_t port, const std::string& host) { if (m_Impl->Peer) m_Impl->Peer->Ping(host.c_str(), port, true); }
int Transport::Ping(const std::string& address) const { return m_Impl->Peer ? m_Impl->Peer->GetAveragePing(ParseAddress(address)) : -1; }
std::vector<std::string> Transport::LocalAddresses(uint16_t port) const {
	std::vector<std::string> addresses; if (!m_Impl->Peer) return addresses;
	for (unsigned i = 0; i < m_Impl->Peer->GetNumberOfAddresses(); ++i) { const std::string ip = m_Impl->Peer->GetLocalIP(i); if (ip != "127.0.0.1" && ip.find(':') == std::string::npos) addresses.push_back(ip + ":" + std::to_string(port)); }
	return addresses;
}
uint64_t Transport::QueuedBytes(const std::string& address) const {
	if (!m_Impl->Peer) return 0;
	RakNet::RakNetStatistics stats{};
	if (!m_Impl->Peer->GetStatistics(ParseAddress(address), &stats)) return 0;
	uint64_t bytes = 0; for (auto size: stats.bytesInSendBuffer) bytes += static_cast<uint64_t>(size); return bytes;
}
std::string Transport::Diagnostics(const std::string& address) const {
	RakNet::RakNetStatistics stats{}; if (!m_Impl->Peer || !m_Impl->Peer->GetStatistics(ParseAddress(address), &stats)) return "no statistics";
	return "sent=" + std::to_string(stats.runningTotal[RakNet::USER_MESSAGE_BYTES_SENT]) + " received=" + std::to_string(stats.runningTotal[RakNet::USER_MESSAGE_BYTES_RECEIVED_PROCESSED]) + " queued=" + std::to_string(QueuedBytes(address)) + " congestion=" + std::to_string(stats.BPSLimitByCongestionControl) + " loss=" + std::to_string(stats.packetlossLastSecond);
}
} // namespace RTE::MP
