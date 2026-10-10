#include "MultiplayerProtocol.h"
#include "MultiplayerTransport.h"
#include "MultiplayerWorldProtocol.h"
#include <chrono>
#include <cstdlib>
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
    const std::vector<uint8_t> fixture{220, 0x43, 0x43, 0x4d, 0x50, 0, 10, 4, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
	Check(writer.Data == fixture, "wire header fixture differs");
	Input input; input.Sequence = 7; input.Held = (uint64_t(1) << 33) | (uint64_t(1) << 63); input.Presses[33] = 8; input.Presses[63] = 9; input.MouseX = 0xffffffff; input.AimY = -0.75f;
	input.ViewValid = input.CursorValid = input.PointerValid = true; input.ViewX = 2500; input.CursorMode = 3; input.CursorX = 2820; input.PointerX = 100;
	WriteInput(writer, input);
	Reader reader(writer.Data); Header header; Input decoded;
	Check(ReadHeader(reader, header) && ReadInput(reader, decoded) && decoded.Held == input.Held && decoded.Presses[33] == 8 && decoded.Presses[63] == 9 && decoded.MouseX == input.MouseX && decoded.AimY == input.AimY, "input round trip lost a control or GUI key");
	Check(decoded.ViewValid && decoded.CursorValid && decoded.PointerValid && decoded.ViewX == 2500 && decoded.CursorMode == 3 && decoded.CursorX == 2820 && decoded.PointerX == 100, "client-owned view or cursor coordinates changed on the wire");
	for (size_t length = 0; length < writer.Data.size(); ++length) {
		Reader truncated(std::span(writer.Data).first(length)); Header h; Input i;
		Check(!(ReadHeader(truncated, h) && ReadInput(truncated, i)), "truncated input accepted");
	}
	writer.Data.push_back(0); Reader extra(writer.Data); Check(ReadHeader(extra, header) && !ReadInput(extra, decoded), "extra input bytes accepted");
	writer.Data = fixture; writer.Data[6] = 1; Reader legacy(writer.Data); Check(!ReadHeader(legacy, header), "legacy protocol accepted");
    writer.Data = fixture; writer.Data[6] = 9; Reader previous(writer.Data); Check(!ReadHeader(previous, header), "v9 accepted the incompatible own-motion snapshot format");
	Writer nan(Kind::Input); input.AimY = std::numeric_limits<float>::quiet_NaN(); WriteInput(nan, input); Reader bad(nan.Data); Check(ReadHeader(bad, header) && !ReadInput(bad, decoded), "nonfinite aim accepted");
	input.AimY = 0; input.ViewX = std::numeric_limits<float>::infinity(); Writer unsafe(Kind::Input); WriteInput(unsafe, input); Reader badView(unsafe.Data);
	Check(ReadHeader(badView, header) && !ReadInput(badView, decoded), "nonfinite local camera accepted");
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
	++input.Sequence; input.MouseX += 10; receiver.Push(input, 115);
	Check(receiver.LastInput().MouseX == 10 && receiver.LastInput().Sequence == 2, "presentation acknowledges mouse motion before a simulation step applies it, causing cursor rebound");
	receiver.Consume(115);
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
	// A resumed player keeps cumulative motion and edge counters. Disconnecting
	// releases controls without accepting old datagrams or replaying old motion.
	receiver.Reset(); input = {}; input.Sequence = 700; input.MouseX = 9000;
	input.Presses[10] = 8; input.Held = uint64_t(1) << 10; input.MouseHeld = 2;
	receiver.Push(input, 500); receiver.Consume(500);
	receiver.ReleaseControls();
	state = receiver.Consume(501);
	Check(state.Released == (uint64_t(1) << 10) && state.MouseReleased == 2 && !state.Snapshot.Held, "reconnect reset lost held-control releases");
	Check(!receiver.Push(input, 502), "reconnect admitted an old input datagram");
	++input.Sequence; input.Held = input.MouseHeld = 0; input.MouseX += 3;
	receiver.Push(input, 503); state = receiver.Consume(503);
	Check(state.MouseDX == 3 && !state.Pressed && !state.MousePressed, "reconnect replayed cumulative mouse motion or old button presses");
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
    W::ResourceRequests repairs; std::unordered_set<uint64_t> missingTiles, requestedTiles;
    for (uint64_t id = 1; id <= 4096; ++id) missingTiles.insert(id);
    for (uint64_t time = 1000; time <= 64000; time += 1000) {
        const auto batch = repairs.Select(missingTiles, time);
        Check(batch.size() == 64, "large scene repair traffic is not bounded to 64 requests per second");
        for (auto id : batch) Check(requestedTiles.insert(id).second, "scene repair starved a missing tile by retrying earlier tiles");
    }
    Check(requestedTiles == missingTiles, "scene repair failed to reach the last missing tile");
    missingTiles = {4096}; Check(repairs.Select(missingTiles, 64001).empty(), "scene repair immediately duplicated an in-flight request");
    Check(repairs.Select(missingTiles, 69000) == std::vector<uint64_t>{4096}, "scene repair never retried a lost resource");
    { W::ResourceRequests graced; std::unordered_set<uint64_t> lacking{7, 8, 9}, view{9};
      Check(graced.Select(lacking, 1000, 2000, &view).empty(), "repair requested a resource the host had only just started sending");
      const auto batch = graced.Select(lacking, 3000, 2000, &view);
      Check(batch.size() == 3 && batch.front() == 9, "repair did not request the first view's scenery ahead of other resources"); }
    W::Resource resource; resource.Width = 4; resource.Height = 3; resource.Pixels = {1,2,3,4,5,6,7,8,9,10,11,12}; resource.ID = W::ResourceHash(resource);
    { std::vector<uint64_t> manifest{resource.ID, 1234}; Writer writer(Kind::WorldManifest); W::WriteManifest(writer, manifest); Reader reader(writer.Data); Header header; std::unordered_set<uint64_t> assets;
      Check(ReadHeader(reader, header) && W::ReadManifest(reader, assets) && assets.size() == 2 && assets.contains(resource.ID), "whole-scene warmup manifest changed resource identities");
      manifest.push_back(resource.ID); Writer duplicate(Kind::WorldManifest); W::WriteManifest(duplicate, manifest); Reader bad(duplicate.Data); Check(ReadHeader(bad, header) && !W::ReadManifest(bad, assets), "duplicate warmup resource accepted"); }
    { W::SceneMap map; map.CameraX = 100;
      W::Node tile; tile.ID = (uint64_t(1) << 62) | (uint64_t(100) << 48); tile.Flags = W::ScreenSpace | W::Layer; tile.Asset = resource.ID;
      tile.X = 1900; tile.X3 = tile.Y3 = 1; tile.Width = tile.SourceWidth = 64; tile.Height = tile.SourceHeight = 64; map.Nodes = {tile};
      Writer writer(Kind::WorldManifest); W::WriteManifest(writer, std::vector<uint64_t>{resource.ID}, map); Reader reader(writer.Data); Header header; std::unordered_set<uint64_t> assets; W::SceneMap copy;
      Check(ReadHeader(reader, header) && W::ReadManifest(reader, assets, &copy) && copy.Nodes == map.Nodes && copy.CameraX == 100, "map baseline lost distant tile coordinates");
      W::RetainedLayers layers; layers.Install(copy); W::Snapshot local; local.CameraX = 2000; layers.Compose(local);
      Check(local.Nodes.size() == 1 && local.Nodes[0].X == 0, "local free flight cannot draw scenery outside the host viewport");
      W::Snapshot update; update.CameraX = 1900; tile.X = 100; tile.Asset = 1234; update.Nodes = {tile}; layers.Update(update);
      local.Nodes.clear(); layers.Compose(local); Check(local.Nodes[0].Asset == 1234 && local.Nodes[0].X == 0, "retained terrain updates moved tiles or retained destroyed terrain");
      layers.Reset(); local.Nodes.clear(); layers.Compose(local); Check(local.Nodes.empty(), "new match retained an earlier scene map");
      map.Nodes[0].Asset = 1234; Writer badMap(Kind::WorldManifest); W::WriteManifest(badMap, std::vector<uint64_t>{resource.ID}, map); Reader bad(badMap.Data);
      Check(ReadHeader(bad, header) && !W::ReadManifest(bad, assets), "map layout refers to an asset outside its manifest");
      const auto revised = W::ManifestResources(std::vector<uint64_t>{resource.ID}, map);
      Writer rejoin(Kind::WorldManifest); W::WriteManifest(rejoin, revised, map); Reader current(rejoin.Data);
      Check(ReadHeader(current, header) && W::ReadManifest(current, assets, &copy) && assets.contains(1234) && assets.size() == 2, "reconnect manifest cannot deliver terrain changed after the original baseline"); }
    { W::SceneMap map; W::Node back; back.ID = (uint64_t(1) << 62) | (uint64_t(1) << 48); back.Flags = W::ScreenSpace | W::Layer; back.Asset = resource.ID;
      back.Width = back.SourceWidth = 64; back.Height = back.SourceHeight = 64;
      W::Node front; front.ID = (uint64_t(1) << 61) | (uint64_t(2) << 32) | 1; front.Flags = W::ScreenSpace | W::Layer; front.Type = W::Shape::Rectangle; front.Width = front.Height = 64;
      map.Nodes = {back, front}; W::RetainedLayers layers; layers.Install(map); W::Snapshot local; layers.Compose(local);
      Check(local.Nodes.size() == 2 && W::LayerOrdinal(local.Nodes[0]) == 1 && W::LayerOrdinal(local.Nodes[1]) == 2, "retained scenery changes native parallax layer order");
      W::Snapshot pending; pending.Width = 640; pending.Height = 480; std::unordered_set<uint64_t> visible; layers.VisibleAssets(pending, visible);
      Check(visible.contains(resource.ID), "first view does not wait for its retained scenery");
      W::Node changed = back; changed.Asset = 4321; pending.Nodes = {changed}; visible.clear(); layers.VisibleAssets(pending, visible);
      Check(!visible.contains(resource.ID), "first view waits for a tile version its own state replaces"); }
    Writer rw(Kind::WorldResource); W::WriteResource(rw, resource); Reader rr(rw.Data); Header h; W::Resource decoded;
    Check(ReadHeader(rr, h) && W::ReadResource(rr, decoded) && decoded.Pixels == resource.Pixels, "retained resource round trip failed");
    rw.Data.back() ^= 1; Reader corrupt(rw.Data); Check(ReadHeader(corrupt, h) && !W::ReadResource(corrupt, decoded), "resource corruption accepted");
    W::Snapshot first; first.ID = 1; first.Time = 1000; first.SceneWidth = 1000; first.SceneHeight = 500; first.Wrap = 1; first.InputSequence = 7;
    W::Node node; node.ID = 11; node.Asset = resource.ID; node.Width = node.SourceWidth = 4; node.Height = node.SourceHeight = 3; node.ClipWidth = 200; first.Nodes.push_back(node);
    Writer sw(Kind::WorldSnapshot); W::WriteSnapshot(sw, first); Reader sr(sw.Data); W::Snapshot copy;
    Check(ReadHeader(sr, h) && W::ReadSnapshot(sr, copy) && copy.Nodes == first.Nodes && copy.InputSequence == 7, "world state round trip failed");
    { auto motion = first; motion.ControlledActor = 11; motion.MotionFlags = W::MotionPredictable | W::MotionJetpack;
      motion.ActorX = 150; motion.ActorY = 70; motion.ActorVelX = -90; motion.ActorVelY = 25; motion.Gravity = 600; motion.InputAge = 65535; motion.SimSpeed = 52;
      Writer writer(Kind::WorldSnapshot); W::WriteSnapshot(writer, motion); Reader reader(writer.Data);
      Check(ReadHeader(reader, h) && W::ReadSnapshot(reader, copy) && copy.ControlledActor == motion.ControlledActor && copy.MotionFlags == motion.MotionFlags && copy.ActorX == motion.ActorX && copy.ActorY == motion.ActorY && copy.ActorVelX == motion.ActorVelX && copy.ActorVelY == motion.ActorVelY && copy.Gravity == motion.Gravity && copy.InputAge == motion.InputAge && copy.SimSpeed == motion.SimSpeed && copy.Nodes == motion.Nodes, "own-motion state or input timing changed in transit"); }
    for (size_t i = 0; i < sw.Data.size(); ++i) { Reader truncated(std::span(sw.Data).first(i)); W::Snapshot value; Check(!(ReadHeader(truncated, h) && W::ReadSnapshot(truncated, value)), "truncated state accepted"); }
    auto duplicate = first; duplicate.Nodes.push_back(node); Writer dw(Kind::WorldSnapshot); W::WriteSnapshot(dw, duplicate); Reader dr(dw.Data); Check(ReadHeader(dr, h) && !W::ReadSnapshot(dr, copy), "duplicate entity identity accepted");
    for (uint64_t time : {uint64_t(0), W::MaxTime + 1, std::numeric_limits<uint64_t>::max()}) { auto invalid = first; invalid.Time = time; Writer writer(Kind::WorldSnapshot); W::WriteSnapshot(writer, invalid); Reader reader(writer.Data); Check(ReadHeader(reader, h) && !W::ReadSnapshot(reader, copy), "unsafe presentation clock accepted"); }
    { auto invalid = first; invalid.Phase = 7; Writer writer(Kind::WorldSnapshot); W::WriteSnapshot(writer, invalid); Reader reader(writer.Data); Check(ReadHeader(reader, h) && !W::ReadSnapshot(reader, copy), "invalid activity phase accepted"); }
    auto invalidEvent = node; invalidEvent.StartTime = W::MaxTime; invalidEvent.EndTime = W::MaxTime + 1; Check(!W::Valid(invalidEvent), "overflowing event time accepted");
    { auto path = first; path.Nodes[0].Type = W::Shape::PixelPath; path.Nodes[0].Pixels = {{0, 0}, {1, -1}, {20, 8}, {-500, 40}}; path.Nodes[0].PixelColors = {1, 255, 8, 0};
      Writer writer(Kind::WorldSnapshot); W::WriteSnapshot(writer, path); Reader reader(writer.Data);
      Check(ReadHeader(reader, h) && W::ReadSnapshot(reader, copy) && copy.Nodes == path.Nodes, "native trail gaps, negative offsets or ricochets changed in transit");
      for (size_t i = writer.Data.size() - 18; i < writer.Data.size(); ++i) { Reader truncated(std::span(writer.Data).first(i)); Check(ReadHeader(truncated, h) && !W::ReadSnapshot(truncated, copy), "truncated raster trail accepted"); }
      path.Nodes[0].Pixels.resize(16385); Check(!W::Valid(path.Nodes[0]), "unbounded raster trail accepted"); }
    { auto loaded = first; loaded.Nodes.clear();
      for (size_t i = 0; i < W::MaxNodes - 18; ++i) { auto pixel = node; pixel.ID = 100 + i; pixel.Type = W::Shape::Pixel; loaded.Nodes.push_back(std::move(pixel)); }
      for (size_t i = 0; i < 16; ++i) { W::Node trail; trail.ID = 40000 + i; trail.Type = W::Shape::PixelPath; trail.Flags = W::Discontinuous;
        trail.Pixels.resize(16384); trail.PixelColors.resize(16384, 1); loaded.Nodes.push_back(std::move(trail)); }
      auto terrain = node; terrain.ID = 50000; terrain.Flags = W::Layer | W::ScreenSpace; loaded.Nodes.push_back(terrain);
      auto cursor = node; cursor.ID = 50001; cursor.Flags = W::ScreenSpace; cursor.Control = W::Interaction::Pointer; loaded.Nodes.push_back(cursor);
      W::BoundSnapshot(loaded);
      Writer writer(Kind::WorldSnapshot); W::WriteSnapshot(writer, loaded);
      Check(writer.Data.size() <= W::MaxPayload, "a valid heavy combat scene exceeds the packet limit and stops all guest state updates");
      Reader reader(writer.Data); Check(ReadHeader(reader, h) && W::ReadSnapshot(reader, copy), "bounded heavy combat snapshot cannot be received");
      Check(std::any_of(copy.Nodes.begin(), copy.Nodes.end(), [](const auto& n) { return n.ID == 50000; }) && std::any_of(copy.Nodes.begin(), copy.Nodes.end(), [](const auto& n) { return n.ID == 50001; }), "combat overload removes terrain or the user cursor");
    }
    W::Timeline timeline; Check(timeline.Push(first, 1020), "initial state rejected");
    { auto burst = first; burst.Nodes.clear();
      for (uint64_t id = 100; id < 5100; ++id) { W::Node particle; particle.ID = id; particle.Type = W::Shape::Pixel; particle.X = float(id % 640); particle.Y = float(id % 360); particle.Color = 7; burst.Nodes.push_back(particle); }
      auto actor = node; actor.ID = 6000; burst.Nodes.push_back(actor); burst.CriticalNodes = std::make_shared<std::unordered_set<uint64_t>>(std::unordered_set<uint64_t>{actor.ID});
      auto terrain = node; terrain.ID = 6001; terrain.Flags = W::Layer; burst.Nodes.push_back(terrain);
      auto hud = node; hud.ID = 6002; hud.Flags = W::ScreenSpace; burst.Nodes.push_back(hud);
      W::Node trail; trail.ID = 6003; trail.Type = W::Shape::PixelPath; trail.Pixels = {{0, 0}, {10, -3}}; trail.PixelColors = {4, 8}; burst.Nodes.push_back(trail);
      auto measure = [](const W::Snapshot& snapshot) { Writer packet(Kind::WorldSnapshot); W::WriteSnapshot(packet, snapshot); return packet.Data.size(); };
      W::FitSnapshot(burst, 30000, measure);
      Check(measure(burst) <= 30000, "explosion particles consume more than one movement update's upload budget and freeze the guest");
      for (uint64_t id : {6000ull, 6001ull, 6002ull, 6003ull}) Check(std::any_of(burst.Nodes.begin(), burst.Nodes.end(), [id](const auto& n) { return n.ID == id; }), "explosion bandwidth control removes an actor, terrain, HUD or bullet trail");
      Check(std::is_sorted(burst.Nodes.begin(), burst.Nodes.end(), [](const auto& a, const auto& b) { return a.ID < b.ID; }), "explosion bandwidth control changes native painter order");
    }
    auto second = first; second.ID = 2; second.Time = 1050; second.Nodes[0].X = 10; second.CameraX = 10;
    Check(timeline.Push(second, 1080), "next state rejected"); Check(!timeline.Push(first, 1081), "reordered state accepted");
    auto middle = timeline.Sample(1120); Check(std::abs(middle.Nodes[0].X - 5) < 0.001f && std::abs(middle.CameraX - 5) < 0.001f, "guest did not interpolate camera and entity between 20 Hz updates");
    float previous = -1; int distinct = 0;
    for (int frame = 0; frame <= 3; ++frame) { const auto sample = timeline.Sample(1095 + frame * 16); if (sample.Nodes[0].X != previous) ++distinct; previous = sample.Nodes[0].X; }
    Check(distinct == 4, "60 Hz presentation repeats state instead of creating intermediate movement");
    Check(timeline.Sample(1500).Nodes[0].X == timeline.Sample(5000).Nodes[0].X, "lost connection extrapolated without a bound");
    { W::Timeline hud; auto a = first, b = second; a.Nodes[0].Flags = b.Nodes[0].Flags = W::ScreenSpace;
      hud.Push(a, 1020); hud.Push(b, 1080);
      Check(hud.Sample(1120).Nodes[0].X == b.Nodes[0].X, "guest cursor waits behind the world interpolation buffer"); }
    { auto view = first; view.ControlledActor = 11; view.AimX = 1; view.AimY = 0; view.LookX = 40; view.LookY = 0; view.Wrap = 3;
      W::Node dot; dot.ID = 30; dot.Parent = 11; dot.Type = W::Shape::Pixel; dot.Flags = W::ScreenSpace; dot.Control = W::Interaction::Aim; dot.X = 30;
      W::Node terrain = first.Nodes[0]; terrain.ID = 31; terrain.Flags = W::ScreenSpace | W::Layer; terrain.X3 = terrain.Y3 = 1;
      view.Nodes = {dot, terrain}; auto local = view;
      W::PredictLocalView(local, view, 0, 1, false);
      Check(std::abs(local.Nodes[0].X) < 0.001f && std::abs(local.Nodes[0].Y - 30) < 0.001f, "local aim waits for another network state");
      view.Nodes[0].Type = W::Shape::Sprite; local = view; W::PredictLocalView(local, view, 0, 1, false);
      Check(std::abs(local.Nodes[0].Angle + 1.57079633f) < .001f, "predicted cursor sprite rotates against its native screen-space direction");
      view.Nodes[0].Type = W::Shape::Pixel;
      Check(local.CameraX == view.CameraX && local.CameraY == view.CameraY, "visual aiming still overrides the local camera");
      float previous = -999; unsigned distinct = 0;
      for (int frame = 0; frame < 10; ++frame) { auto sample = view; W::PredictLocalView(sample, view, std::cos(frame * 0.05f), std::sin(frame * 0.05f), false); if (sample.Nodes[0].Y != previous) ++distinct; previous = sample.Nodes[0].Y; }
      Check(distinct == 10, "mouse presentation cannot move between host updates");
      local = view; local.Nodes[0].Control = W::Interaction::RadialCursor; W::PredictLocalView(local, view, 0, 1, true);
      Check(std::abs(local.Nodes[0].Y - 30) < 0.001f && local.CameraX == view.CameraX && local.CameraY == view.CameraY, "radial cursor prediction moved the battlefield camera");
      view.MouseX = 0xfffffffe; view.MouseY = 20; view.Nodes[0].Control = W::Interaction::Pointer; view.Nodes[0].X = 100; view.Nodes[0].Y = 100;
      view.Nodes[0].X2 = view.Nodes[0].Y2 = 101;
      W::LocalPointer pointer; Input pointerInput; pointerInput.MouseX = 3; pointerInput.MouseY = 22;
      local = view; pointer.Apply(local, view, pointerInput, true);
      Check(local.Nodes[0].X == 105 && local.Nodes[0].Y == 102, "GUI cursor prediction lost cumulative mouse wrap or vertical movement");
      view.MouseX = 3; view.MouseY = 22; view.Nodes[0].X = 5; view.Nodes[0].Y = 8; view.Nodes[0].X2 = 6; view.Nodes[0].Y2 = 9;
      local = view; pointer.Apply(local, view, pointerInput, true);
      Check(local.Nodes[0].X == 105 && local.Nodes[0].Y == 102, "host pointer update overrides the client-owned cursor");
      pointer.Export(pointerInput, true); Check(pointerInput.PointerValid && pointerInput.PointerX == 106, "GUI clicks do not use the cursor hotspot the guest sees");
      view.ViewMode = 3; view.MouseScale = 1; view.CameraX = view.CameraTargetX = 60; view.CameraY = view.CameraTargetY = 20;
      W::LocalCamera camera; Input mouse; mouse.MouseX = 20; mouse.MouseY = view.MouseY; view.MouseX = 0;
      local = view; camera.Apply(local, view, mouse, 1000); const float predicted = local.CameraX;
      Check(predicted > view.CameraX, "Q camera does not respond between host updates");
      view.CameraTargetX += 20; view.MouseX = 20;
      local = view; camera.Apply(local, view, mouse, 1016);
      Check(local.CameraX >= predicted, "mouse acknowledgement pulls the guest camera backwards before native easing catches up");
      auto noAimCamera = view; W::PredictLocalView(noAimCamera, view, 0, 1, false);
      Check(noAimCamera.CameraX == view.CameraX && noAimCamera.CameraY == view.CameraY, "actor aim prediction fights Q camera movement");
      view.CameraX = view.CameraTargetX = 60; view.SceneWidth = 4096; view.MouseX = 0; mouse.MouseX = 0; camera.Reset();
      local = view; camera.Apply(local, view, mouse, 2000);
      mouse.MouseX = 600;
      for (uint64_t frame = 1; frame <= 90; ++frame) { local = view; camera.Apply(local, view, mouse, 2000 + frame * 16); }
      Check(W::Displacement(view.CameraX, local.CameraX, view.SceneWidth, true) > 150, "free camera stops at the host view margin during missing snapshots");
      const float owned = local.CameraX;
      view.MouseX = mouse.MouseX; view.CameraX = 2000; view.CameraTargetX = 2500;
      local = view; camera.Apply(local, view, mouse, 3456);
      Check(std::abs(local.CameraX - owned) < 2, "late host camera or input acknowledgement rebases local free flight");
      camera.Export(mouse, true); Check(mouse.ViewValid && mouse.CursorValid && mouse.CursorMode == 3 && mouse.ViewX == local.CameraX, "client view and action coordinates are not exported");
      const float paused = local.CameraX; mouse.MouseX += 100;
      local = view; camera.Apply(local, view, mouse, 3472, false);
      Check(local.CameraX == paused, "opening the menu resets the local camera");
      local = view; camera.Apply(local, view, mouse, 3488, true);
      Check(std::abs(local.CameraX - paused) < 2, "resume applies mouse motion from an inactive view");
      camera.Reset(); camera.Export(mouse, true); Check(!mouse.ViewValid && !mouse.CursorValid, "new match retains the previous local camera");
      view.ViewMode = 0; view.MouseScale = 0; view.ControlledActor = 11; view.CameraX = 1000; view.CameraTargetX = 1500;
      auto body = first.Nodes[0]; body.X = 2000; body.Y = 300; view.Nodes = {body}; mouse = {}; mouse.Held = uint64_t(1) << RTE::INPUT_PREV;
      local = view; camera.Apply(local, view, mouse, 4000); local = view; camera.Apply(local, view, mouse, 4251);
      Check(local.ViewMode == 3, "held-Q free flight waits for the host's mode change");
      mouse.MouseX = 300; local = view; camera.Apply(local, view, mouse, 4267); camera.Export(mouse, true);
      Check(mouse.CursorX == 2300 && mouse.CursorMode == 3, "locally activated Q camera does not own its selection cursor");
      view.ViewMode = 3; view.MouseScale = 1; view.CameraTargetX = 800; view.CameraX = 800;
      local = view; camera.Apply(local, view, mouse, 4283); camera.Export(mouse, true);
      Check(mouse.CursorX == 2300, "host confirmation of Q mode overwrites the local selection target");
      mouse.Held = 0; local = view; camera.Apply(local, view, mouse, 4299);
      Check(local.ViewMode == 0, "releasing Q waits for the host to leave free flight");
      local = view; camera.Apply(local, view, mouse, 4315); Check(local.ViewMode == 0, "late selection-mode snapshots reopen the local Q camera");
    }
    { W::Snapshot lz; lz.ID = 50; lz.Width = 640; lz.Height = 360; lz.SceneWidth = 4096; lz.SceneHeight = 1024; lz.ViewMode = W::LocalCamera::LandingZoneMode; lz.MouseScale = 1;
      lz.CameraX = lz.CameraTargetX = 1000; lz.CameraY = lz.CameraTargetY = 100;
      W::Node anchor; anchor.ID = (uint64_t(1) << 63) | 1; anchor.Type = W::Shape::Rectangle; anchor.Flags = W::ScreenSpace; anchor.Control = W::Interaction::WorldCursor; anchor.X = 320; anchor.Y = 200; lz.Nodes = {anchor};
      W::LocalCamera camera; Input mouse; auto local = lz; camera.Apply(local, lz, mouse, 1000);
      mouse.MouseX = 50; mouse.MouseY = 40; local = lz; camera.Apply(local, lz, mouse, 1016);
      Input sent; camera.Export(sent, true);
      Check(sent.CursorValid && sent.CursorMode == W::LocalCamera::LandingZoneMode && sent.CursorX == 1370 && sent.CursorY == 300, "landing zone does not move sideways locally or leaves the host's terrain height");
      camera.RecordSent(7); mouse.MouseX = 60; local = lz; camera.Apply(local, lz, mouse, 1032); camera.RecordSent(8);
      // The host limits the zone to an area ending at 1350, answering input 7 and then input 8.
      lz.ID = 51; lz.InputSequence = 7; lz.Nodes[0].X = 350; local = lz; camera.Apply(local, lz, mouse, 1048); camera.Export(sent, true);
      Check(sent.CursorX == 1360, "landing zone ignores the host's landing-area limit or discards motion made since");
      lz.ID = 52; lz.InputSequence = 8; local = lz; camera.Apply(local, lz, mouse, 1064); local = lz; camera.Apply(local, lz, mouse, 1080); camera.Export(sent, true);
      Check(sent.CursorX == 1350, "landing-zone correction is applied twice or not at all"); }
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
	const char* requestedPort = std::getenv("CCCP_MPTEST_PORT");
	const int testPort = requestedPort ? std::atoi(requestedPort) : 38997;
	Check(testPort >= 1024 && testPort <= 65535, "invalid test UDP port");
	const uint16_t port = uint16_t(testPort);
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
		for (const auto& event: discovery.Poll()) if (event.Kind == TransportEvent::Type::Discovered) discovered = event.Data == announcement.Data && event.Address == "127.0.0.1:" + std::to_string(port);
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
	std::array<Transport, 3> guests; Transport overflow; unsigned accepted = 0; bool full = false;
	for (auto& guest: guests) Check(guest.Start(false, 0, "", error) && guest.Connect("127.0.0.1", port, "room-secret", error), "additional player connection failed");
	const auto roomDeadline = Now() + 3000;
	while (Now() < roomDeadline && accepted < 3) { host.Poll(); for (auto& guest: guests) for (const auto& event: guest.Poll()) if (event.Kind == TransportEvent::Type::Connected) ++accepted; std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
	Check(accepted == 3, "dedicated transport did not admit all four remote player slots");
	Check(overflow.Start(false, 0, "", error) && overflow.Connect("127.0.0.1", port, "room-secret", error), "overflow setup failed");
	const auto overflowDeadline = Now() + 3000;
	while (Now() < overflowDeadline && !full) { host.Poll(); for (const auto& event: overflow.Poll()) if (event.Kind == TransportEvent::Type::Failed) full = event.Error == "This room is full."; std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
	Check(full, "fifth remote player was not rejected from the full dedicated transport");
	std::vector<uint8_t> oversized(1401, PacketID); Check(!client.Send(hostAddress, oversized, Delivery::State), "oversized datagram sent");
	client.Close(hostAddress);
	const auto closeDeadline = Now() + 3000;
	while (Now() < closeDeadline && !disconnected) { for (const auto& event: host.Poll()) if (event.Kind == TransportEvent::Type::Disconnected && event.Address == clientAddress) disconnected = true; client.Poll(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
	Check(disconnected, "disconnect not delivered");
	for (int cycle = 0; cycle < 20; ++cycle) { Check(client.Start(false, 0, "", error), "transport restart failed"); client.Stop(); }
}
void Deltas() {
    namespace W = World;
    W::Snapshot base; base.ID = 10; base.Time = 1000; base.Width = base.SceneWidth = 640; base.Height = base.SceneHeight = 360;
    for (uint64_t i = 1; i <= 200; ++i) { W::Node n; n.ID = i << 8; n.Asset = 77; n.X = float(i * 3) + .5f; n.Y = 40.25f; n.Angle = .125f; n.Width = n.Height = n.SourceWidth = n.SourceHeight = 8; base.Nodes.push_back(n); }
    Writer full(Kind::WorldSnapshot); W::WriteSnapshot(full, base); Reader fr(full.Data); Header h; W::Snapshot decodedBase;
    Check(ReadHeader(fr, h) && W::ReadSnapshot(fr, decodedBase) && decodedBase.Nodes == base.Nodes, "complete state did not survive quantized encoding");
    auto next = base; next.ID = 11; next.Time = 1050;
    next.Nodes[5].X += 1.75f; next.Nodes[5].Angle = -.5f;       // moved
    next.Nodes.erase(next.Nodes.begin() + 7);                     // removed
    W::Node added; added.ID = 999 << 8; added.Asset = 78; added.X = 12; added.Y = 13; added.Width = added.Height = added.SourceWidth = added.SourceHeight = 4; next.Nodes.push_back(added);
    Writer delta(Kind::WorldSnapshot); W::WriteSnapshot(delta, next, &base);
    Check(delta.Data.size() < full.Data.size() / 5, "delta state was not much smaller than the complete state");
    Check(delta.Data.size() - 144 < 199 * 3 + 40, "unchanged entities cost more than about two bytes each"); // 144: state header with the own-actor motion record
    auto lookup = [&](uint32_t id) -> const W::Snapshot* { return id == decodedBase.ID ? &decodedBase : nullptr; };
    Reader dr(delta.Data); W::Snapshot decoded;
    Check(ReadHeader(dr, h) && W::ReadSnapshot(dr, decoded, lookup) && decoded.Nodes == next.Nodes, "delta state did not reconstruct moved, unchanged, removed and new entities");
    Reader missing(delta.Data); W::Snapshot orphan; Check(ReadHeader(missing, h) && !W::ReadSnapshot(missing, orphan), "delta state decoded without its acknowledged baseline");
    // Quantization is 1/16 pixel; sub-step motion is withheld rather than drifting.
    auto still = next; still.ID = 12; still.Nodes[0].X += .01f; Writer tiny(Kind::WorldSnapshot); W::WriteSnapshot(tiny, still, &next);
    auto lookupNext = [&](uint32_t id) -> const W::Snapshot* { return id == decoded.ID ? &decoded : nullptr; };
    Reader tr(tiny.Data); W::Snapshot quantized; Check(ReadHeader(tr, h) && W::ReadSnapshot(tr, quantized, lookupNext) && quantized.Nodes[0].X == next.Nodes[0].X, "sub-quantum motion changed the reconstructed position");
}
void Adaptation() {
    namespace W = World;
    // Random loss and jitter alone keep the full configured rate.
    W::SendRate rate; rate.Reset(375000, 0); std::mt19937 random(7);
    for (uint64_t t = 0; t < 20000; t += 10) { rate.Sample(120 + random() % 40); rate.Limited(); rate.Update(t, .03); }
    Check(rate.Rate() == rate.Cap(), "random loss and jitter reduced the send rate");
    // A standing queue (rising round trip) backs off to a fraction of the cap.
    for (uint64_t t = 20000, queue = 0; t < 26000; t += 10) { queue = std::min<uint64_t>(250, queue + 1); rate.Sample(120 + queue); rate.Limited(); rate.Update(t); }
    Check(rate.Rate() < rate.Cap() * .5, "a growing queue did not reduce the send rate");
    const double reduced = rate.Rate();
    for (uint64_t t = 26000; t < 36000; t += 10) { rate.Sample(120); rate.Limited(); rate.Update(t); }
    Check(rate.Rate() > reduced * 2, "the send rate did not recover once the queue drained");
    W::SendRate idle; idle.Reset(375000, 0); for (uint64_t t = 0; t < 5000; t += 10) { idle.Sample(80); idle.Update(t); }
    Check(idle.Rate() == W::SendRate::StartRate, "an idle connection grew its rate without demand");

    // Nearest tiles stream first; wrapped layers measure the short way around.
    W::SceneMap map; W::Node near, far, wrapped;
    near.ID = 1; near.Asset = 11; near.X = 300; near.Y = 160; near.Width = near.Height = 64;
    far.ID = 2; far.Asset = 12; far.X = 3000; far.Y = 160; far.Width = far.Height = 64;
    wrapped.ID = 3; wrapped.Asset = 13; wrapped.X = 3900; wrapped.Y = 160; wrapped.Width = wrapped.Height = 64; wrapped.X2 = 4000;
    map.Nodes = {far, wrapped, near};
    const std::vector<uint64_t> assets{12, 13, 99, 11};
    const auto ordered = W::NearestResources(assets, map, 640, 360);
    Check(ordered.size() == 4 && ordered[0] == 11 && ordered[1] == 13 && ordered[2] == 12 && ordered[3] == 99, "scene resources did not stream nearest-first");

    // The presentation delay follows the measured interval and jitter.
    auto adapt = [](unsigned jitter) {
        W::Timeline timeline; std::mt19937 random(3); W::Snapshot state; state.Width = state.SceneWidth = 640; state.Height = state.SceneHeight = 360;
        for (uint32_t i = 1; i <= 80; ++i) { state.ID = i; state.Time = 1000 + i * 50; timeline.Push(state, state.Time + 30 + (jitter ? random() % jitter : 0)); timeline.Sample(state.Time + 30); }
        for (uint64_t t = 5100; t < 9000; t += 16) timeline.Sample(t);
        return timeline.Delay();
    };
    const float steady = adapt(0), jittery = adapt(60);
    Check(steady >= 50 && steady <= 60, "steady 20 Hz delivery did not settle near one interval");
    Check(jittery > steady + 30 && jittery <= float(W::MaximumDelayMS), "jittery delivery did not lengthen the presentation delay");

    // The player's own actor (with its attachments) is shown at the present
    // time while the rest of the world keeps its interpolation buffer.
    W::Timeline own; W::Snapshot state; state.Width = 640; state.Height = 360; state.SceneWidth = 4096; state.SceneHeight = 1024;
    W::Node actor, arm, rock; actor.ID = 7 << 8; arm.ID = (7 << 8) | 1; arm.Parent = actor.ID; rock.ID = 9 << 8;
    for (W::Node* n : {&actor, &arm, &rock}) { n->Type = W::Shape::Rectangle; n->Width = n->Height = 4; }
    state.ControlledActor = actor.ID;
    for (uint32_t i = 0; i < 20; ++i) { state.ID = i + 1; state.Time = 1000 + i * 50; actor.X = 100.0f + i * 10; arm.X = actor.X + 3; rock.X = 500.0f + i * 10; state.Nodes = {actor, arm, rock}; own.Push(state, state.Time + 20); }
    auto find = [](const W::Snapshot& s, uint64_t id) { for (const auto& n : s.Nodes) if (n.ID == id) return n.X; return -1.0f; };
    auto sampled = own.Sample(1980);
    Check(std::abs(find(sampled, actor.ID) - 292) < .01f && std::abs(find(sampled, arm.ID) - 295) < .01f, "own actor and attachments are not shown at the present time");
    Check(std::abs(find(sampled, rock.ID) - 677) < .01f, "own-actor presentation moved the rest of the world");
    // The actor stopped; the continued position fades back instead of jumping.
    state.ID = 21; state.Time = 2000; actor.X = 290; arm.X = 293; rock.X = 700; state.Nodes = {actor, arm, rock}; own.Push(state, 2020);
    sampled = own.Sample(2025); Check(std::abs(find(sampled, actor.ID) - 292) < .01f, "a new state made the own actor jump");
    sampled = own.Sample(2125); Check(find(sampled, actor.ID) > 290 && find(sampled, actor.ID) < 291, "the own actor did not settle on the host's position");
}
}
void Motion() {
    namespace W = World;
    // A floor at y = 100 and a wall from x = 200.
    const W::LocalMotion::Solid solid = [](float x, float y) { return y >= 100 || x >= 200 ? 1 : 0; };
    W::Snapshot host; host.ID = 1; host.Time = 1000; host.SceneWidth = 2000; host.SceneHeight = 400; host.ControlledActor = 7 << 8; host.MotionFlags = W::MotionPredictable;
    host.ActorX = 50; host.ActorY = 70; host.Gravity = 600; host.SimSpeed = 100; host.InputSequence = 1;
    W::LocalMotion motion; W::LocalMotion::Control still, right; right.Move = 1;
    float x = 0, y = 0; uint64_t now = 5000; uint32_t sequence = 1;
    auto frames = [&](const W::LocalMotion::Control& control, int count) { for (int i = 0; i < count; ++i) { now += 16; motion.Update(host, now, sequence, control, solid, x, y); } };
    motion.Update(host, now, sequence, still, solid, x, y);
    host.ID = 2; host.Time = 1050; now += 16;
    Check(motion.Update(host, now, sequence, still, solid, x, y) && std::abs(x - 50) < .01f && std::abs(y - 70) < .01f, "own-movement prediction does not start from a standing host state");
    // The host shows the actor walking at 90 px/s while right is held: that gait's speed.
    sequence = 2; frames(right, 1);
    host.ID = 3; host.Time = 1100; host.InputSequence = 2; host.InputAge = 0; host.ActorVelX = 90; frames(right, 1);
    const float walking = x; frames(right, 6);
    Check(x - walking > 7 && x - walking < 10 && std::abs(y - 70) < .01f, "own movement waits for the host instead of following the player's input");
    frames(still, 8); const float stopped = x; frames(still, 4);
    Check(std::abs(x - stopped) < .15f, "own movement keeps sliding after the player lets go");
    // A host state that disagrees moves the drawn actor gradually.
    host.ID = 4; host.Time = 1150; host.InputSequence = 2; host.InputAge = 300; host.ActorX = stopped - 10; host.ActorVelX = 0;
    now += 16; motion.Update(host, now, sequence, still, solid, x, y);
    Check(std::abs(x - stopped) < 2 && motion.LastCorrection() > 9, "a host correction makes the own actor jump");
    frames(still, 40); Check(std::abs(x - (stopped - 10)) < .5f, "own movement does not settle on the host's position");
    frames(right, 200); Check(x > 185 && x < 194, "own movement walks through a wall");
    // Falling from a height lands on the floor.
    W::LocalMotion fall; host.ID = 10; host.ControlledActor = 8 << 8; host.ActorX = 50; host.ActorY = 70; host.ActorVelX = host.ActorVelY = 0; host.InputSequence = 3; sequence = 3;
    fall.Update(host, now, sequence, still, solid, x, y);
    host.ID = 11; host.ActorY = 0; host.Time = 1200; host.InputAge = 0;
    for (int i = 0; i < 90; ++i) { now += 16; fall.Update(host, now, sequence, still, solid, x, y); }
    Check(std::abs(y - 70) < 1, "own movement falls through the floor or never lands");
}
int main() {
	try { Wire(); Inputs(); Worlds(); Deltas(); Adaptation(); Motion(); TransportLoopback(); std::cout << "PASS: retained world resources, 60 Hz interpolation from 20 Hz state, camera and scene wrapping, lifecycle, bounded loss continuation, malformed scene packets, all controls, real UDP, password rejection, discovery, full rooms and reconnect\n"; return 0; }
	catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
