#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace RTE::MP {
enum class Delivery { Control, Input, State, Audio, WorldResource };
struct TransportEvent {
	enum class Type { Connected, Disconnected, Failed, Data, Discovered, RoomCode, ServiceStatus };
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
	bool ConnectRelay(bool hostRoom, const std::string& service, uint16_t port, const std::string& code, const std::string& password, std::string& error);
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
	std::string Diagnostics(const std::string& address) const;
private:
	struct Impl;
	std::unique_ptr<Impl> m_Impl;
};
} // namespace RTE::MP
