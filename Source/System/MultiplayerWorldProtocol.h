#pragma once

#include "MultiplayerProtocol.h"
#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace RTE::MP::World {
// The wire contains retained scene resources and presentation state, never a
// completed player framebuffer. One resource can be reused by many entities.
inline constexpr uint32_t MaxPayload = 4 * 1024 * 1024;
inline constexpr uint32_t MaxPackedPayload = MaxPayload + MaxPayload / 255 + 64;
inline constexpr size_t MaxNodes = 32768;
inline constexpr uint32_t SnapshotIntervalMS = 50;
inline constexpr uint32_t InterpolationMS = 75;
inline constexpr uint32_t ExtrapolationMS = 100;
inline constexpr uint16_t TileSize = 64;
inline constexpr uint64_t MaxTime = uint64_t(std::numeric_limits<int64_t>::max()) / 4;

enum class Shape : uint8_t { Sprite, Pixel, Line, Rectangle, Triangle, Circle, CircleOutline, Ellipse, EllipseOutline, Arc, Spline, RoundedRectangle, RoundedOutline, Flash, PixelPath };
enum Flags : uint8_t { ScreenSpace = 1, Masked = 2, FlipX = 4, FlipY = 8, SolidColor = 16, Discontinuous = 32, Additive = 64, Layer = 128 };
enum class Interaction : uint8_t { None, Aim, RadialCursor, Pointer, WorldCursor, WorldOverlay, RadialBackground };
struct Node {
    uint64_t ID = 0, Asset = 0, Parent = 0;
    uint64_t StartTime = 0, EndTime = 0;
    Shape Type = Shape::Sprite;
    Interaction Control = Interaction::None;
    uint8_t Flags = Masked, Color = 0, Alpha = 255;
    uint8_t BlendMode = 0, TintR = 255, TintG = 255, TintB = 255;
    float X = 0, Y = 0, Angle = 0, Width = 1, Height = 1;
    float PivotX = 0, PivotY = 0, SourceX = 0, SourceY = 0, SourceWidth = 0, SourceHeight = 0;
    float X2 = 0, Y2 = 0, X3 = 0, Y3 = 0;
    uint16_t ClipX = 0, ClipY = 0, ClipWidth = 65535, ClipHeight = 65535;
    // A native bullet trail is one raster path, including gaps and ricochets.
    // Keeping its exact pixels avoids thousands of full scene nodes per shot.
    std::vector<std::pair<int16_t, int16_t>> Pixels;
    std::vector<uint8_t> PixelColors;
    bool operator==(const Node&) const = default;
};
struct Snapshot {
    uint32_t ID = 0, InputSequence = 0;
    uint32_t MouseX = 0, MouseY = 0;
    uint64_t Time = 0;
    uint16_t Width = 640, Height = 360, SceneWidth = 0, SceneHeight = 0;
    uint8_t Phase = 4; // Native ActivityState, from NotStarted (0) to Over (6).
    bool Paused = false;
    uint8_t Wrap = 0;
    float CameraX = 0, CameraY = 0;
    uint64_t ControlledActor = 0;
    float AimX = 0, AimY = 0, LookX = 0, LookY = 0;
    uint8_t ViewMode = 0;
    float CameraTargetX = 0, CameraTargetY = 0, MouseScale = 0, ScrollSpeed = 0.1f;
    std::vector<Node> Nodes;
};
inline size_t NodeBytes(const Node& node) { return 117 + (node.Type == Shape::PixelPath ? 2 + 5 * node.Pixels.size() : 0); }
// Under an extreme particle load, preserve the scene and controls instead of
// rejecting every oversized update. Selection retains native painter order.
inline void BoundSnapshot(Snapshot& snapshot) {
    size_t bytes = 256;
    for (const auto& node : snapshot.Nodes) bytes += NodeBytes(node);
    if (snapshot.Nodes.size() <= MaxNodes && bytes <= MaxPayload) return;
    std::vector<bool> keep(snapshot.Nodes.size()); size_t count = 0; bytes = 256;
    auto priority = [](const Node& node) {
        if (node.Flags & (ScreenSpace | Layer)) return 0;
        if (node.Type == Shape::Sprite && !(node.Flags & Discontinuous)) return 1;
        return node.Type == Shape::PixelPath || (node.Flags & Discontinuous) ? 3 : 2;
    };
    for (int pass = 0; pass < 4; ++pass) for (size_t i = 0; i < snapshot.Nodes.size(); ++i) {
        const auto& node = snapshot.Nodes[i]; const size_t size = NodeBytes(node);
        if (priority(node) != pass || count == MaxNodes || bytes + size > MaxPayload) continue;
        keep[i] = true; ++count; bytes += size;
    }
    size_t dest = 0;
    for (size_t i = 0; i < snapshot.Nodes.size(); ++i) if (keep[i]) { if (dest != i) snapshot.Nodes[dest] = std::move(snapshot.Nodes[i]); ++dest; }
    snapshot.Nodes.resize(dest);
}
struct Resource {
    uint64_t ID = 0;
    uint16_t Width = 0, Height = 0;
    uint8_t Depth = 8;
    std::vector<uint8_t> Pixels;
};
inline uint64_t Hash(std::span<const uint8_t> bytes, uint64_t seed = 14695981039346656037ULL) {
    for (uint8_t byte : bytes) { seed ^= byte; seed *= 1099511628211ULL; }
    return seed ? seed : 1;
}
inline bool Coordinate(float n) { return std::isfinite(n) && std::abs(n) <= 1000000; }
inline bool Valid(const Node& node) {
    if (!node.ID || uint8_t(node.Control) > uint8_t(Interaction::RadialBackground) || uint8_t(node.Type) > uint8_t(Shape::PixelPath) || node.BlendMode > 11 || node.EndTime > MaxTime || (!node.StartTime && node.EndTime) || (node.StartTime && (node.EndTime <= node.StartTime || node.EndTime - node.StartTime > 1000))) return false;
    if (node.Type == Shape::PixelPath ? (node.Pixels.empty() || node.Pixels.size() > 16384 || node.Pixels.size() != node.PixelColors.size()) : (!node.Pixels.empty() || !node.PixelColors.empty())) return false;
    const float values[]{node.X, node.Y, node.Angle, node.Width, node.Height, node.PivotX, node.PivotY, node.SourceX, node.SourceY, node.SourceWidth, node.SourceHeight, node.X2, node.Y2, node.X3, node.Y3};
    for (float value : values) if (!Coordinate(value)) return false;
    return node.Type != Shape::Sprite || (node.Asset && node.Width >= 0 && node.Height >= 0 && node.SourceWidth > 0 && node.SourceHeight > 0);
}
inline void WriteNode(Writer& writer, const Node& n) {
    writer.U64(n.ID); writer.U64(n.Asset); writer.U64(n.Parent); writer.U8(uint8_t(n.Type)); writer.U8(uint8_t(n.Control)); writer.U8(n.Flags); writer.U8(n.Color); writer.U8(n.Alpha);
    writer.U64(n.StartTime); writer.U64(n.EndTime);
    writer.U8(n.BlendMode); writer.U8(n.TintR); writer.U8(n.TintG); writer.U8(n.TintB);
    for (float value : {n.X, n.Y, n.Angle, n.Width, n.Height, n.PivotX, n.PivotY, n.SourceX, n.SourceY, n.SourceWidth, n.SourceHeight, n.X2, n.Y2, n.X3, n.Y3}) writer.F32(value);
    writer.U16(n.ClipX); writer.U16(n.ClipY); writer.U16(n.ClipWidth); writer.U16(n.ClipHeight);
    if (n.Type == Shape::PixelPath) {
        writer.U16(uint16_t(n.Pixels.size()));
        for (size_t i = 0; i < n.Pixels.size(); ++i) { writer.U16(uint16_t(n.Pixels[i].first)); writer.U16(uint16_t(n.Pixels[i].second)); writer.U8(i < n.PixelColors.size() ? n.PixelColors[i] : n.Color); }
    }
}
inline bool ReadNode(Reader& reader, Node& n) {
    uint8_t shape, control;
    if (!reader.U64(n.ID) || !reader.U64(n.Asset) || !reader.U64(n.Parent) || n.Parent == n.ID || !reader.U8(shape) || !reader.U8(control) || !reader.U8(n.Flags) || !reader.U8(n.Color) || !reader.U8(n.Alpha)) return false;
    n.Type = Shape(shape);
    n.Control = Interaction(control);
    if (!reader.U64(n.StartTime) || !reader.U64(n.EndTime)) return false;
    if (!reader.U8(n.BlendMode) || !reader.U8(n.TintR) || !reader.U8(n.TintG) || !reader.U8(n.TintB)) return false;
    for (float* value : {&n.X, &n.Y, &n.Angle, &n.Width, &n.Height, &n.PivotX, &n.PivotY, &n.SourceX, &n.SourceY, &n.SourceWidth, &n.SourceHeight, &n.X2, &n.Y2, &n.X3, &n.Y3}) if (!reader.F32(*value)) return false;
    if (!reader.U16(n.ClipX) || !reader.U16(n.ClipY) || !reader.U16(n.ClipWidth) || !reader.U16(n.ClipHeight)) return false;
    n.Pixels.clear(); n.PixelColors.clear();
    if (n.Type == Shape::PixelPath) {
        uint16_t count; if (!reader.U16(count) || !count || count > 16384 || count > reader.Remaining() / 5) return false;
        n.Pixels.reserve(count); n.PixelColors.reserve(count);
        for (unsigned i = 0; i < count; ++i) { uint16_t x, y; uint8_t color; if (!reader.U16(x) || !reader.U16(y) || !reader.U8(color)) return false; n.Pixels.emplace_back(std::bit_cast<int16_t>(x), std::bit_cast<int16_t>(y)); n.PixelColors.push_back(color); }
    }
    return Valid(n);
}
inline void WriteSnapshot(Writer& writer, const Snapshot& s) {
    writer.U32(s.ID); writer.U32(s.InputSequence); writer.U64(s.Time); writer.U16(s.Width); writer.U16(s.Height); writer.U16(s.SceneWidth); writer.U16(s.SceneHeight); writer.U8(s.Wrap); writer.U8(s.Phase);
    writer.F32(s.CameraX); writer.F32(s.CameraY); writer.U64(s.ControlledActor); writer.U32(s.MouseX); writer.U32(s.MouseY);
    writer.F32(s.AimX); writer.F32(s.AimY); writer.F32(s.LookX); writer.F32(s.LookY);
    writer.U8(s.Paused); writer.U8(s.ViewMode); writer.F32(s.CameraTargetX); writer.F32(s.CameraTargetY); writer.F32(s.MouseScale); writer.F32(s.ScrollSpeed); writer.U32(uint32_t(s.Nodes.size()));
    for (const auto& n : s.Nodes) WriteNode(writer, n);
}
inline bool ReadSnapshot(Reader& reader, Snapshot& s) {
    uint32_t count;
    if (!reader.U32(s.ID) || !reader.U32(s.InputSequence) || !reader.U64(s.Time) || !reader.U16(s.Width) || !reader.U16(s.Height) || !reader.U16(s.SceneWidth) || !reader.U16(s.SceneHeight) || !reader.U8(s.Wrap) || !reader.U8(s.Phase) || !reader.F32(s.CameraX) || !reader.F32(s.CameraY) || !reader.U64(s.ControlledActor) || !reader.U32(s.MouseX) || !reader.U32(s.MouseY) || !reader.F32(s.AimX) || !reader.F32(s.AimY) || !reader.F32(s.LookX) || !reader.F32(s.LookY)) return false;
    uint8_t paused;
    if (!reader.U8(paused) || paused > 1 || !reader.U8(s.ViewMode) || !reader.F32(s.CameraTargetX) || !reader.F32(s.CameraTargetY) || !reader.F32(s.MouseScale) || !reader.F32(s.ScrollSpeed) || !reader.U32(count)) return false;
    s.Paused = paused;
    if (s.ViewMode > 20 || !Coordinate(s.CameraTargetX) || !Coordinate(s.CameraTargetY) || s.MouseScale < 0 || s.MouseScale > 2 || s.ScrollSpeed < 0 || s.ScrollSpeed > 1) return false;
    if (!Coordinate(s.AimX) || !Coordinate(s.AimY) || std::abs(s.AimX) > 1 || std::abs(s.AimY) > 1 || !Coordinate(s.LookX) || !Coordinate(s.LookY)) return false;
    if (s.Phase > 6) return false;
    if (!s.ID || !s.Time || s.Time > MaxTime || s.Width < 320 || s.Width > 3840 || s.Height < 180 || s.Height > 2160 || !s.SceneWidth || !s.SceneHeight || s.SceneWidth > 16384 || s.SceneHeight > 16384 || s.Wrap > 3 || !Coordinate(s.CameraX) || !Coordinate(s.CameraY) || count > MaxNodes || count > reader.Remaining() / 117) return false;
    s.Nodes.resize(count); std::unordered_set<uint64_t> ids;
    for (auto& n : s.Nodes) if (!ReadNode(reader, n) || !ids.insert(n.ID).second) return false;
    return reader.Done();
}
inline uint64_t ResourceHash(const Resource& r) {
    Writer metadata(Kind::WorldResource); metadata.U16(r.Width); metadata.U16(r.Height); metadata.U8(r.Depth);
    return Hash(r.Pixels, Hash(metadata.Data));
}
inline void WriteResource(Writer& writer, const Resource& r) {
    writer.U64(r.ID); writer.U16(r.Width); writer.U16(r.Height); writer.U8(r.Depth); writer.U32(uint32_t(r.Pixels.size()));
    writer.Data.insert(writer.Data.end(), r.Pixels.begin(), r.Pixels.end());
}
inline bool ReadResource(Reader& reader, Resource& r) {
    uint32_t size;
    if (!reader.U64(r.ID) || !reader.U16(r.Width) || !reader.U16(r.Height) || !reader.U8(r.Depth) || !reader.U32(size)) return false;
    if (!r.ID || !r.Width || !r.Height || r.Width > 4096 || r.Height > 4096 || (r.Depth != 8 && r.Depth != 32) || size > MaxPayload || size != uint64_t(r.Width) * r.Height * (r.Depth / 8) || size != reader.Remaining()) return false;
    r.Pixels.assign(reader.Rest().begin(), reader.Rest().end());
    return ResourceHash(r) == r.ID;
}
inline void WriteManifest(Writer& writer, std::span<const uint64_t> assets) {
    writer.U32(uint32_t(assets.size())); for (auto id : assets) writer.U64(id);
}
inline bool ReadManifest(Reader& reader, std::unordered_set<uint64_t>& assets) {
    uint32_t count; if (!reader.U32(count) || count > MaxPayload / 8 || reader.Remaining() != uint64_t(count) * 8) return false;
    assets.clear();
    for (uint32_t i = 0; i < count; ++i) { uint64_t id; if (!reader.U64(id) || !id || !assets.insert(id).second) return false; }
    return reader.Done();
}

