#include "MultiplayerProtocol.h"
#include "MultiplayerTransport.h"
#include <chrono>
#include <iostream>
#include <random>
#include <stdexcept>
#include <thread>

using namespace RTE::MP;
namespace {
void Check(bool pass, const char* message) { if (!pass) throw std::runtime_error(message); }
uint64_t Now() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void Wire() {
	Writer writer(Kind::Input, 0x0102030405060708ull, 0x090a0b0c);
	const std::vector<uint8_t> fixture{220, 0x43, 0x43, 0x4d, 0x50, 0, 2, 4, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
	Check(writer.Data == fixture, "wire header fixture differs");
	Input input; input.Sequence = 7; input.Held = (uint64_t(1) << 33) | (uint64_t(1) << 63); input.Presses[33] = 8; input.Presses[63] = 9; input.MouseX = 0xffffffff; input.AimY = -0.75f;
	WriteInput(writer, input);
	Reader reader(writer.Data); Header header; Input decoded;
	Check(ReadHeader(reader, header) && ReadInput(reader, decoded) && decoded.Held == input.Held && decoded.Presses[33] == 8 && decoded.Presses[63] == 9 && decoded.MouseX == input.MouseX && decoded.AimY == input.AimY, "input round trip lost a control or GUI key");
	for (size_t length = 0; length < writer.Data.size(); ++length) {
		Reader truncated(std::span(writer.Data).first(length)); Header h; Input i;
		Check(!(ReadHeader(truncated, h) && ReadInput(truncated, i)), "truncated input accepted");
	}
	writer.Data.push_back(0); Reader extra(writer.Data); Check(ReadHeader(extra, header) && !ReadInput(extra, decoded), "extra input bytes accepted");
	writer.Data = fixture; writer.Data[6] = 1; Reader legacy(writer.Data); Check(!ReadHeader(legacy, header), "legacy protocol accepted");
	Writer nan(Kind::Input); input.AimY = std::numeric_limits<float>::quiet_NaN(); WriteInput(nan, input); Reader bad(nan.Data); Check(ReadHeader(bad, header) && !ReadInput(bad, decoded), "nonfinite aim accepted");
}
void Inputs() {
	InputReceiver receiver; Input input; input.Sequence = 1; input.MouseX = 5; input.Presses[10] = input.Releases[10] = 1;
	input.MousePresses[0] = input.MouseReleases[0] = 1;
	Check(receiver.Push(input, 100), "first input rejected");
	Check(!receiver.Push(input, 101), "duplicate input accepted");
	auto state = receiver.Consume(102);
	Check(state.MouseDX == 5 && state.Pressed == (uint64_t(1) << 10) && state.Released == state.Pressed && state.MousePressed == 1 && state.MouseReleased == 1, "short click lost");
	++input.Sequence; input.MouseX += 5; Check(receiver.Push(input, 110), "repeated motion rejected");
	Check(receiver.Consume(110).MouseDX == 5, "identical consecutive motion was discarded");
	// Simulate a lost press packet and a lost release packet: their counters survive.
	input.Sequence += 3; input.Presses[14] = input.Releases[14] = 1; input.MouseX += 15;
	receiver.Push(input, 120); state = receiver.Consume(120);
	Check(state.Pressed == (uint64_t(1) << 14) && state.Released == state.Pressed && state.MouseDX == 15, "loss recovery lost input edges");
	Check(receiver.Consume(121).Pressed == 0, "press edge replayed");
	++input.Sequence; input.Held = uint64_t(1) << 33; input.MouseHeld = 2; input.AimX = 1; receiver.Push(input, 130);
	Check(receiver.LastSequence() == input.Sequence, "input observation consumed state");
	state = receiver.Consume(381); Check(state.Snapshot.Held == 0 && state.Released == (uint64_t(1) << 33) && state.MouseReleased == 2 && state.Snapshot.AimX == 0, "timeout left controls stuck");
	Check(receiver.Consume(382).Released == 0, "timeout release replayed");
	receiver.Reset(); input = {}; input.Sequence = 0xfffffffe; input.MouseX = 0xfffffffe; receiver.Push(input, 400); receiver.Consume(400);
	input.Sequence = 1; input.MouseX = 3; receiver.Push(input, 410); Check(receiver.Consume(410).MouseDX == 5, "sequence or motion wrap failed");
	Check(Newer(1, 0xfffffffe) && !Newer(0xfffffffe, 1), "sequence ordering wrap failed");
}
struct Packet { FrameInfo Info; uint16_t Index; bool Parity; std::vector<uint8_t> Data; };
std::vector<Packet> Packets(const FrameInfo& info, const std::vector<uint8_t>& bytes) {
	std::vector<Packet> packets;
	for (uint16_t start = 0; start < ChunkCount(info); start += ParityGroup) {
		std::vector<uint8_t> parity(ChunkBytes);
		for (uint16_t index = start; index < std::min<int>(start + ParityGroup, ChunkCount(info)); ++index) {
			auto part = std::span(bytes).subspan(size_t(index) * ChunkBytes, ChunkSize(info, index));
			for (size_t j = 0; j < part.size(); ++j) parity[j] ^= part[j];
			packets.push_back({info, index, false, {part.begin(), part.end()}});
		}
		packets.push_back({info, static_cast<uint16_t>(start / ParityGroup), true, std::move(parity)});
	}
	return packets;
}
void Frames() {
	std::mt19937 random(1234); std::vector<uint8_t> pixels(25 * ChunkBytes - 7); for (auto& pixel: pixels) pixel = static_cast<uint8_t>(random());
	FrameInfo info{1, 4, static_cast<uint32_t>(pixels.size()), 640, 360, 12, 13};
	for (unsigned missing = 0; missing < ParityGroup; ++missing) {
		auto packets = Packets(info, pixels); std::erase_if(packets, [missing](const Packet& packet) { return !packet.Parity && packet.Index % ParityGroup == missing; });
		std::shuffle(packets.begin(), packets.end(), random); FrameAssembler assembler; std::optional<FrameAssembler::Complete> complete;
		for (const auto& packet: packets) {
			Writer writer(Kind::Frame, 9, 2); WriteChunk(writer, {packet.Info, packet.Index, packet.Parity, packet.Data});
			Check(writer.Data.size() < 1400, "frame exceeded datagram limit"); Reader reader(writer.Data); Header header; FrameChunk chunk;
			Check(ReadHeader(reader, header) && ReadChunk(reader, chunk), "frame wire decode failed");
			if (auto frame = assembler.Push(chunk, 100)) complete = std::move(frame);
			Check(!assembler.Push(chunk, 100), "duplicate frame chunk presented twice");
		}
		Check(complete && complete->Bytes == pixels && complete->Info == info, "parity recovery failed under loss and reorder");
	}
	FrameAssembler assembler; auto packets = Packets(info, pixels);
	for (const auto& packet: packets) if (packet.Parity || packet.Index > 1) Check(!assembler.Push({packet.Info, packet.Index, packet.Parity, packet.Data}, 100), "two missing chunks incorrectly recovered");
	++info.ID; auto next = Packets(info, pixels); std::optional<FrameAssembler::Complete> completed;
	for (const auto& packet: next) if (auto frame = assembler.Push({packet.Info, packet.Index, packet.Parity, packet.Data}, 110)) completed = std::move(frame);
	Check(completed && completed->Bytes == pixels, "lost frame corrupted the next independent frame");
	Check(!assembler.Push({packets[0].Info, 0, false, packets[0].Data}, 111), "old frame accepted after presentation");
	assembler.Reset(); Check(!assembler.Push({info, 0, false, packets[0].Data}, 100), "incomplete frame presented");
	for (const auto& packet: next) if (packet.Parity || packet.Index > 1) Check(!assembler.Push({info, packet.Index, packet.Parity, packet.Data}, 1101), "expired frame chunks retained");
	FrameInfo huge = info; huge.Bytes = 0xffffffffu; Check(!assembler.Push({huge, 0, false, packets[0].Data}, 500), "unbounded frame accepted");
	// Decoder receives arbitrary network bytes, including truncated valid chunks.
	for (int attempt = 0; attempt < 20000; ++attempt) {
		std::vector<uint8_t> fuzz(random() % 1500); for (auto& byte: fuzz) byte = static_cast<uint8_t>(random());
		Writer packet(attempt % 2 ? Kind::Frame : Kind::Input); packet.Bytes(fuzz);
		Reader reader(packet.Data); Header header; Check(ReadHeader(reader, header), "fuzz header failed");
		if (header.Type == Kind::Frame) { FrameChunk chunk; if (ReadChunk(reader, chunk)) assembler.Push(chunk, 1000 + attempt); }
		else { Input input; ReadInput(reader, input); }
	}
}
void TransportLoopback() {
	Transport host, client, wrong, discovery; std::string error;
	constexpr uint16_t port = 38997;
	Check(host.Start(true, port, "room-secret", error), error.c_str());
	Writer announcement(Kind::Announcement); announcement.Text("Test room", 63); announcement.U8(1); announcement.U8(0); host.Advertise(announcement.Data);
	Check(wrong.Start(false, 0, "", error) && wrong.Connect("127.0.0.1", port, "wrong", error), "wrong password connection setup failed");
	Check(client.Start(false, 0, "", error) && client.Connect("127.0.0.1", port, "room-secret", error), "client setup failed");
	Check(discovery.Start(false, 0, "", error), "discovery setup failed"); discovery.Discover(port, "127.0.0.1");
	bool hostConnected = false, clientConnected = false, rejected = false, discovered = false, echoed = false, disconnected = false;
	std::string hostAddress, clientAddress; Writer message(Kind::Hello); message.Text("loopback");
	const uint64_t deadline = Now() + 10000;
	while (Now() < deadline && (!echoed || !rejected || !discovered)) {
		for (const auto& event: host.Poll()) { if (event.Kind == TransportEvent::Type::Connected) { hostConnected = true; clientAddress = event.Address; } else if (event.Kind == TransportEvent::Type::Data) { Check(event.Data == message.Data, "transport altered data"); host.Send(event.Address, event.Data, Delivery::Control); } }
		for (const auto& event: client.Poll()) { if (event.Kind == TransportEvent::Type::Connected) { clientConnected = true; hostAddress = event.Address; client.Send(hostAddress, message.Data, Delivery::Control); } else if (event.Kind == TransportEvent::Type::Data) echoed = event.Data == message.Data; }
		for (const auto& event: wrong.Poll()) if (event.Kind == TransportEvent::Type::Failed) rejected = event.Error == "Incorrect room password.";
		for (const auto& event: discovery.Poll()) if (event.Kind == TransportEvent::Type::Discovered) discovered = event.Data == announcement.Data && event.Address == "127.0.0.1:38997";
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	Check(hostConnected && clientConnected && echoed && rejected && discovered, "loopback, password rejection or LAN discovery failed");
	std::vector<uint8_t> bulk(135983, 77); FrameInfo info{1, 1, static_cast<uint32_t>(bulk.size()), 640, 360, 0, 0}; FrameAssembler assembler;
	for (const auto& packet: Packets(info, bulk)) { Writer frame(Kind::Frame, 1, 1); WriteChunk(frame, {info, packet.Index, packet.Parity, packet.Data}); Check(host.Send(clientAddress, frame.Data, Delivery::Frame), "bulk frame send failed"); }
	bool frameReceived = false; unsigned received = 0; const auto frameStarted = Now(); uint64_t firstPacket = 0, lastPacket = 0;
	while (Now() - frameStarted < 3000 && !frameReceived) {
		for (const auto& event: client.Poll()) if (event.Kind == TransportEvent::Type::Data) { ++received; if (!firstPacket) firstPacket = Now(); lastPacket = Now(); Reader reader(event.Data); Header header; FrameChunk chunk; if (ReadHeader(reader, header) && ReadChunk(reader, chunk)) if (auto complete = assembler.Push(chunk, Now())) frameReceived = complete->Bytes == bulk; }
		host.Poll(); std::this_thread::sleep_for(std::chrono::milliseconds(16));
	}
	std::cout << "UDP frame: " << received << " chunks, " << Now() - frameStarted << " ms, arrival span=" << lastPacket - firstPacket << "\n";
	Check(frameReceived, "real UDP frame failed to assemble");
	std::array<Transport, 2> guests; Transport overflow; unsigned accepted = 0; bool full = false;
	for (auto& guest: guests) Check(guest.Start(false, 0, "", error) && guest.Connect("127.0.0.1", port, "room-secret", error), "additional player connection failed");
	const auto roomDeadline = Now() + 3000;
	while (Now() < roomDeadline && accepted < 2) { host.Poll(); for (auto& guest: guests) for (const auto& event: guest.Poll()) if (event.Kind == TransportEvent::Type::Connected) ++accepted; std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
	Check(accepted == 2, "four-player room did not admit its player slots");
	Check(overflow.Start(false, 0, "", error) && overflow.Connect("127.0.0.1", port, "room-secret", error), "overflow setup failed");
	const auto overflowDeadline = Now() + 3000;
	while (Now() < overflowDeadline && !full) { host.Poll(); for (const auto& event: overflow.Poll()) if (event.Kind == TransportEvent::Type::Failed) full = event.Error == "This room is full."; std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
	Check(full, "fifth player was not rejected from the full room");
	std::vector<uint8_t> oversized(1401, PacketID); Check(!client.Send(hostAddress, oversized, Delivery::Frame), "oversized datagram sent");
	client.Close(hostAddress);
	const auto closeDeadline = Now() + 3000;
	while (Now() < closeDeadline && !disconnected) { for (const auto& event: host.Poll()) if (event.Kind == TransportEvent::Type::Disconnected && event.Address == clientAddress) disconnected = true; client.Poll(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
	Check(disconnected, "disconnect not delivered");
	for (int cycle = 0; cycle < 20; ++cycle) { Check(client.Start(false, 0, "", error), "transport restart failed"); client.Stop(); }
}
}
int main() {
	try { Wire(); Inputs(); Frames(); TransportLoopback(); std::cout << "PASS: wire bounds, all controls, loss/reorder/duplicates, timeout, frame repair, 20000 malformed packet bodies, real UDP frames, password rejection, LAN discovery, full rooms, disconnect and restart\n"; return 0; }
	catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
