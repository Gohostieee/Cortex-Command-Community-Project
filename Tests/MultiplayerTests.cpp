#include "MultiplayerProtocol.h"
#include "MultiplayerTransport.h"
#include "MultiplayerWorldProtocol.h"
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
    const std::vector<uint8_t> fixture{220, 0x43, 0x43, 0x4d, 0x50, 0, 3, 4, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
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
void Worlds() {
    namespace W = RTE::MP::World;
    W::ResourceWindow credits; unsigned issued = 0;
    while (credits.CanSend(1131)) credits.Sent(1, uint16_t(issued++), 1131);
    const size_t outstanding = credits.Bytes(); Check(issued > 1 && outstanding <= W::ResourceWindow::Limit && !credits.CanSend(1131), "relay resources exceeded the guest receipt window");
    // Immediate broker transport ACKs do not release any application credits.
    Check(!credits.Receipt(2, 0) && credits.Bytes() == outstanding, "another resource released delivery credits");
    Check(credits.Receipt(1, 0) && !credits.Receipt(1, 0) && credits.CanSend(1131), "duplicate guest receipts inflated delivery credits");
    credits.Sent(1, 0, 1131); Check(credits.Bytes() == outstanding && credits.Contains(1), "resource receipt did not admit exactly its next fragment");
    credits.Reset(); Check(credits.Bytes() == 0 && !credits.Contains(1) && !credits.Receipt(1, 1) && !credits.CanSend(1401), "match reset retained stale resource credits");
    W::Resource resource; resource.Width = 4; resource.Height = 3; resource.Pixels = {1,2,3,4,5,6,7,8,9,10,11,12}; resource.ID = W::ResourceHash(resource);
    Writer rw(Kind::WorldResource); W::WriteResource(rw, resource); Reader rr(rw.Data); Header h; W::Resource decoded;
    Check(ReadHeader(rr, h) && W::ReadResource(rr, decoded) && decoded.Pixels == resource.Pixels, "retained resource round trip failed");
    rw.Data.back() ^= 1; Reader corrupt(rw.Data); Check(ReadHeader(corrupt, h) && !W::ReadResource(corrupt, decoded), "resource corruption accepted");
    W::Snapshot first; first.ID = 1; first.Time = 1000; first.SceneWidth = 1000; first.SceneHeight = 500; first.Wrap = 1; first.InputSequence = 7;
    W::Node node; node.ID = 11; node.Asset = resource.ID; node.Width = node.SourceWidth = 4; node.Height = node.SourceHeight = 3; node.ClipWidth = 200; first.Nodes.push_back(node);
    Writer sw(Kind::WorldSnapshot); W::WriteSnapshot(sw, first); Reader sr(sw.Data); W::Snapshot copy;
    Check(ReadHeader(sr, h) && W::ReadSnapshot(sr, copy) && copy.Nodes == first.Nodes && copy.InputSequence == 7, "world state round trip failed");
    for (size_t i = 0; i < sw.Data.size(); ++i) { Reader truncated(std::span(sw.Data).first(i)); W::Snapshot value; Check(!(ReadHeader(truncated, h) && W::ReadSnapshot(truncated, value)), "truncated state accepted"); }
    auto duplicate = first; duplicate.Nodes.push_back(node); Writer dw(Kind::WorldSnapshot); W::WriteSnapshot(dw, duplicate); Reader dr(dw.Data); Check(ReadHeader(dr, h) && !W::ReadSnapshot(dr, copy), "duplicate entity identity accepted");
    for (uint64_t time : {uint64_t(0), W::MaxTime + 1, std::numeric_limits<uint64_t>::max()}) { auto invalid = first; invalid.Time = time; Writer writer(Kind::WorldSnapshot); W::WriteSnapshot(writer, invalid); Reader reader(writer.Data); Check(ReadHeader(reader, h) && !W::ReadSnapshot(reader, copy), "unsafe presentation clock accepted"); }
    { auto invalid = first; invalid.Phase = 7; Writer writer(Kind::WorldSnapshot); W::WriteSnapshot(writer, invalid); Reader reader(writer.Data); Check(ReadHeader(reader, h) && !W::ReadSnapshot(reader, copy), "invalid activity phase accepted"); }
    auto invalidEvent = node; invalidEvent.StartTime = W::MaxTime; invalidEvent.EndTime = W::MaxTime + 1; Check(!W::Valid(invalidEvent), "overflowing event time accepted");
    W::Timeline timeline; Check(timeline.Push(first, 1020), "initial state rejected");
    auto second = first; second.ID = 2; second.Time = 1050; second.Nodes[0].X = 10; second.CameraX = 10;
    Check(timeline.Push(second, 1080), "next state rejected"); Check(!timeline.Push(first, 1081), "reordered state accepted");
    auto middle = timeline.Sample(1120); Check(std::abs(middle.Nodes[0].X - 5) < 0.001f && std::abs(middle.CameraX - 5) < 0.001f, "guest did not interpolate camera and entity between 20 Hz updates");
    float previous = -1; int distinct = 0;
    for (int frame = 0; frame <= 3; ++frame) { const auto sample = timeline.Sample(1095 + frame * 16); if (sample.Nodes[0].X != previous) ++distinct; previous = sample.Nodes[0].X; }
    Check(distinct == 4, "60 Hz presentation repeats state instead of creating intermediate movement");
    Check(timeline.Sample(1500).Nodes[0].X == timeline.Sample(5000).Nodes[0].X, "lost connection extrapolated without a bound");
    auto third = second; third.ID = 3; third.Time = 1100; third.Nodes.clear();
    Check(timeline.Push(third, 1120), "despawn state rejected"); Check(timeline.Sample(1170).Nodes.size() == 1 && timeline.Sample(1195).Nodes.empty(), "lifecycle changed before its presentation time");
    timeline.Reset(); first.Nodes[0].X = 995; first.Nodes[0].Angle = 3.1f; second = first; second.ID = 2; second.Time = 1050; second.Nodes[0].X = 5; second.Nodes[0].Angle = -3.1f;
    timeline.Push(first, 1020); timeline.Push(second, 1070); middle = timeline.Sample(1120);
    Check(std::abs(middle.Nodes[0].X - 1000) < 0.001f && std::abs(middle.Nodes[0].Angle - 3.14159265f) < 0.001f, "scene or angle wrap used the long path");
    { W::Timeline layers; auto a = first, b = second; a.Nodes[0].Flags = b.Nodes[0].Flags = W::ScreenSpace | W::Layer; a.Nodes[0].X2 = b.Nodes[0].X2 = 640; a.Nodes[0].X = -600; b.Nodes[0].X = 0;
      layers.Push(a, 1020); layers.Push(b, 1070);
      Check(std::abs(layers.Sample(1120).Nodes[0].X + 620) < 0.001f && layers.Sample(1145).Nodes[0].X == 0, "wrapped parallax jumped or moved repeated tiles into the wrong cycle"); }
    timeline.Reset(); first.Nodes[0].Flags |= W::Discontinuous; second.Nodes[0].Flags |= W::Discontinuous; timeline.Push(first, 1020); timeline.Push(second, 1070);
    Check(timeline.Sample(1120).Nodes[0].X == 995, "teleport was interpolated");
    timeline.Reset(); first.Nodes[0].Flags = W::Masked; first.Nodes[0].X = 0; second = first; second.ID = 2; second.Time = 1050; second.Nodes[0].X = 10; second.Nodes[0].Parent = 123;
    timeline.Push(first, 1020); timeline.Push(second, 1070); Check(timeline.Sample(1120).Nodes[0].X == 0, "reparent was interpolated across a lifecycle boundary");
    std::mt19937 random(97); std::vector<uint8_t> bytes(ChunkBytes * 7 + 19); for (auto& b : bytes) b = uint8_t(random());
    std::vector<unsigned> order; for (unsigned i = 0; i < (bytes.size() + ChunkBytes - 1) / ChunkBytes; ++i) order.push_back(i); std::shuffle(order.begin(), order.end(), random);
    W::Assembler assembler; std::optional<std::vector<uint8_t>> assembled;
    for (unsigned i : order) { auto data = std::span(bytes).subspan(i * ChunkBytes, std::min<size_t>(ChunkBytes, bytes.size() - i * ChunkBytes)); if (auto complete = assembler.Push({1, uint32_t(bytes.size()), uint16_t(i), data}, 100)) assembled = std::move(complete); }
    Check(assembled && *assembled == bytes, "retained resources failed under reordering");
    std::vector<uint8_t> parity(ChunkBytes); for (size_t i = 0; i < bytes.size(); ++i) parity[i % ChunkBytes] ^= bytes[i];
    // The first group has exactly one missing chunk; its parity repairs it.
    parity.assign(ChunkBytes, 0); for (size_t i = 0; i < ChunkBytes * 7; ++i) parity[i % ChunkBytes] ^= bytes[i]; for (size_t i = ChunkBytes * 7; i < bytes.size(); ++i) parity[i % ChunkBytes] ^= bytes[i];
    assembler.Reset(); assembled.reset();
    for (unsigned i : order) if (i != 3) { auto data = std::span(bytes).subspan(i * ChunkBytes, std::min<size_t>(ChunkBytes, bytes.size() - i * ChunkBytes)); if (auto complete = assembler.Push({4, uint32_t(bytes.size()), uint16_t(i), data}, 100)) assembled = std::move(complete); }
    assembled = assembler.Push({4, uint32_t(bytes.size()), 0, parity, true}, 100); Check(assembled && *assembled == bytes, "scene state parity did not repair a lost datagram");
    timeline.Reset(); first.Nodes[0].Flags = W::Masked; first.Nodes[0].X = 0; first.Nodes[0].Angle = 0; W::Node child = first.Nodes[0]; child.ID = 12; child.Parent = 11; child.X = 10; first.Nodes.push_back(child);
    second = first; second.ID = 2; second.Time = 1050; second.Nodes[0].Angle = 1.57079633f; second.Nodes[1].X = 0; second.Nodes[1].Y = -10; second.Nodes[1].Angle = 1.57079633f;
    timeline.Push(first, 1020); timeline.Push(second, 1070); middle = timeline.Sample(1120); Check(std::abs(middle.Nodes[1].X - 7.071f) < 0.01f && std::abs(middle.Nodes[1].Y + 7.071f) < 0.01f, "attachment detached from interpolated parent rotation");
    W::Node trail; trail.ID = uint64_t(1) << 59; trail.Type = W::Shape::Pixel; trail.StartTime = 1010; trail.EndTime = 1030; second.Nodes.push_back(trail);
    timeline.Reset(); timeline.Push(first, 1020); timeline.Push(second, 1070); Check(timeline.Sample(1115).Nodes.size() == 3 && timeline.Sample(1135).Nodes.size() == 2, "short-lived trail did not use its host event time");
    Check(!assembler.Push({3, 0xffffffff, 0, std::span(bytes).first(ChunkBytes)}, 200), "unbounded scene assembly accepted");
    for (int i = 0; i < 10000; ++i) { std::vector<uint8_t> fuzz(random() % 400); for (auto& b : fuzz) b = uint8_t(random()); Reader bad(fuzz); W::Snapshot state; W::ReadSnapshot(bad, state); Reader badResource(fuzz); W::ReadResource(badResource, decoded); Reader badChunk(fuzz); W::Chunk chunk; if (W::ReadChunk(badChunk, chunk)) assembler.Push(chunk, 1000 + i); }
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
    std::vector<uint8_t> bulk(135983, 77); World::Assembler assembler;
    for (size_t offset = 0; offset < bulk.size(); offset += ChunkBytes) {
        Writer packet(Kind::WorldResource, 1, 1); World::WriteChunk(packet, {1, uint32_t(bulk.size()), uint16_t(offset / ChunkBytes), std::span(bulk).subspan(offset, std::min<size_t>(ChunkBytes, bulk.size() - offset))});
        Check(host.Send(clientAddress, packet.Data, Delivery::WorldResource), "retained resource send failed");
    }
    bool resourceReceived = false; unsigned received = 0; const auto started = Now();
    while (Now() - started < 5000 && !resourceReceived) {
        for (const auto& event : client.Poll()) if (event.Kind == TransportEvent::Type::Data) { ++received; Reader reader(event.Data); Header header; World::Chunk chunk; if (ReadHeader(reader, header) && World::ReadChunk(reader, chunk)) if (auto complete = assembler.Push(chunk, Now())) resourceReceived = *complete == bulk; }
        host.Poll(); std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::cout << "UDP retained resource: " << received << " chunks, " << Now() - started << " ms\n";
    Check(resourceReceived, "real UDP retained resource failed to assemble");
	std::array<Transport, 2> guests; Transport overflow; unsigned accepted = 0; bool full = false;
	for (auto& guest: guests) Check(guest.Start(false, 0, "", error) && guest.Connect("127.0.0.1", port, "room-secret", error), "additional player connection failed");
	const auto roomDeadline = Now() + 3000;
	while (Now() < roomDeadline && accepted < 2) { host.Poll(); for (auto& guest: guests) for (const auto& event: guest.Poll()) if (event.Kind == TransportEvent::Type::Connected) ++accepted; std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
	Check(accepted == 2, "four-player room did not admit its player slots");
	Check(overflow.Start(false, 0, "", error) && overflow.Connect("127.0.0.1", port, "room-secret", error), "overflow setup failed");
	const auto overflowDeadline = Now() + 3000;
	while (Now() < overflowDeadline && !full) { host.Poll(); for (const auto& event: overflow.Poll()) if (event.Kind == TransportEvent::Type::Failed) full = event.Error == "This room is full."; std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
	Check(full, "fifth player was not rejected from the full room");
	std::vector<uint8_t> oversized(1401, PacketID); Check(!client.Send(hostAddress, oversized, Delivery::State), "oversized datagram sent");
	client.Close(hostAddress);
	const auto closeDeadline = Now() + 3000;
	while (Now() < closeDeadline && !disconnected) { for (const auto& event: host.Poll()) if (event.Kind == TransportEvent::Type::Disconnected && event.Address == clientAddress) disconnected = true; client.Poll(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
	Check(disconnected, "disconnect not delivered");
	for (int cycle = 0; cycle < 20; ++cycle) { Check(client.Start(false, 0, "", error), "transport restart failed"); client.Stop(); }
}
}
int main() {
	try { Wire(); Inputs(); Worlds(); TransportLoopback(); std::cout << "PASS: retained world resources, 60 Hz interpolation from 20 Hz state, camera and scene wrapping, lifecycle, bounded loss continuation, malformed scene packets, all controls, real UDP, password rejection, discovery, full rooms and reconnect\n"; return 0; }
	catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