// Complete snapshots make loss/reordering self-repairing. Chunks are bounded,
// and resources use a separate reliable assembly rather than blocking poses.
struct Chunk { uint32_t ID = 0, Size = 0; uint16_t Index = 0; std::span<const uint8_t> Bytes; bool Parity = false; };
// Transport ACKs from a broker do not mean its destination has the resource.
// Only a receipt from the guest releases these bounded end-to-end credits.
class ResourceWindow {
public:
    static constexpr size_t Limit = 128 * 1024;
    bool CanSend(size_t bytes) const { return bytes && bytes <= 1400 && m_Bytes + bytes <= Limit; }
    void Sent(uint32_t message, uint16_t index, size_t bytes) {
        if (CanSend(bytes) && m_Packets.try_emplace(Key(message, index), bytes).second) m_Bytes += bytes;
    }
    bool Receipt(uint32_t message, uint16_t index) {
        auto packet = m_Packets.find(Key(message, index)); if (packet == m_Packets.end()) return false;
        m_Bytes -= packet->second; m_Packets.erase(packet); return true;
    }
    bool Contains(uint32_t message) const { return std::any_of(m_Packets.begin(), m_Packets.end(), [message](const auto& p) { return p.first >> 16 == message; }); }
    size_t Bytes() const { return m_Bytes; }
    void Reset() { m_Packets.clear(); m_Bytes = 0; }
private:
    static uint64_t Key(uint32_t message, uint16_t index) { return (uint64_t(message) << 16) | index; }
    std::unordered_map<uint64_t, size_t> m_Packets;
    size_t m_Bytes = 0;
};
// Limit reliable repair traffic independently of the number of missing tiles.
// Oldest requests go first so a large map cannot starve its last missing asset.
class ResourceRequests {
public:
    std::vector<uint64_t> Select(const std::unordered_set<uint64_t>& missing, uint64_t now) {
        std::erase_if(m_Last, [&](const auto& request) { return !missing.contains(request.first); });
        std::vector<std::pair<uint64_t, uint64_t>> eligible;
        for (auto id : missing) {
            const auto found = m_Last.find(id);
            if (found == m_Last.end() || now - found->second >= 5000) eligible.emplace_back(found == m_Last.end() ? 0 : found->second, id);
        }
        std::sort(eligible.begin(), eligible.end());
        std::vector<uint64_t> selected;
        for (size_t i = 0; i < std::min<size_t>(64, eligible.size()); ++i) { selected.push_back(eligible[i].second); m_Last[eligible[i].second] = now; }
        return selected;
    }
    void Reset() { m_Last.clear(); }
private:
    std::unordered_map<uint64_t, uint64_t> m_Last;
};
inline void WriteChunk(Writer& writer, const Chunk& c) {
    writer.U32(c.ID); writer.U32(c.Size); writer.U16(c.Index); writer.U8(c.Parity); writer.Data.insert(writer.Data.end(), c.Bytes.begin(), c.Bytes.end());
}
inline bool ValidChunk(const Chunk& c) {
    if (!c.ID || !c.Size || c.Size > MaxPackedPayload) return false;
    const uint32_t count = (c.Size + ChunkBytes - 1) / ChunkBytes;
    if (c.Parity) return c.Index < (count + ParityGroup - 1) / ParityGroup && c.Bytes.size() == ChunkBytes;
    return c.Index < count && c.Bytes.size() == std::min<uint32_t>(ChunkBytes, c.Size - uint32_t(c.Index) * ChunkBytes);
}
inline bool ReadChunk(Reader& reader, Chunk& c) {
    uint8_t parity;
    if (!reader.U32(c.ID) || !reader.U32(c.Size) || !reader.U16(c.Index) || !reader.U8(parity) || parity > 1) return false;
    c.Parity = parity != 0; c.Bytes = reader.Rest(); return ValidChunk(c);
}
class Assembler {
public:
    explicit Assembler(uint64_t expiryMS = 5000) : m_ExpiryMS(expiryMS) {}
    std::optional<std::vector<uint8_t>> Push(const Chunk& c, uint64_t now) {
        if (!ValidChunk(c)) return {};
        std::erase_if(m_Pending, [this, now](const Pending& p) { return now - p.Start > m_ExpiryMS; });
        auto found = std::find_if(m_Pending.begin(), m_Pending.end(), [&](const Pending& p) { return p.ID == c.ID; });
        if (found == m_Pending.end()) {
            if (m_Pending.size() >= 4) m_Pending.pop_front();
            m_Pending.push_back({c.ID, now, std::vector<uint8_t>(c.Size), std::vector<bool>((c.Size + ChunkBytes - 1) / ChunkBytes), {}, 0}); found = std::prev(m_Pending.end());
        }
        if (found->Bytes.size() != c.Size) return {};
        if (c.Parity) found->Parity.try_emplace(c.Index, c.Bytes.begin(), c.Bytes.end());
        else if (!found->Have[c.Index]) { std::copy(c.Bytes.begin(), c.Bytes.end(), found->Bytes.begin() + size_t(c.Index) * ChunkBytes); found->Have[c.Index] = true; ++found->Received; }
        const uint16_t group = c.Parity ? c.Index : c.Index / ParityGroup;
        if (auto parity = found->Parity.find(group); parity != found->Parity.end()) {
            const size_t first = size_t(group) * ParityGroup, last = std::min(first + ParityGroup, found->Have.size()); size_t missing = 0, index = 0;
            for (size_t i = first; i < last; ++i) if (!found->Have[i]) { ++missing; index = i; }
            if (missing == 1) {
                auto recovered = parity->second;
                for (size_t i = first; i < last; ++i) if (i != index) for (size_t j = 0; j < std::min<size_t>(ChunkBytes, found->Bytes.size() - i * ChunkBytes); ++j) recovered[j] ^= found->Bytes[i * ChunkBytes + j];
                std::copy_n(recovered.begin(), std::min<size_t>(ChunkBytes, found->Bytes.size() - index * ChunkBytes), found->Bytes.begin() + index * ChunkBytes); found->Have[index] = true; ++found->Received;
            }
        }
        if (found->Received != found->Have.size()) return {};
        auto complete = std::move(found->Bytes); m_Pending.erase(found); return complete;
    }
    void Reset() { m_Pending.clear(); }
private:
    struct Pending { uint32_t ID; uint64_t Start; std::vector<uint8_t> Bytes; std::vector<bool> Have; std::unordered_map<uint16_t, std::vector<uint8_t>> Parity; size_t Received; };
    std::deque<Pending> m_Pending;
    uint64_t m_ExpiryMS;
};
inline float Displacement(float from, float to, float span, bool wrap) {
    float d = to - from;
    if (wrap && span > 0) d -= std::round(d / span) * span;
    return d;
}
inline float Blend(float from, float to, float alpha, float span = 0, bool wrap = false) { return from + Displacement(from, to, span, wrap) * alpha; }
// Predict presentation only. The host remains responsible for projectile hits,
// terrain, actor movement, inventory, scripts and AI.
inline void PredictLocalView(Snapshot& sampled, const Snapshot& latest, float aimX, float aimY, bool radial) {
    if (latest.ViewMode != 0 && !radial) return;
    if (!latest.ControlledActor || aimX * aimX + aimY * aimY < 0.01f) return;
    auto delta = [&](const Snapshot& state) {
        return state.AimX * state.AimX + state.AimY * state.AimY >= 0.01f ?
            Displacement(std::atan2(state.AimY, state.AimX), std::atan2(aimY, aimX), 6.28318530718f, true) : 0.0f;
    };
    const float hudAngle = delta(latest), c = std::cos(hudAngle), s = std::sin(hudAngle);
    for (auto& n : sampled.Nodes) if ((n.Flags & ScreenSpace) && n.Parent == latest.ControlledActor &&
        ((n.Control == Interaction::Aim && !radial) || (n.Control == Interaction::RadialCursor && radial))) {
        auto rotate = [&](float& x, float& y) { const float oldX = x; x = x * c - y * s; y = oldX * s + y * c; };
        rotate(n.X, n.Y); n.Angle -= hudAngle;
        if (n.Type == Shape::Line || n.Type == Shape::Triangle || n.Type == Shape::Spline) { rotate(n.X2, n.Y2); rotate(n.X3, n.Y3); }
        if (n.Type == Shape::Spline) rotate(n.Width, n.Height);
    }
    if (radial || latest.ViewMode != 0 || sampled.ViewMode != 0 || sampled.ControlledActor != latest.ControlledActor || latest.LookX * latest.LookX + latest.LookY * latest.LookY == 0) return;
    const float angle = delta(sampled), cc = std::cos(angle), ss = std::sin(angle);
    float dx = sampled.LookX * cc - sampled.LookY * ss - sampled.LookX;
    float dy = sampled.LookX * ss + sampled.LookY * cc - sampled.LookY;
    const float length = std::hypot(dx, dy); if (length > 96) { dx *= 96 / length; dy *= 96 / length; }
    const float previousX = sampled.CameraX, previousY = sampled.CameraY;
    sampled.CameraX += dx; sampled.CameraY += dy;
    if (!(sampled.Wrap & 1)) sampled.CameraX = std::clamp(sampled.CameraX, 0.0f, float(std::max(0, int(sampled.SceneWidth) - int(sampled.Width))));
    if (!(sampled.Wrap & 2)) sampled.CameraY = std::clamp(sampled.CameraY, 0.0f, float(std::max(0, int(sampled.SceneHeight) - int(sampled.Height))));
    dx = sampled.CameraX - previousX; dy = sampled.CameraY - previousY;
    for (auto& n : sampled.Nodes) if ((n.Flags & Layer) && (n.Flags & ScreenSpace)) { n.X -= dx * n.X3; n.Y -= dy * n.Y3; }
}
inline void PredictLocalPointer(Snapshot& sampled, const Snapshot& latest, uint32_t mouseX, uint32_t mouseY) {
    const float dx = float(std::clamp(std::bit_cast<int32_t>(mouseX - latest.MouseX), -2048, 2048));
    const float dy = float(std::clamp(std::bit_cast<int32_t>(mouseY - latest.MouseY), -2048, 2048));
    for (auto& n : sampled.Nodes) if (n.Control == Interaction::Pointer && (n.Flags & ScreenSpace)) {
        n.X = std::clamp(n.X + dx, -1.0f, float(sampled.Width - 2)); n.Y = std::clamp(n.Y + dy, -1.0f, float(sampled.Height - 2));
    }
}
// Persistent camera integration prevents a mouse ACK from withdrawing a visual
// offset while the native camera is still easing towards that same target.
class LocalCamera {
public:
    void Reset() { m_Time = 0; }
    void Apply(Snapshot& sampled, const Snapshot& latest, const Input& input, uint64_t now) {
        if (latest.MouseScale <= 0 || input.Device != 1) { Reset(); return; }
        if (!m_Time || m_Mode != latest.ViewMode || now - m_Time > 250) { m_X = latest.CameraX; m_Y = latest.CameraY; m_Time = now > 16 ? now - 16 : now; m_Mode = latest.ViewMode; }
        const float pendingX = float(std::clamp(std::bit_cast<int32_t>(input.MouseX - latest.MouseX), -2048, 2048)) * latest.MouseScale;
        const float pendingY = float(std::clamp(std::bit_cast<int32_t>(input.MouseY - latest.MouseY), -2048, 2048)) * latest.MouseScale;
        const float progress = std::min(1.0f, latest.ScrollSpeed * std::min<uint64_t>(now - m_Time, 50) * .05f);
        m_Time = now;
        m_X += Displacement(m_X, latest.CameraTargetX + pendingX, latest.SceneWidth, latest.Wrap & 1) * progress;
        m_Y += Displacement(m_Y, latest.CameraTargetY + pendingY, latest.SceneHeight, latest.Wrap & 2) * progress;
        const float oldX = sampled.CameraX, oldY = sampled.CameraY;
        float dx = Displacement(oldX, m_X, sampled.SceneWidth, sampled.Wrap & 1), dy = Displacement(oldY, m_Y, sampled.SceneHeight, sampled.Wrap & 2);
        const float distance = std::hypot(dx, dy); if (distance > 96) { dx *= 96 / distance; dy *= 96 / distance; }
        sampled.CameraX += dx; sampled.CameraY += dy;
        if (!(sampled.Wrap & 1)) sampled.CameraX = std::clamp(sampled.CameraX, 0.0f, float(std::max(0, int(sampled.SceneWidth) - int(sampled.Width))));
        if (!(sampled.Wrap & 2)) sampled.CameraY = std::clamp(sampled.CameraY, 0.0f, float(std::max(0, int(sampled.SceneHeight) - int(sampled.Height))));
        dx = Displacement(oldX, sampled.CameraX, sampled.SceneWidth, sampled.Wrap & 1); dy = Displacement(oldY, sampled.CameraY, sampled.SceneHeight, sampled.Wrap & 2);
        for (auto& n : sampled.Nodes) {
            if ((n.Flags & Layer) && (n.Flags & ScreenSpace)) { n.X -= dx * n.X3; n.Y -= dy * n.Y3; }
            if (n.Control == Interaction::WorldOverlay && (n.Flags & ScreenSpace)) { n.X -= Displacement(latest.CameraX, sampled.CameraX, sampled.SceneWidth, sampled.Wrap & 1); n.Y -= Displacement(latest.CameraY, sampled.CameraY, sampled.SceneHeight, sampled.Wrap & 2); }
            if (n.Control == Interaction::WorldCursor && (n.Flags & ScreenSpace)) {
                const float cx = pendingX - Displacement(latest.CameraX, sampled.CameraX, sampled.SceneWidth, sampled.Wrap & 1);
                const float cy = pendingY - Displacement(latest.CameraY, sampled.CameraY, sampled.SceneHeight, sampled.Wrap & 2);
                n.X += cx; n.Y += cy;
                if (n.Type == Shape::Line || n.Type == Shape::Triangle || n.Type == Shape::Spline) { n.X2 += cx; n.Y2 += cy; n.X3 += cx; n.Y3 += cy; }
            }
        }
    }
private:
    uint64_t m_Time = 0;
    uint8_t m_Mode = 0;
    float m_X = 0, m_Y = 0;
};
class Timeline {
public:
    bool Push(Snapshot snapshot, uint64_t received) {
        if (!snapshot.ID || !snapshot.Time || snapshot.Time > MaxTime || received > MaxTime) return false;
        if (!m_States.empty() && (!Newer(snapshot.ID, m_States.back().ID) || snapshot.Time <= m_States.back().Time)) return false;
        // Minimum arrival offset avoids translating packet jitter into animation.
        const int64_t offset = int64_t(received) - int64_t(snapshot.Time);
        m_Offset = m_States.empty() ? offset : std::min(m_Offset, offset);
        m_States.push_back(std::move(snapshot)); while (m_States.size() > 8) m_States.pop_front(); return true;
    }
    bool Empty() const { return m_States.empty(); }
    const Snapshot& Latest() const { return m_States.back(); }
    std::unordered_set<uint64_t> Resources() const { std::unordered_set<uint64_t> ids; for (const auto& s : m_States) for (const auto& n : s.Nodes) if (n.Asset) ids.insert(n.Asset); return ids; }
    Snapshot Sample(uint64_t now) const {
        if (m_States.empty()) return {};
        const int64_t target = int64_t(now) - m_Offset - InterpolationMS;
        const Snapshot* before = &m_States.front(); const Snapshot* after = before;
        for (const auto& state : m_States) { if (int64_t(state.Time) <= target) before = &state; if (int64_t(state.Time) >= target) { after = &state; break; } after = &state; }
        // After the newest update, a small bounded continuation covers jitter;
        // we freeze rather than invent unbounded motion on a lost connection.
        if (before == after && target > int64_t(after->Time) && m_States.size() > 1) before = &m_States[m_States.size() - 2];
        const float dt = float(after->Time - before->Time);
        const float alpha = dt > 0 ? std::clamp(float(target - int64_t(before->Time)) / dt, 0.0f, 1.0f + ExtrapolationMS / dt) : 1;
        Snapshot result = alpha < 1 ? *before : *after;
        if (before->ControlledActor == after->ControlledActor) {
            result.AimX = Blend(before->AimX, after->AimX, std::min(alpha, 1.0f)); result.AimY = Blend(before->AimY, after->AimY, std::min(alpha, 1.0f));
            result.LookX = Blend(before->LookX, after->LookX, std::min(alpha, 1.0f)); result.LookY = Blend(before->LookY, after->LookY, std::min(alpha, 1.0f));
        }
        if (std::abs(Displacement(before->CameraX, after->CameraX, after->SceneWidth, after->Wrap & 1)) <= 256 && std::abs(Displacement(before->CameraY, after->CameraY, after->SceneHeight, after->Wrap & 2)) <= 256) {
            result.CameraX = Blend(before->CameraX, after->CameraX, alpha, after->SceneWidth, after->Wrap & 1);
            result.CameraY = Blend(before->CameraY, after->CameraY, alpha, after->SceneHeight, after->Wrap & 2);
        }
        std::unordered_map<uint64_t, const Node*> old, next;
        for (const auto& n : before->Nodes) old[n.ID] = &n;
        for (const auto& n : after->Nodes) next[n.ID] = &n;
        for (auto& n : result.Nodes) {
            auto a = old.find(n.ID), b = next.find(n.ID);
            if (a == old.end() || b == next.end()) continue;
            const Node& from = *a->second; const Node& to = *b->second;
            if ((to.Flags & Layer) && from.Type == Shape::Sprite && to.Type == Shape::Sprite && from.Flags == to.Flags && from.X2 == to.X2 && from.Y2 == to.Y2) {
                // Parallax layers wrap at their own bitmap span. Keep each
                // repeated tile in the coordinate cycle of the selected state.
                const float dx = Displacement(from.X, to.X, to.X2, to.X2 > 0), dy = Displacement(from.Y, to.Y, to.Y2, to.Y2 > 0);
                if (std::abs(dx) <= 256 && std::abs(dy) <= 256) {
                    n.X = alpha < 1 ? from.X + dx * alpha : to.X + dx * (alpha - 1);
                    n.Y = alpha < 1 ? from.Y + dy * alpha : to.Y + dy * (alpha - 1);
                }
                continue;
            }
            if ((to.Flags & Discontinuous) || ((to.Flags & ScreenSpace) && !(to.Flags & Layer)) || from.Type != to.Type || from.Parent != to.Parent || ((from.Flags ^ to.Flags) & (FlipX | FlipY)) || std::abs(Displacement(from.X, to.X, after->SceneWidth, after->Wrap & 1)) > 256 || std::abs(Displacement(from.Y, to.Y, after->SceneHeight, after->Wrap & 2)) > 256) continue;
            n.X = Blend(from.X, to.X, alpha, after->SceneWidth, !(to.Flags & ScreenSpace) && (after->Wrap & 1)); n.Y = Blend(from.Y, to.Y, alpha, after->SceneHeight, !(to.Flags & ScreenSpace) && (after->Wrap & 2));
            n.Angle = Blend(from.Angle, to.Angle, alpha, 6.28318530718f, true);
        }
        // Keep attachments on their native parent transforms while interpolating
        // changing joint offsets. World-space interpolation alone separates a
        // limb from a rapidly rotating body between snapshots.
        std::unordered_map<uint64_t, Node*> sampled;
        for (auto& n : result.Nodes) sampled[n.ID] = &n;
        std::unordered_set<uint64_t> resolved, visiting;
        auto resolve = [&](auto&& self, Node& n, unsigned depth) -> void {
            if (!n.Parent || depth > 32 || resolved.contains(n.ID) || visiting.contains(n.ID)) return;
            visiting.insert(n.ID);
            auto a = old.find(n.ID), b = next.find(n.ID), pa = old.find(n.Parent), pb = next.find(n.Parent); auto parent = sampled.find(n.Parent);
            if (a != old.end() && b != next.end() && pa != old.end() && pb != next.end() && parent != sampled.end() && a->second->Parent == b->second->Parent && !(n.Flags & (ScreenSpace | Discontinuous)) && ((pa->second->Flags ^ pb->second->Flags) & FlipX) == 0) {
                self(self, *parent->second, depth + 1);
                const auto& from = *a->second; const auto& to = *b->second; const auto& p0 = *pa->second; const auto& p1 = *pb->second;
                if (std::abs(Displacement(p0.X, p1.X, after->SceneWidth, after->Wrap & 1)) <= 256 && std::abs(Displacement(p0.Y, p1.Y, after->SceneHeight, after->Wrap & 2)) <= 256) {
                    auto local = [&](const Node& child, const Node& p) { const float dx = Displacement(p.X, child.X, after->SceneWidth, after->Wrap & 1), dy = Displacement(p.Y, child.Y, after->SceneHeight, after->Wrap & 2); const float c = std::cos(p.Angle), s = std::sin(p.Angle); return std::pair{dx * c - dy * s, dx * s + dy * c}; };
                    auto l0 = local(from, p0), l1 = local(to, p1); const float x = Blend(l0.first, l1.first, alpha), y = Blend(l0.second, l1.second, alpha); const auto& p = *parent->second; const float c = std::cos(p.Angle), s = std::sin(p.Angle);
                    n.X = p.X + x * c + y * s; n.Y = p.Y - x * s + y * c;
                    n.Angle = p.Angle + Blend(from.Angle - p0.Angle, to.Angle - p1.Angle, alpha, 6.28318530718f, true);
                }
            }
            visiting.erase(n.ID); resolved.insert(n.ID);
        };
        for (auto& n : result.Nodes) resolve(resolve, n, 0);
        std::erase_if(result.Nodes, [](const Node& n) { return n.StartTime != 0; });
        std::unordered_set<uint64_t> events;
        std::vector<Node> trails;
        for (const auto& state : m_States) for (const auto& n : state.Nodes) if (n.StartTime && int64_t(n.StartTime) <= target && target < int64_t(n.EndTime) && events.insert(n.ID).second) trails.push_back(n);
        const auto position = std::find_if(result.Nodes.begin(), result.Nodes.end(), [](const Node& n) { return (!(n.Flags & ScreenSpace) && n.ID < (uint64_t(1) << 59)) || ((n.Flags & Layer) && ((n.ID >> 48) & 0x3fff) >= 101); });
        result.Nodes.insert(position, trails.begin(), trails.end());
        // Native HUD commands are transient and their ordinal IDs can refer to
        // different controls in the next update. Present the latest complete
        // command list without the world's interpolation delay.
        std::erase_if(result.Nodes, [](const Node& n) { return (n.Flags & ScreenSpace) && !(n.Flags & Layer); });
        for (const auto& n : m_States.back().Nodes) if ((n.Flags & ScreenSpace) && !(n.Flags & Layer)) result.Nodes.push_back(n);
        return result;
    }
    void Reset() { m_States.clear(); m_Offset = 0; }
private:
    std::deque<Snapshot> m_States;
    int64_t m_Offset = 0;
};
} // namespace RTE::MP::World
