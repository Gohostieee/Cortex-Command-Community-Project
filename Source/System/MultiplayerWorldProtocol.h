#pragma once

#include "MultiplayerProtocol.h"
#include "Constants.h"
#include <deque>
#include <map>
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
    // Capture-only importance; receivers need only the selected drawing nodes.
    std::unordered_set<uint64_t> CriticalNodes;
};
template <typename Measure>
inline size_t FitSnapshot(Snapshot& snapshot, size_t packedBudget, Measure measure) {
    size_t packedSize = measure(snapshot);
    if (packedSize <= packedBudget) return packedSize;
    auto essential = [&](const Node& n) { return (n.Flags & (ScreenSpace | Layer)) || snapshot.CriticalNodes.contains(n.ID); };
    const size_t optional = std::count_if(snapshot.Nodes.begin(), snapshot.Nodes.end(), [&](const Node& n) { return !essential(n); });
    if (!optional) return packedSize;
    auto original = std::move(snapshot.Nodes);
    snapshot.Nodes.clear();
    for (const auto& node : original) if (essential(node)) snapshot.Nodes.push_back(node);
    const size_t criticalSize = measure(snapshot);
    if (criticalSize >= packedBudget) return criticalSize;
    std::array<size_t, 2> counts{};
    auto rank = [](const Node& node) { return node.Type == Shape::PixelPath ? 0 : 1; };
    for (const auto& node : original) if (!essential(node)) ++counts[rank(node)];
    size_t limit = optional;
    // Fit the actual compressed representation, retaining native painter order.
    // Critical state remains complete even when it alone exceeds the budget.
    for (int attempt = 0; attempt < 6; ++attempt) {
        const size_t next = size_t(double(limit) * double(packedBudget - criticalSize) / double(packedSize - criticalSize) * 0.85);
        limit = attempt == 5 ? 0 : std::min(limit - 1, next);
        snapshot.Nodes.clear(); std::array<size_t, 2> seen{};
        const std::array<size_t, 2> keepCount{std::min(limit, counts[0]), limit > counts[0] ? limit - counts[0] : 0};
        for (const auto& node : original) {
            if (essential(node)) snapshot.Nodes.push_back(node);
            else { const auto group = rank(node); const bool keep = (seen[group] + 1) * keepCount[group] / counts[group] != seen[group] * keepCount[group] / counts[group]; ++seen[group]; if (keep) snapshot.Nodes.push_back(node); }
        }
        packedSize = measure(snapshot);
        if (packedSize <= packedBudget || !limit) break;
    }
    return packedSize;
}
inline size_t NodeBytes(const Node& node) { return 117 + (node.Type == Shape::PixelPath ? 2 + 5 * node.Pixels.size() : 0); }
// Under an extreme particle load, preserve the scene and controls instead of
// rejecting every oversized update. Selection retains native painter order.
inline void BoundSnapshot(Snapshot& snapshot) {
    size_t bytes = 256;
    for (const auto& node : snapshot.Nodes) bytes += NodeBytes(node);
    if (snapshot.Nodes.size() <= MaxNodes && bytes <= MaxPayload) return;
    std::vector<bool> keep(snapshot.Nodes.size()); size_t count = 0; bytes = 256;
    auto priority = [&](const Node& node) {
        if ((node.Flags & Layer) && ((node.ID >> 48) & 0x3fff) == 102) return 0;
        if ((node.Flags & (ScreenSpace | Layer)) || snapshot.CriticalNodes.contains(node.ID)) return 1;
        if (node.Type == Shape::Sprite && !(node.Flags & Discontinuous)) return 2;
        return node.Type == Shape::PixelPath || (node.Flags & Discontinuous) ? 4 : 3;
    };
    for (int pass = 0; pass < 5; ++pass) for (size_t i = 0; i < snapshot.Nodes.size(); ++i) {
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
struct SceneMap {
    float CameraX = 0, CameraY = 0;
    std::vector<Node> Nodes;
};
inline std::vector<uint64_t> ManifestResources(std::span<const uint64_t> baseline, const SceneMap& map) {
    std::vector<uint64_t> assets(baseline.begin(), baseline.end());
    std::unordered_set<uint64_t> known(baseline.begin(), baseline.end());
    for (const auto& node : map.Nodes) if (node.Asset && known.insert(node.Asset).second) assets.push_back(node.Asset);
    return assets;
}
inline unsigned LayerOrdinal(const Node& node) {
    if (!(node.Flags & Layer)) return 0;
    return node.ID & (uint64_t(1) << 62) ? unsigned((node.ID >> 48) & 0x3fff) :
        node.ID & (uint64_t(1) << 61) ? unsigned((node.ID >> 32) & 0xffff) : 0;
}
// The baseline supplies the entire passive map. Pose snapshots update only
// nearby mutable tiles; travelling farther never discards already known tiles.
class RetainedLayers {
public:
    void Reset() { m_Nodes.clear(); }
    void Install(const SceneMap& map) { Reset(); for (const auto& n : map.Nodes) Store(n, map.CameraX, map.CameraY); }
    void Update(const Snapshot& snapshot) { for (const auto& n : snapshot.Nodes) Store(n, snapshot.CameraX, snapshot.CameraY); }
    std::unordered_set<uint64_t> Resources() const { std::unordered_set<uint64_t> assets; for (const auto& [id, n] : m_Nodes) if (n.Asset) assets.insert(n.Asset); return assets; }
    void Compose(Snapshot& snapshot) const {
        if (m_Nodes.empty()) return;
        std::vector<Node> nodes; nodes.reserve(m_Nodes.size() + snapshot.Nodes.size());
        auto append = [&](bool foreground) {
            for (const auto& [id, source] : m_Nodes) if ((LayerOrdinal(source) == 101) == foreground) {
                auto n = source; n.X -= snapshot.CameraX * n.X3; n.Y -= snapshot.CameraY * n.Y3;
                if (n.Type == Shape::Sprite) {
                    const auto near = [](float value, float extent, float span, int viewport) {
                        if (span > 0) { float d = std::fmod(value + extent / 2 - viewport / 2, span); if (d > span / 2) d -= span; if (d < -span / 2) d += span; value = viewport / 2 + d - extent / 2; }
                        return value <= viewport && value + extent >= 0;
                    };
                    if (!near(n.X, n.Width, n.X2, snapshot.Width) || !near(n.Y, n.Height, n.Y2, snapshot.Height)) continue;
                }
                nodes.push_back(std::move(n));
            }
        };
        append(false); bool foreground = false;
        for (auto& n : snapshot.Nodes) {
            const unsigned ordinal = LayerOrdinal(n);
            if (ordinal && ordinal <= 101) continue;
            if (!foreground && ((n.Flags & ScreenSpace) || (n.Flags & Additive))) { append(true); foreground = true; }
            nodes.push_back(std::move(n));
        }
        if (!foreground) append(true);
        snapshot.Nodes = std::move(nodes);
    }
private:
    void Store(Node n, float cameraX, float cameraY) {
        const unsigned ordinal = LayerOrdinal(n); if (!ordinal || ordinal > 101) return;
        n.X += cameraX * n.X3; n.Y += cameraY * n.Y3;
        m_Nodes[n.ID] = std::move(n);
    }
    struct Order {
        bool operator()(uint64_t a, uint64_t b) const {
            const auto ordinal = [](uint64_t id) { return id & (uint64_t(1) << 62) ? (id >> 48) & 0x3fff : (id >> 32) & 0xffff; };
            return ordinal(a) != ordinal(b) ? ordinal(a) < ordinal(b) : a < b;
        }
    };
    std::map<uint64_t, Node, Order> m_Nodes;
};
inline void WriteManifest(Writer& writer, std::span<const uint64_t> assets, const SceneMap& map = {}) {
    writer.U32(uint32_t(assets.size())); for (auto id : assets) writer.U64(id);
    writer.F32(map.CameraX); writer.F32(map.CameraY); writer.U32(uint32_t(map.Nodes.size()));
    for (const auto& node : map.Nodes) WriteNode(writer, node);
}
inline bool ReadManifest(Reader& reader, std::unordered_set<uint64_t>& assets, SceneMap* output = nullptr) {
    uint32_t count; if (!reader.U32(count) || count > MaxPayload / 8 || reader.Remaining() < uint64_t(count) * 8 + 12) return false;
    assets.clear();
    for (uint32_t i = 0; i < count; ++i) { uint64_t id; if (!reader.U64(id) || !id || !assets.insert(id).second) return false; }
    SceneMap map;
    if (!reader.F32(map.CameraX) || !reader.F32(map.CameraY) || !Coordinate(map.CameraX) || !Coordinate(map.CameraY) || !reader.U32(count) || count > MaxNodes || count > reader.Remaining() / 117) return false;
    map.Nodes.resize(count); std::unordered_set<uint64_t> ids;
    for (auto& node : map.Nodes) if (!ReadNode(reader, node) || !(node.Flags & Layer) || !ids.insert(node.ID).second || (node.Asset && !assets.contains(node.Asset))) return false;
    if (output) *output = std::move(map);
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
    if ((latest.ViewMode != 0 || sampled.ViewMode != 0) && !radial) return;
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
}
inline void Translate(Node& n, float dx, float dy) {
    n.X += dx; n.Y += dy;
    if (n.Type == Shape::Line || n.Type == Shape::Triangle || n.Type == Shape::Spline) { n.X2 += dx; n.Y2 += dy; n.X3 += dx; n.Y3 += dy; }
    if (n.Type == Shape::Spline) { n.Width += dx; n.Height += dy; }
}
class LocalPointer {
public:
    void Reset() { *this = LocalPointer(); }
    void Export(Input& input, bool enabled) const { input.PointerValid = m_Ready && enabled; input.PointerX = m_X; input.PointerY = m_Y; }
    void Apply(Snapshot& sampled, const Snapshot& latest, const Input& input, bool enabled) {
        const Node* pointer = nullptr;
        for (const auto& n : latest.Nodes) if (n.Control == Interaction::Pointer && (n.Flags & ScreenSpace)) { pointer = &n; break; }
        if (!pointer) { m_Ready = false; return; }
        if (!m_Ready || m_Mode != latest.ViewMode) {
            m_X = pointer->X2; m_Y = pointer->Y2; m_MouseX = latest.MouseX; m_MouseY = latest.MouseY; m_Mode = latest.ViewMode; m_Ready = true;
        }
        if (enabled) {
            m_X = std::clamp(m_X + float(std::clamp(std::bit_cast<int32_t>(input.MouseX - m_MouseX), -2048, 2048)), 0.0f, float(sampled.Width - 1));
            m_Y = std::clamp(m_Y + float(std::clamp(std::bit_cast<int32_t>(input.MouseY - m_MouseY), -2048, 2048)), 0.0f, float(sampled.Height - 1));
        }
        m_MouseX = input.MouseX; m_MouseY = input.MouseY;
        for (auto& n : sampled.Nodes) if (n.Control == Interaction::Pointer && (n.Flags & ScreenSpace)) Translate(n, m_X - pointer->X2, m_Y - pointer->Y2);
    }
private:
    bool m_Ready = false;
    uint8_t m_Mode = 0;
    uint32_t m_MouseX = 0, m_MouseY = 0;
    float m_X = 0, m_Y = 0;
};
// The native snapshot seeds a new view mode once. Subsequent mouse motion and
// easing belong to the guest; host camera coordinates and ACKs cannot rebase it.
class LocalCamera {
public:
    void Reset() { *this = LocalCamera(); }
    void Apply(Snapshot& sampled, const Snapshot& latest, const Input& input, uint64_t now, bool enabled = true) {
        const bool first = !m_Time;
        const bool pointer = std::any_of(latest.Nodes.begin(), latest.Nodes.end(), [](const Node& n) { return n.Control == Interaction::Pointer; });
        const bool selecting = enabled && !pointer && (input.Held & ((uint64_t(1) << INPUT_NEXT) | (uint64_t(1) << INPUT_PREV)));
        if (selecting && !m_SelectTime) m_SelectTime = now;
        if (!selecting) m_SelectTime = 0;
        uint8_t mode = latest.ViewMode;
        if (latest.ViewMode == 3) m_HostSelectionSeen = true;
        if (latest.ViewMode == 0 && m_HostSelectionSeen) { m_SelectionEnded = false; m_HostSelectionSeen = false; }
        if (latest.ViewMode == 0 && m_SelectTime && now - m_SelectTime >= 250) mode = 3;
        if (m_Mode == 3 && m_WasSelecting && !selecting) m_SelectionEnded = true;
        if (m_SelectionEnded && mode == 3) mode = 0;
        m_WasSelecting = selecting;
        if (first) { m_X = latest.CameraX; m_Y = latest.CameraY; m_MouseX = latest.MouseX; m_MouseY = latest.MouseY; }
        if (first || m_Mode != mode) {
            m_Mode = mode; m_TargetX = latest.CameraTargetX; m_TargetY = latest.CameraTargetY;
            if (mode == 3 && latest.ViewMode != 3) for (const auto& n : sampled.Nodes) if (n.ID == latest.ControlledActor) { m_TargetX = n.X - sampled.Width / 2; m_TargetY = n.Y - sampled.Height / 2; break; }
            m_CursorX = m_TargetX + sampled.Width / 2; m_CursorY = m_TargetY + sampled.Height / 2;
            for (const auto& n : latest.Nodes) if (n.Control == Interaction::WorldCursor) { m_CursorX = latest.CameraX + n.X; m_CursorY = latest.CameraY + n.Y; break; }
            if (!first) { m_MouseX = input.MouseX; m_MouseY = input.MouseY; }
        }
        const float dxInput = enabled ? float(std::clamp(std::bit_cast<int32_t>(input.MouseX - m_MouseX), -2048, 2048)) : 0;
        const float dyInput = enabled ? float(std::clamp(std::bit_cast<int32_t>(input.MouseY - m_MouseY), -2048, 2048)) : 0;
        m_MouseX = input.MouseX; m_MouseY = input.MouseY;
        const float elapsed = first ? 16.0f : float(std::min<uint64_t>(now - m_Time, 50)); m_Time = now;
        const float mouseScale = mode == 3 ? 1.0f : latest.MouseScale;
        if (enabled && mouseScale > 0 && mode != 0) {
            float dx = dxInput * mouseScale, dy = dyInput * mouseScale;
            if (!dx && !dy) {
                const auto held = [&](unsigned bit) { return bool(input.Held & (uint64_t(1) << bit)); };
                const float vx = input.Device == DEVICE_MOUSE_KEYB ? float(held(INPUT_L_RIGHT)) - float(held(INPUT_L_LEFT)) : input.MoveX;
                const float vy = input.Device == DEVICE_MOUSE_KEYB ? float(held(INPUT_L_DOWN)) - float(held(INPUT_L_UP)) : input.MoveY;
                dx = vx * elapsed * .6f * mouseScale; dy = vy * elapsed * .6f * mouseScale;
            }
            m_CursorX += dx; m_CursorY += dy; m_TargetX += dx; m_TargetY += dy;
            Bound(m_CursorX, sampled.SceneWidth, sampled.Wrap & 1, 0); Bound(m_CursorY, sampled.SceneHeight, sampled.Wrap & 2, 0);
            // Preserve the native cursor-to-camera offset at world edges.
            m_TargetX = m_CursorX - sampled.Width / 2; m_TargetY = m_CursorY - sampled.Height / 2;
        } else if (mode == 0 && latest.ControlledActor) {
            for (const auto& n : sampled.Nodes) if (n.ID == latest.ControlledActor) {
                float lx = latest.LookX, ly = latest.LookY;
                if (input.AimX * input.AimX + input.AimY * input.AimY >= .01f && latest.AimX * latest.AimX + latest.AimY * latest.AimY >= .01f) {
                    const float angle = std::atan2(input.AimY, input.AimX) - std::atan2(latest.AimY, latest.AimX);
                    lx = latest.LookX * std::cos(angle) - latest.LookY * std::sin(angle); ly = latest.LookX * std::sin(angle) + latest.LookY * std::cos(angle);
                }
                m_TargetX = n.X + lx - sampled.Width / 2; m_TargetY = n.Y + ly - sampled.Height / 2; break;
            }
        } else if (mouseScale <= 0 && mode != 0) {
            // Death watches and scripted transitions still choose their target.
            m_TargetX = latest.CameraTargetX; m_TargetY = latest.CameraTargetY;
        }
        if (enabled) {
            const float progress = std::min(1.0f, latest.ScrollSpeed * elapsed * .05f);
            m_X += Displacement(m_X, m_TargetX, latest.SceneWidth, latest.Wrap & 1) * progress;
            m_Y += Displacement(m_Y, m_TargetY, latest.SceneHeight, latest.Wrap & 2) * progress;
            Bound(m_X, sampled.SceneWidth, sampled.Wrap & 1, sampled.Width); Bound(m_Y, sampled.SceneHeight, sampled.Wrap & 2, sampled.Height);
        }
        const float oldX = sampled.CameraX, oldY = sampled.CameraY;
        sampled.CameraX = m_X; sampled.CameraY = m_Y;
        sampled.ViewMode = mode;
        const float dx = Displacement(oldX, m_X, sampled.SceneWidth, sampled.Wrap & 1), dy = Displacement(oldY, m_Y, sampled.SceneHeight, sampled.Wrap & 2);
        const Node* cursor = nullptr; for (const auto& n : latest.Nodes) if (n.Control == Interaction::WorldCursor) { cursor = &n; break; }
        for (auto& n : sampled.Nodes) {
            if ((n.Flags & Layer) && (n.Flags & ScreenSpace)) { n.X -= dx * n.X3; n.Y -= dy * n.Y3; }
            if (n.Control == Interaction::WorldOverlay && (n.Flags & ScreenSpace)) Translate(n,
                Displacement(latest.CameraTargetX + sampled.Width / 2, m_CursorX, sampled.SceneWidth, sampled.Wrap & 1) - Displacement(latest.CameraX, m_X, sampled.SceneWidth, sampled.Wrap & 1),
                Displacement(latest.CameraTargetY + sampled.Height / 2, m_CursorY, sampled.SceneHeight, sampled.Wrap & 2) - Displacement(latest.CameraY, m_Y, sampled.SceneHeight, sampled.Wrap & 2));
            if (cursor && n.Control == Interaction::WorldCursor && (n.Flags & ScreenSpace)) Translate(n,
                sampled.Width / 2 + Displacement(m_X + sampled.Width / 2, m_CursorX, sampled.SceneWidth, sampled.Wrap & 1) - cursor->X,
                sampled.Height / 2 + Displacement(m_Y + sampled.Height / 2, m_CursorY, sampled.SceneHeight, sampled.Wrap & 2) - cursor->Y);
        }
    }
    void Export(Input& input, bool enabled) const {
        input.ViewValid = m_Time && enabled; input.ViewX = m_X; input.ViewY = m_Y;
        input.CursorValid = input.ViewValid && (m_Mode == 1 || m_Mode == 3 || m_Mode == 7 || (m_Mode >= 12 && m_Mode <= 18));
        input.CursorMode = m_Mode; input.CursorX = m_CursorX; input.CursorY = m_CursorY;
    }
private:
    static void Bound(float& value, float span, bool wrap, int viewport) {
        if (wrap) { value = std::fmod(value, span); if (value < 0) value += span; }
        else value = std::clamp(value, 0.0f, std::max(0.0f, span - viewport));
    }
    uint64_t m_Time = 0;
    uint64_t m_SelectTime = 0;
    bool m_WasSelecting = false, m_SelectionEnded = false, m_HostSelectionSeen = false;
    uint8_t m_Mode = 0;
    uint32_t m_MouseX = 0, m_MouseY = 0;
    float m_X = 0, m_Y = 0, m_TargetX = 0, m_TargetY = 0, m_CursorX = 0, m_CursorY = 0;
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
