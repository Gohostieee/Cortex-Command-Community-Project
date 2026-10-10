#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace RTE::MP {
// Enough in-flight data for a 12 Mbps guest across a 160 ms round trip.
inline constexpr uint32_t MinimumWindowBytes = 256 * 1024;
// Receipt is reliable but unordered: one lost acknowledgement must not hold
// every later receipt (and so the resource window) behind it.
enum class Delivery { Control, Input, State, Audio, WorldResource, Receipt };
struct TransportEvent {
	enum class Type { Connected, Disconnected, Failed, Data, Discovered, RoomCode, ServiceStatus, HostedRoom };
	Type Kind{};
	std::string Address, Error;
	std::vector<uint8_t> Data;
};

// Owns the peer and its internal threads. Call this interface from one thread.
class Transport {
public:
	Transport();
	~Transport();
	Transport(const Transport&) = delete;
	Transport& operator=(const Transport&) = delete;
	bool Start(bool host, uint16_t port, const std::string& password, std::string& error);
	bool Connect(const std::string& host, uint16_t port, const std::string& password, std::string& error);
	bool ConnectRelay(bool hostRoom, const std::string& service, uint16_t port, const std::string& code, const std::string& password, std::string& error, bool hosted = false, const std::string& roomName = "");
	bool ReconnectRelay(std::string& error);
	bool IsRelayReady() const;
	int RelaySlot() const;
	std::string RoomCode() const;
	void Stop();
	std::vector<TransportEvent> Poll(size_t budget = 2048);
	bool Send(const std::string& address, std::span<const uint8_t> data, Delivery delivery);
	void Close(const std::string& address);
	void Advertise(std::span<const uint8_t> data);
	void Discover(uint16_t port, const std::string& host = "255.255.255.255");
	int Ping(const std::string& address) const;
	std::vector<std::string> LocalAddresses(uint16_t port) const;
	uint64_t QueuedBytes(const std::string& address) const;
	// Fraction of datagrams to this peer that needed retransmission in the last second.
	double PacketLoss(const std::string& address) const;
	std::string Diagnostics(const std::string& address) const;
private:
	struct Impl;
	std::unique_ptr<Impl> m_Impl;
};
} // namespace RTE::MP
