#pragma once

#include "MultiplayerProtocol.h"
#include "Constants.h"
#include <deque>
#include <memory>
#include <tuple>
#include <functional>
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
// The controlled actor can be predicted (a walking actor under the player's control
// in the normal view), and has a jetpack.
inline constexpr uint8_t MotionPredictable = 1, MotionJetpack = 2;
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
    // The controlled actor's movement, for the guest's own-movement prediction:
    // flags, position and velocity (pixels, pixels per second), scene gravity,
    // the time the host has simulated since applying InputSequence, and the
    // host's simulation speed in percent of real time.
    uint8_t MotionFlags = 0;
    float ActorX = 0, ActorY = 0, ActorVelX = 0, ActorVelY = 0, Gravity = 0;
    uint16_t InputAge = 0;
    uint8_t SimSpeed = 100;
    std::vector<Node> Nodes;
    // Capture-only importance; receivers need only the selected drawing nodes.
    // Shared with the capture pass rather than copied for every guest.
    std::shared_ptr<const std::unordered_set<uint64_t>> CriticalNodes;
    bool Critical(uint64_t id) const { return CriticalNodes && CriticalNodes->contains(id); }
};
template <typename Measure>
inline size_t FitSnapshot(Snapshot& snapshot, size_t packedBudget, Measure measure) {
    size_t packedSize = measure(snapshot);
    if (packedSize <= packedBudget) return packedSize;
    auto essential = [&](const Node& n) { return (n.Flags & (ScreenSpace | Layer)) || snapshot.Critical(n.ID); };
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
        if ((node.Flags & (ScreenSpace | Layer)) || snapshot.Critical(node.ID)) return 1;
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
// Snapshots are delta-encoded against a baseline the guest has acknowledged
// (Quake 3 style), or against default nodes when there is none. Each node
// sends its identity as a difference from the previous node, a mask of
// changed field groups, and only those fields. Positions are quantized to
// 1/16 pixel and angles to 1/4096 radian as varint differences, so a moving
// sprite costs a few bytes instead of 117 and an unchanged one about two.
inline void WriteVarint(Writer& writer, uint64_t value) { while (value >= 0x80) { writer.U8(uint8_t(value) | 0x80); value >>= 7; } writer.U8(uint8_t(value)); }
inline bool ReadVarint(Reader& reader, uint64_t& value) {
    value = 0;
    for (int shift = 0; shift < 64; shift += 7) { uint8_t byte; if (!reader.U8(byte)) return false; value |= uint64_t(byte & 0x7f) << shift; if (!(byte & 0x80)) return shift < 63 || byte <= 1; }
    return false;
}
inline uint64_t ZigZag(int64_t value) { return (uint64_t(value) << 1) ^ uint64_t(value >> 63); }
inline int64_t UnZigZag(uint64_t value) { return int64_t(value >> 1) ^ -int64_t(value & 1); }
inline constexpr float PositionScale = 16, AngleScale = 4096;
inline int64_t Quantize(float value, float scale) { return std::llround(double(value) * scale); }
enum NodeField : uint32_t {
    FieldAsset = 1, FieldParent = 2, FieldKind = 4, FieldColor = 8, FieldTime = 16, FieldStyle = 32, FieldX = 64, FieldY = 128, FieldAngle = 256,
    FieldSize = 512, FieldPivot = 1024, FieldSource = 2048, FieldSpan = 4096, FieldRatio = 8192, FieldClip = 16384, FieldPixels = 32768
};
inline uint32_t ChangedFields(const Node& n, const Node& b) {
    uint32_t mask = 0;
    if (n.Asset != b.Asset) mask |= FieldAsset;
    if (n.Parent != b.Parent) mask |= FieldParent;
    if (n.Type != b.Type || n.Control != b.Control || n.Flags != b.Flags) mask |= FieldKind;
    if (n.Color != b.Color || n.Alpha != b.Alpha) mask |= FieldColor;
    if (n.StartTime != b.StartTime || n.EndTime != b.EndTime) mask |= FieldTime;
    if (n.BlendMode != b.BlendMode || n.TintR != b.TintR || n.TintG != b.TintG || n.TintB != b.TintB) mask |= FieldStyle;
    if (Quantize(n.X, PositionScale) != Quantize(b.X, PositionScale)) mask |= FieldX;
    if (Quantize(n.Y, PositionScale) != Quantize(b.Y, PositionScale)) mask |= FieldY;
    if (Quantize(n.Angle, AngleScale) != Quantize(b.Angle, AngleScale)) mask |= FieldAngle;
    if (n.Width != b.Width || n.Height != b.Height) mask |= FieldSize;
    if (n.PivotX != b.PivotX || n.PivotY != b.PivotY) mask |= FieldPivot;
    if (n.SourceX != b.SourceX || n.SourceY != b.SourceY || n.SourceWidth != b.SourceWidth || n.SourceHeight != b.SourceHeight) mask |= FieldSource;
    if (n.X2 != b.X2 || n.Y2 != b.Y2) mask |= FieldSpan;
    if (n.X3 != b.X3 || n.Y3 != b.Y3) mask |= FieldRatio;
    if (n.ClipX != b.ClipX || n.ClipY != b.ClipY || n.ClipWidth != b.ClipWidth || n.ClipHeight != b.ClipHeight) mask |= FieldClip;
    if (n.Pixels != b.Pixels || n.PixelColors != b.PixelColors) mask |= FieldPixels;
    return mask;
}
inline void WriteNodeDelta(Writer& writer, const Node& n, const Node& b, uint32_t mask) {
    WriteVarint(writer, mask);
    if (mask & FieldAsset) WriteVarint(writer, n.Asset);
    if (mask & FieldParent) WriteVarint(writer, n.Parent);
    if (mask & FieldKind) { writer.U8(uint8_t(n.Type)); writer.U8(uint8_t(n.Control)); writer.U8(n.Flags); }
    if (mask & FieldColor) { writer.U8(n.Color); writer.U8(n.Alpha); }
    if (mask & FieldTime) { WriteVarint(writer, n.StartTime); WriteVarint(writer, n.EndTime); }
    if (mask & FieldStyle) { writer.U8(n.BlendMode); writer.U8(n.TintR); writer.U8(n.TintG); writer.U8(n.TintB); }
    if (mask & FieldX) WriteVarint(writer, ZigZag(Quantize(n.X, PositionScale) - Quantize(b.X, PositionScale)));
    if (mask & FieldY) WriteVarint(writer, ZigZag(Quantize(n.Y, PositionScale) - Quantize(b.Y, PositionScale)));
    if (mask & FieldAngle) WriteVarint(writer, ZigZag(Quantize(n.Angle, AngleScale) - Quantize(b.Angle, AngleScale)));
    if (mask & FieldSize) { writer.F32(n.Width); writer.F32(n.Height); }
    if (mask & FieldPivot) { writer.F32(n.PivotX); writer.F32(n.PivotY); }
    if (mask & FieldSource) { writer.F32(n.SourceX); writer.F32(n.SourceY); writer.F32(n.SourceWidth); writer.F32(n.SourceHeight); }
    if (mask & FieldSpan) { writer.F32(n.X2); writer.F32(n.Y2); }
    if (mask & FieldRatio) { writer.F32(n.X3); writer.F32(n.Y3); }
    if (mask & FieldClip) { writer.U16(n.ClipX); writer.U16(n.ClipY); writer.U16(n.ClipWidth); writer.U16(n.ClipHeight); }
    if (mask & FieldPixels) {
        WriteVarint(writer, n.Pixels.size());
        for (size_t i = 0; i < n.Pixels.size(); ++i) { writer.U16(uint16_t(n.Pixels[i].first)); writer.U16(uint16_t(n.Pixels[i].second)); writer.U8(i < n.PixelColors.size() ? n.PixelColors[i] : n.Color); }
    }
}
inline bool ReadNodeDelta(Reader& reader, Node& n) {
    uint64_t mask, value;
    if (!ReadVarint(reader, mask) || mask >= (uint64_t(FieldPixels) << 1)) return false;
    // Quantized fields continue from the baseline's own representable value.
    const int64_t baseX = Quantize(n.X, PositionScale), baseY = Quantize(n.Y, PositionScale), baseAngle = Quantize(n.Angle, AngleScale);
    if ((mask & FieldAsset) && !ReadVarint(reader, n.Asset)) return false;
    if ((mask & FieldParent) && !ReadVarint(reader, n.Parent)) return false;
    if (mask & FieldKind) { uint8_t type, control; if (!reader.U8(type) || !reader.U8(control) || !reader.U8(n.Flags)) return false; n.Type = Shape(type); n.Control = Interaction(control); }
    if ((mask & FieldColor) && (!reader.U8(n.Color) || !reader.U8(n.Alpha))) return false;
    if ((mask & FieldTime) && (!ReadVarint(reader, n.StartTime) || !ReadVarint(reader, n.EndTime))) return false;
    if ((mask & FieldStyle) && (!reader.U8(n.BlendMode) || !reader.U8(n.TintR) || !reader.U8(n.TintG) || !reader.U8(n.TintB))) return false;
    auto quantized = [&](uint32_t field, int64_t base, float scale, float& target) {
        if (!(mask & field)) return true;
        if (!ReadVarint(reader, value)) return false;
        const int64_t delta = UnZigZag(value); if (std::abs(delta) > int64_t(1) << 40) return false;
        target = float(double(base + delta) / scale); return true;
    };
    if (!quantized(FieldX, baseX, PositionScale, n.X) || !quantized(FieldY, baseY, PositionScale, n.Y) || !quantized(FieldAngle, baseAngle, AngleScale, n.Angle)) return false;
    if ((mask & FieldSize) && (!reader.F32(n.Width) || !reader.F32(n.Height))) return false;
    if ((mask & FieldPivot) && (!reader.F32(n.PivotX) || !reader.F32(n.PivotY))) return false;
    if ((mask & FieldSource) && (!reader.F32(n.SourceX) || !reader.F32(n.SourceY) || !reader.F32(n.SourceWidth) || !reader.F32(n.SourceHeight))) return false;
    if ((mask & FieldSpan) && (!reader.F32(n.X2) || !reader.F32(n.Y2))) return false;
    if ((mask & FieldRatio) && (!reader.F32(n.X3) || !reader.F32(n.Y3))) return false;
    if ((mask & FieldClip) && (!reader.U16(n.ClipX) || !reader.U16(n.ClipY) || !reader.U16(n.ClipWidth) || !reader.U16(n.ClipHeight))) return false;
    if (mask & FieldPixels) {
        if (!ReadVarint(reader, value) || value > 16384 || value > reader.Remaining() / 5) return false;
        n.Pixels.clear(); n.PixelColors.clear(); n.Pixels.reserve(size_t(value)); n.PixelColors.reserve(size_t(value));
        for (uint64_t i = 0; i < value; ++i) { uint16_t x, y; uint8_t color; if (!reader.U16(x) || !reader.U16(y) || !reader.U8(color)) return false; n.Pixels.emplace_back(std::bit_cast<int16_t>(x), std::bit_cast<int16_t>(y)); n.PixelColors.push_back(color); }
    }
    return Valid(n);
}
using NodeIndex = std::unordered_map<uint64_t, const Node*>;
inline NodeIndex IndexNodes(const Snapshot& s) { NodeIndex index; index.reserve(s.Nodes.size()); for (const auto& n : s.Nodes) index.emplace(n.ID, &n); return index; }
// A caller encoding the same baseline repeatedly can pass its index once.
inline void WriteSnapshot(Writer& writer, const Snapshot& s, const Snapshot* baseline = nullptr, const NodeIndex* index = nullptr) {
    writer.U32(s.ID); writer.U32(baseline ? baseline->ID : 0); writer.U32(s.InputSequence); writer.U64(s.Time); writer.U16(s.Width); writer.U16(s.Height); writer.U16(s.SceneWidth); writer.U16(s.SceneHeight); writer.U8(s.Wrap); writer.U8(s.Phase);
    writer.F32(s.CameraX); writer.F32(s.CameraY); writer.U64(s.ControlledActor); writer.U32(s.MouseX); writer.U32(s.MouseY);
    writer.F32(s.AimX); writer.F32(s.AimY); writer.F32(s.LookX); writer.F32(s.LookY);
    writer.U8(s.Paused); writer.U8(s.ViewMode); writer.F32(s.CameraTargetX); writer.F32(s.CameraTargetY); writer.F32(s.MouseScale); writer.F32(s.ScrollSpeed);
    writer.U8(s.MotionFlags); writer.F32(s.ActorX); writer.F32(s.ActorY); writer.F32(s.ActorVelX); writer.F32(s.ActorVelY); writer.F32(s.Gravity); writer.U16(s.InputAge); writer.U8(s.SimSpeed);
    writer.U32(uint32_t(s.Nodes.size()));
    NodeIndex local; if (baseline && !index) { local = IndexNodes(*baseline); index = &local; }
    const NodeIndex empty; const NodeIndex& base = baseline ? *index : empty;
    const Node none; uint64_t previous = 0;
    for (const auto& n : s.Nodes) {
        WriteVarint(writer, ZigZag(int64_t(n.ID - previous))); previous = n.ID;
        const auto found = base.find(n.ID); const Node& b = found != base.end() ? *found->second : none;
        WriteNodeDelta(writer, n, b, ChangedFields(n, b));
    }
}
template <typename Lookup>
inline bool ReadSnapshot(Reader& reader, Snapshot& s, Lookup&& lookup) {
    uint32_t count, baselineID;
    if (!reader.U32(s.ID) || !reader.U32(baselineID) || !reader.U32(s.InputSequence) || !reader.U64(s.Time) || !reader.U16(s.Width) || !reader.U16(s.Height) || !reader.U16(s.SceneWidth) || !reader.U16(s.SceneHeight) || !reader.U8(s.Wrap) || !reader.U8(s.Phase) || !reader.F32(s.CameraX) || !reader.F32(s.CameraY) || !reader.U64(s.ControlledActor) || !reader.U32(s.MouseX) || !reader.U32(s.MouseY) || !reader.F32(s.AimX) || !reader.F32(s.AimY) || !reader.F32(s.LookX) || !reader.F32(s.LookY)) return false;
    uint8_t paused;
    if (!reader.U8(paused) || paused > 1 || !reader.U8(s.ViewMode) || !reader.F32(s.CameraTargetX) || !reader.F32(s.CameraTargetY) || !reader.F32(s.MouseScale) || !reader.F32(s.ScrollSpeed)) return false;
    if (!reader.U8(s.MotionFlags) || s.MotionFlags > 3 || !reader.F32(s.ActorX) || !reader.F32(s.ActorY) || !reader.F32(s.ActorVelX) || !reader.F32(s.ActorVelY) || !reader.F32(s.Gravity) || !reader.U16(s.InputAge) || !reader.U8(s.SimSpeed) || s.SimSpeed > 200 || !reader.U32(count)) return false;
    if (!Coordinate(s.ActorX) || !Coordinate(s.ActorY) || std::abs(s.ActorVelX) > 100000 || std::abs(s.ActorVelY) > 100000 || std::abs(s.Gravity) > 100000) return false;
    s.Paused = paused;
    if (s.ViewMode > 20 || !Coordinate(s.CameraTargetX) || !Coordinate(s.CameraTargetY) || s.MouseScale < 0 || s.MouseScale > 2 || s.ScrollSpeed < 0 || s.ScrollSpeed > 1) return false;
    if (!Coordinate(s.AimX) || !Coordinate(s.AimY) || std::abs(s.AimX) > 1 || std::abs(s.AimY) > 1 || !Coordinate(s.LookX) || !Coordinate(s.LookY)) return false;
    if (s.Phase > 6) return false;
    if (!s.ID || !s.Time || s.Time > MaxTime || s.Width < 320 || s.Width > 3840 || s.Height < 180 || s.Height > 2160 || !s.SceneWidth || !s.SceneHeight || s.SceneWidth > 16384 || s.SceneHeight > 16384 || s.Wrap > 3 || !Coordinate(s.CameraX) || !Coordinate(s.CameraY) || count > MaxNodes || count > reader.Remaining() / 2) return false;
    // A delta needs the exact baseline it was encoded against; without it the
    // state is undecodable and the host falls back once acknowledgements stop.
    const Snapshot* baseline = nullptr;
    if (baselineID) { baseline = lookup(baselineID); if (!baseline || baseline->ID != baselineID) return false; }
    std::unordered_map<uint64_t, const Node*> base;
    if (baseline) { base.reserve(baseline->Nodes.size()); for (const auto& n : baseline->Nodes) base.emplace(n.ID, &n); }
    s.Nodes.resize(count); std::unordered_set<uint64_t> ids; ids.reserve(count); uint64_t previous = 0, value;
    for (auto& n : s.Nodes) {
        if (!ReadVarint(reader, value)) return false;
        const uint64_t id = previous + uint64_t(UnZigZag(value)); previous = id;
        const auto found = base.find(id); n = found != base.end() ? *found->second : Node(); n.ID = id;
        if (!ReadNodeDelta(reader, n) || n.Parent == n.ID || !ids.insert(n.ID).second) return false;
    }
    return reader.Done();
}
inline bool ReadSnapshot(Reader& reader, Snapshot& s) { return ReadSnapshot(reader, s, [](uint32_t) -> const Snapshot* { return nullptr; }); }
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
// Reorders a manifest so tiles nearest the guest's first view stream first.
// Map nodes are screen-relative for their own parallax layer; wrapped layers
// measure the shorter way around their span.
inline std::vector<uint64_t> NearestResources(std::span<const uint64_t> assets, const SceneMap& map, int width, int height) {
    std::unordered_map<uint64_t, float> distance;
    auto axis = [](float position, float extent, float span, int viewport) {
        float d = std::abs(position + extent / 2 - viewport / 2.0f);
        if (span > 0) { d = std::fmod(d, span); d = std::min(d, span - d); }
        return std::max(0.0f, d - extent / 2 - viewport / 2.0f);
    };
    for (const auto& node : map.Nodes) if (node.Asset) {
        const float d = std::max(axis(node.X, node.Width, node.X2, width), axis(node.Y, node.Height, node.Y2, height));
        if (auto found = distance.find(node.Asset); found == distance.end() || d < found->second) distance[node.Asset] = d;
    }
    std::vector<uint64_t> ordered(assets.begin(), assets.end());
    std::stable_sort(ordered.begin(), ordered.end(), [&](uint64_t a, uint64_t b) {
        const auto da = distance.find(a), db = distance.find(b);
        return (da == distance.end() ? std::numeric_limits<float>::max() : da->second) < (db == distance.end() ? std::numeric_limits<float>::max() : db->second);
    });
    return ordered;
}
inline unsigned LayerOrdinal(const Node& node) {
    if (!(node.Flags & Layer)) return 0;
    return node.ID & (uint64_t(1) << 62) ? unsigned((node.ID >> 48) & 0x3fff) :
        node.ID & (uint64_t(1) << 61) ? unsigned((node.ID >> 32) & 0xffff) : 0;
}
inline constexpr unsigned ForegroundLayer = 101, FogLayer = 102;
inline constexpr uint8_t PhaseEditing = 2;
// The baseline supplies the entire passive map. Snapshots carry only tiles
// that changed since the guest's acknowledged state, including team fog;
// travelling farther never discards already known tiles.
class RetainedLayers {
public:
    void Reset() { m_Nodes.clear(); m_Foreground.clear(); }
    void Install(const SceneMap& map) { Reset(); for (const auto& n : map.Nodes) Store(n, map.CameraX, map.CameraY); }
    void Update(const Snapshot& snapshot) { for (const auto& n : snapshot.Nodes) Store(n, snapshot.CameraX, snapshot.CameraY); }
    std::unordered_set<uint64_t> Resources() const { std::unordered_set<uint64_t> assets; for (const auto& [id, n] : m_Nodes) if (n.Asset) assets.insert(n.Asset); return assets; }
    // Assets of the retained tiles this view draws, so the first state can wait for its own surroundings.
    // A tile the state itself carries replaces the retained version, which a
    // battle may have changed (and the host discarded) since the manifest.
    // Whether the guest's copy of the foreground terrain is solid at a world point:
    // 1 solid, 0 open, -1 unknown (outside the map or pixels not yet held).
    template <typename Find>
    int Solid(float x, float y, float sceneWidth, bool wrapX, Find&& find) const {
        if (wrapX && sceneWidth > 0) { x = std::fmod(x, sceneWidth); if (x < 0) x += sceneWidth; }
        const auto tile = m_Foreground.find(TileKey(int(std::floor(x / TileSize)), int(std::floor(y / TileSize))));
        if (tile == m_Foreground.end()) return -1;
        const Node& n = m_Nodes.at(tile->second); if (!n.Asset) return 0;
        const Resource* resource = find(n.Asset); if (!resource || resource->Depth != 8) return -1;
        const int px = int(std::floor(x - n.X)), py = int(std::floor(y - n.Y));
        if (px < 0 || py < 0 || px >= resource->Width || py >= resource->Height) return -1;
        return resource->Pixels[size_t(py) * resource->Width + px] != ColorKeys::g_MaskColor ? 1 : 0;
    }
    void VisibleAssets(const Snapshot& snapshot, std::unordered_set<uint64_t>& assets) const {
        std::unordered_set<uint64_t> replaced; for (const auto& n : snapshot.Nodes) if (LayerOrdinal(n)) replaced.insert(n.ID);
        for (const auto& [id, source] : m_Nodes) if (source.Asset && !replaced.contains(id) && (LayerOrdinal(source) != FogLayer || snapshot.Phase != PhaseEditing)) { Node n = Placed(source, snapshot); if (Visible(n, snapshot)) assets.insert(n.Asset); }
    }
    void Compose(Snapshot& snapshot) const {
        if (m_Nodes.empty()) return;
        std::vector<Node> nodes; nodes.reserve(m_Nodes.size() + snapshot.Nodes.size());
        // Backdrops and background terrain draw first, then objects, then the
        // foreground terrain and the team's fog before any screen overlay.
        auto group = [](unsigned ordinal) { return ordinal == FogLayer ? 2 : ordinal == ForegroundLayer ? 1 : 0; };
        auto append = [&](int which) {
            if (which == 2 && snapshot.Phase == PhaseEditing) return;
            for (const auto& [id, source] : m_Nodes) if (group(LayerOrdinal(source)) == which) {
                auto n = Placed(source, snapshot);
                if (Visible(n, snapshot)) nodes.push_back(std::move(n));
            }
        };
        append(0); bool foreground = false;
        for (auto& n : snapshot.Nodes) {
            if (LayerOrdinal(n)) continue;
            if (!foreground && ((n.Flags & ScreenSpace) || (n.Flags & Additive))) { append(1); append(2); foreground = true; }
            nodes.push_back(std::move(n));
        }
        if (!foreground) { append(1); append(2); }
        snapshot.Nodes = std::move(nodes);
    }
private:
    static Node Placed(const Node& source, const Snapshot& snapshot) { Node n = source; n.X -= snapshot.CameraX * n.X3; n.Y -= snapshot.CameraY * n.Y3; return n; }
    static bool Visible(const Node& n, const Snapshot& snapshot) {
        if (n.Type != Shape::Sprite) return true;
        const auto near = [](float value, float extent, float span, int viewport) {
            if (span > 0) { float d = std::fmod(value + extent / 2 - viewport / 2, span); if (d > span / 2) d -= span; if (d < -span / 2) d += span; value = viewport / 2 + d - extent / 2; }
            return value <= viewport && value + extent >= 0;
        };
        return near(n.X, n.Width, n.X2, snapshot.Width) && near(n.Y, n.Height, n.Y2, snapshot.Height);
    }
    void Store(Node n, float cameraX, float cameraY) {
        const unsigned ordinal = LayerOrdinal(n); if (!ordinal || ordinal > FogLayer) return;
        n.X += cameraX * n.X3; n.Y += cameraY * n.Y3;
        if (ordinal == ForegroundLayer && n.Type == Shape::Sprite) m_Foreground[TileKey(int(std::floor((n.X + 1) / TileSize)), int(std::floor((n.Y + 1) / TileSize)))] = n.ID;
        m_Nodes[n.ID] = std::move(n);
    }
    static uint64_t TileKey(int x, int y) { return (uint64_t(uint32_t(x)) << 32) | uint32_t(y); }
    std::unordered_map<uint64_t, uint64_t> m_Foreground;
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
    void Sent(uint32_t message, uint16_t index, size_t bytes, uint64_t now = 0) {
        if (CanSend(bytes) && m_Packets.try_emplace(Key(message, index), Packet{bytes, now}).second) m_Bytes += bytes;
    }
    // A receipt also reports when its fragment left, for round-trip samples.
    bool Receipt(uint32_t message, uint16_t index, uint64_t* sentAt = nullptr) {
        auto packet = m_Packets.find(Key(message, index)); if (packet == m_Packets.end()) return false;
        if (sentAt) *sentAt = packet->second.Time;
        m_Bytes -= packet->second.Bytes; m_Packets.erase(packet); return true;
    }
    bool Contains(uint32_t message) const { return std::any_of(m_Packets.begin(), m_Packets.end(), [message](const auto& p) { return p.first >> 16 == message; }); }
    size_t Bytes() const { return m_Bytes; }
    void Reset() { m_Packets.clear(); m_Bytes = 0; }
private:
    struct Packet { size_t Bytes = 0; uint64_t Time = 0; };
    static uint64_t Key(uint32_t message, uint16_t index) { return (uint64_t(message) << 16) | index; }
    std::unordered_map<uint64_t, Packet> m_Packets;
    size_t m_Bytes = 0;
};
// Per-guest world send rate. Random WAN loss is not congestion, so loss alone
// barely matters; queueing delay is. Like LEDBAT (RFC 6817) the uncongested
// baseline is the minimum round trip over a few seconds, and each 500 ms window
// compares its own fastest sample with it: jitter raises the median but rarely
// the window minimum, while a standing queue raises every sample. The target
// leaves room for one paced burst, client frame timing and WAN jitter; a link
// that is really overfilled shows hundreds of milliseconds and loss together.
class SendRate {
public:
    static constexpr double Floor = 32000, StartRate = 125000;
    static constexpr uint64_t WindowMS = 500, BaselineMS = 5000, TargetQueueMS = 90, HeavyQueueMS = 180;
    void Reset(double cap, uint64_t now) { *this = SendRate(); m_Cap = std::max(Floor, cap); m_Rate = std::min(m_Cap, StartRate); m_WindowStart = now; m_BaselineStart = now; }
    void SetCap(double cap) { m_Cap = std::max(Floor, cap); m_Rate = std::min(m_Rate, m_Cap); }
    double Rate() const { return m_Rate; }
    double Cap() const { return m_Cap; }
    uint64_t BaseRtt() const { return std::min(m_BaseCurrent, m_BasePrevious); }
    uint64_t QueueDelay() const { return m_QueueDelay; }
    void Sample(uint64_t rtt) { m_WindowMin = std::min(m_WindowMin, rtt); m_BaseCurrent = std::min(m_BaseCurrent, rtt); ++m_Samples; }
    // Called when queued world data had to wait for rate credit.
    void Limited() { m_Limited = true; }
    void Update(uint64_t now, double lossFraction = 0) {
        if (!m_Cap) return;
        if (now - m_BaselineStart >= BaselineMS) { m_BasePrevious = m_BaseCurrent; m_BaseCurrent = UINT64_MAX; m_BaselineStart = now; }
        if (now - m_WindowStart < WindowMS) return;
        if (m_Samples) {
            const uint64_t base = BaseRtt();
            m_QueueDelay = m_WindowMin > base ? m_WindowMin - base : 0;
            // One noisy window is not a queue: back off when the delay persists.
            const bool queued = m_QueueDelay > TargetQueueMS, heavy = m_QueueDelay > HeavyQueueMS;
            if ((heavy && m_Queued) || lossFraction > .25) { m_Rate = std::max(Floor, m_Rate * .7); m_SlowStart = false; }
            else if ((queued && m_Queued) || lossFraction > .15) { m_Rate = std::max(Floor, m_Rate * .9); m_SlowStart = false; }
            else if (m_Limited && !queued) m_Rate = std::min(m_Cap, m_SlowStart ? m_Rate * 1.5 : m_Rate + std::max(8000.0, m_Cap * .05));
            m_Queued = queued;
        }
        m_WindowStart = now; m_WindowMin = UINT64_MAX; m_Samples = 0; m_Limited = false;
    }
private:
    double m_Cap = 0, m_Rate = 0;
    uint64_t m_WindowStart = 0, m_BaselineStart = 0, m_WindowMin = UINT64_MAX, m_BaseCurrent = UINT64_MAX, m_BasePrevious = UINT64_MAX, m_QueueDelay = 0;
    unsigned m_Samples = 0;
    bool m_Limited = false, m_SlowStart = true, m_Queued = false;
};
// Limit reliable repair traffic independently of the number of missing tiles.
// Oldest requests go first so a large map cannot starve its last missing asset.
// An asset is requested only after it has been missing for the grace period:
// the host is usually already sending it, and a request resends it first.
// Assets in `first` (what holds the guest's first view) precede the rest.
class ResourceRequests {
public:
    std::vector<uint64_t> Select(const std::unordered_set<uint64_t>& missing, uint64_t now, uint64_t grace = 0, const std::unordered_set<uint64_t>* first = nullptr) {
        std::erase_if(m_Last, [&](const auto& request) { return !missing.contains(request.first); });
        std::erase_if(m_Seen, [&](const auto& seen) { return !missing.contains(seen.first); });
        std::vector<std::tuple<bool, uint64_t, uint64_t>> eligible;
        for (auto id : missing) {
            if (now - m_Seen.try_emplace(id, now).first->second < grace) continue;
            const auto found = m_Last.find(id);
            if (found == m_Last.end() || now - found->second >= 5000) eligible.emplace_back(!first || !first->contains(id), found == m_Last.end() ? 0 : found->second, id);
        }
        std::sort(eligible.begin(), eligible.end());
        std::vector<uint64_t> selected;
        for (size_t i = 0; i < std::min<size_t>(64, eligible.size()); ++i) { selected.push_back(std::get<2>(eligible[i])); m_Last[std::get<2>(eligible[i])] = now; }
        return selected;
    }
    void Reset() { m_Last.clear(); m_Seen.clear(); }
private:
    std::unordered_map<uint64_t, uint64_t> m_Last, m_Seen;
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
// Snapshots keep only a few partial states. Reliable resources arrive
// unordered, so many tiles' fragments interleave under loss; evicting a
// partial tile would discard fragments the transport will never resend.
class Assembler {
public:
    explicit Assembler(uint64_t expiryMS = 5000, size_t maxPending = 4, size_t maxBytes = MaxPackedPayload * 4) : m_ExpiryMS(expiryMS), m_MaxPending(maxPending), m_MaxBytes(maxBytes) {}
    std::optional<std::vector<uint8_t>> Push(const Chunk& c, uint64_t now) {
        if (!ValidChunk(c)) return {};
        std::erase_if(m_Pending, [this, now](const Pending& p) { return now - p.Start > m_ExpiryMS; });
        auto found = std::find_if(m_Pending.begin(), m_Pending.end(), [&](const Pending& p) { return p.ID == c.ID; });
        if (found == m_Pending.end()) {
            size_t bytes = c.Size; for (const auto& p : m_Pending) bytes += p.Bytes.size();
            while (!m_Pending.empty() && (m_Pending.size() >= m_MaxPending || bytes > m_MaxBytes)) { bytes -= m_Pending.front().Bytes.size(); m_Pending.pop_front(); }
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
    size_t m_MaxPending, m_MaxBytes;
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
        const Node* cursor = nullptr; for (const auto& n : latest.Nodes) if (n.Control == Interaction::WorldCursor) { cursor = &n; break; }
        const bool landing = mode == LandingZoneMode;
        if (landing && cursor && latest.ID != m_Reconciled) {
            // The host keeps the zone inside its landing areas. Apply its correction
            // to the position this state answered, once, keeping the motion since.
            m_Reconciled = latest.ID;
            for (const auto& [sequence, sentX] : m_SentCursor) if (sequence && sequence == latest.InputSequence) {
                const float correction = Displacement(sentX, latest.CameraX + cursor->X, latest.SceneWidth, latest.Wrap & 1);
                if (std::abs(correction) > 1) { m_CursorX += correction; for (auto& sent : m_SentCursor) if (Newer(sent.first, sequence)) sent.second += correction; }
                break;
            }
        }
        if (enabled && mouseScale > 0 && mode != 0) {
            float dx = dxInput * mouseScale, dy = landing ? 0 : dyInput * mouseScale;
            if (!dx && !dy) {
                const auto held = [&](unsigned bit) { return bool(input.Held & (uint64_t(1) << bit)); };
                const float vx = input.Device == DEVICE_MOUSE_KEYB ? float(held(INPUT_L_RIGHT)) - float(held(INPUT_L_LEFT)) : input.MoveX;
                const float vy = input.Device == DEVICE_MOUSE_KEYB ? float(held(INPUT_L_DOWN)) - float(held(INPUT_L_UP)) : input.MoveY;
                dx = vx * elapsed * .6f * mouseScale; dy = landing ? 0 : vy * elapsed * .6f * mouseScale;
            }
            m_CursorX += dx; m_CursorY += dy; m_TargetX += dx; m_TargetY += dy;
            Bound(m_CursorX, sampled.SceneWidth, sampled.Wrap & 1, 0); Bound(m_CursorY, sampled.SceneHeight, sampled.Wrap & 2, 0);
            // Preserve the native cursor-to-camera offset at world edges.
            m_TargetX = m_CursorX - sampled.Width / 2; m_TargetY = m_CursorY - sampled.Height / 2;
            // A landing zone moves sideways; its height follows the host's terrain.
            if (landing) { if (cursor) m_CursorY = latest.CameraY + cursor->Y; m_TargetY = latest.CameraTargetY; }
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
        input.CursorValid = input.ViewValid && (m_Mode == 1 || m_Mode == 3 || m_Mode == 7 || m_Mode == LandingZoneMode || (m_Mode >= 12 && m_Mode <= 18));
        input.CursorMode = m_Mode; input.CursorX = m_CursorX; input.CursorY = m_CursorY;
    }
    // Remembers the cursor carried by the input with this sequence, so a host
    // correction can be matched to the position it answered.
    void RecordSent(uint32_t sequence) { auto& slot = m_SentCursor[sequence % m_SentCursor.size()]; slot = {sequence, m_CursorX}; }
    static constexpr uint8_t LandingZoneMode = 8;
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
    std::array<std::pair<uint32_t, float>, 64> m_SentCursor{};
    uint32_t m_Reconciled = 0;
};
// The guest's prediction of its own actor's movement. The actor moves at once
// from the player's input, using a small model of walking, gravity and the
// jetpack against the terrain the guest holds, instead of a round trip later.
// The model's speeds come from the host's own states (per gait), so it needs
// no copy of the actor. Each host state restarts the model from the host's
// position and velocity at the last input the host applied, and replays the
// inputs made since. What still differs fades out; if the model keeps
// disagreeing with the host (doors, other actors, unusual bodies), it steps
// aside and the host's movement is shown instead.
class LocalMotion {
public:
    struct Control { float Move = 0; bool Jet = false, Fast = false, Crouch = false; };
    using Solid = std::function<int(float x, float y)>; // 1 solid, 0 open, -1 unknown
    static constexpr float GroundAcceleration = 1600, StepUp = 8, StepDown = 12, HalfWidth = 7, HeadHeight = 24, TrustedError = 12;
    void Reset() { *this = LocalMotion(); }
    // Advances one rendered frame. Returns whether (x, y) is the predicted
    // position of the actor (its host position, as Snapshot::ActorX/Y).
    bool Update(const Snapshot& latest, uint64_t now, uint32_t sequence, const Control& control, const Solid& solid, float& x, float& y) {
        if (!(latest.MotionFlags & MotionPredictable) || !latest.ControlledActor) { if (m_Actor) Reset(); return false; }
        m_Width = latest.Wrap & 1 ? float(latest.SceneWidth) : 0;
        if (latest.ControlledActor != m_Actor) { Reset(); m_Actor = latest.ControlledActor; m_Width = latest.Wrap & 1 ? float(latest.SceneWidth) : 0; m_StateID = latest.ID; Learn(latest, Control{}, solid); m_State = Host(latest, solid); }
        const float dt = m_LastTime && now > m_LastTime ? float(std::min<uint64_t>(now - m_LastTime, 100)) * .001f * std::max(.05f, latest.SimSpeed * .01f) : 0;
        m_LastTime = now;
        Step(m_State, control, dt, solid);
        m_Frames.push_back({now, sequence, dt, control});
        while (m_Frames.size() > 1 && now - m_Frames.front().Time > 2000) m_Frames.pop_front();
        if (latest.ID != m_StateID) Reconcile(latest, solid);
        const float fade = std::exp(-dt / .1f); m_OffsetX *= fade; m_OffsetY *= fade;
        x = Wrapped(m_State.X + m_OffsetX); y = m_State.Y + m_OffsetY;
        return m_FootKnown && m_Error < TrustedError;
    }
    // The last correction a host state made, in pixels.
    float LastCorrection() const { return m_LastCorrection; }
    bool Active() const { return m_Actor && m_FootKnown && m_Error < TrustedError; }
private:
    struct State { float X = 0, Y = 0, VX = 0, VY = 0; bool Ground = false; };
    struct Frame { uint64_t Time; uint32_t Sequence; float Dt; Control Input; };
    static int Gait(const Control& c) { return c.Crouch ? 2 : c.Fast ? 1 : 0; }
    static float Approach(float value, float target, float step) { return value < target ? std::min(target, value + step) : std::max(target, value - step); }
    static void Blend(float& value, float sample) { value = value > 0 ? value * .7f + sample * .3f : sample; }
    float Wrapped(float x) const { if (m_Width > 0) { x = std::fmod(x, m_Width); if (x < 0) x += m_Width; } return x; }
    // The first point at or below fromY, within range, that is not open terrain.
    static bool Surface(float x, float fromY, float range, const Solid& solid, float& surface) {
        for (float y = std::floor(fromY); y <= fromY + range; ++y) if (solid(x, y) != 0) { surface = y; return true; }
        return false;
    }
    bool Wall(float x, float y, float vx, const Solid& solid) const {
        if (vx == 0) return false;
        const float edge = x + (vx > 0 ? HalfWidth : -HalfWidth);
        return solid(edge, y + m_Foot - StepUp - 2) == 1 || solid(edge, y) == 1;
    }
    State Host(const Snapshot& latest, const Solid& solid) const {
        State s{latest.ActorX, latest.ActorY, latest.ActorVelX, latest.ActorVelY, false};
        float ground; s.Ground = m_FootKnown && std::abs(s.VY) < 40 && Surface(s.X, s.Y + m_Foot - 3, 7, solid, ground);
        return s;
    }
    void Step(State& s, const Control& c, float dt, const Solid& solid) const {
        if (dt <= 0 || !m_FootKnown) return;
        if (s.Ground && !c.Jet) {
            // Walking at the speed the host showed for this gait, or stopping.
            const float walk = m_Walk[Gait(c)];
            const float target = c.Move != 0 ? (walk > 0 ? c.Move * walk : s.VX) : 0;
            s.VX = Approach(s.VX, target, GroundAcceleration * dt); s.VY = 0;
            const float x = s.X + s.VX * dt;
            if (Wall(x, s.Y, s.VX, solid)) { s.VX = 0; return; }
            float ground;
            if (Surface(x, s.Y + m_Foot - StepUp, StepUp + StepDown, solid, ground)) s.Y = ground - m_Foot; else s.Ground = false;
            s.X = x;
            return;
        }
        s.VY += m_Gravity * dt;
        if (c.Jet && m_Jet > 0) s.VY -= m_Jet * dt;
        if (c.Move != 0 && m_Air > 0) s.VX += c.Move * m_Air * dt;
        const float x = s.X + s.VX * dt;
        if (Wall(x, s.Y, s.VX, solid)) s.VX = 0; else s.X = x;
        const float y = s.Y + s.VY * dt;
        float ground;
        if (s.VY > 0 && Surface(s.X, s.Y + m_Foot, y - s.Y + 1, solid, ground)) { s.Y = ground - m_Foot; s.VY = 0; s.Ground = true; return; }
        if (s.VY < 0 && solid(s.X, y - HeadHeight) == 1) { s.VY = 0; return; }
        s.Y = y;
    }
    void Learn(const Snapshot& latest, const Control& c, const Solid& solid) {
        const float vx = latest.ActorVelX, vy = latest.ActorVelY;
        m_Gravity = latest.Gravity;
        // At rest vertically, the terrain just below is where the actor stands.
        if (std::abs(vy) < 6 && (!m_HostTime || std::abs(m_HostVY) < 6)) {
            // A hovering or jumping actor can also be still; only a plausible, consistent distance counts.
            float ground; if (Surface(latest.ActorX, latest.ActorY + 4, 64, solid, ground) && solid(latest.ActorX, ground) == 1) {
                const float foot = ground - latest.ActorY;
                if (foot >= 8 && foot <= 60 && (!m_FootKnown || std::abs(foot - m_Foot) < 12)) { m_Foot = m_FootKnown ? m_Foot * .5f + foot * .5f : foot; m_FootKnown = true; }
            }
        }
        const State host = Host(latest, solid);
        const float elapsed = m_HostTime && latest.Time > m_HostTime ? float(latest.Time - m_HostTime) * .001f : 0;
        if (host.Ground && !c.Jet && std::abs(c.Move) > .5f && vx * c.Move > 10) Blend(m_Walk[Gait(c)], std::abs(vx));
        if (!host.Ground && m_HostAir && elapsed > .02f && elapsed < .25f) {
            const float ay = (vy - m_HostVY) / elapsed, ax = (vx - m_HostVX) / elapsed;
            if (c.Jet && m_HostJet && m_Gravity - ay > 0) Blend(m_Jet, m_Gravity - ay);
            if (std::abs(c.Move) > .5f && ax * c.Move > 0) Blend(m_Air, std::abs(ax));
        }
        m_HostVX = vx; m_HostVY = vy; m_HostTime = latest.Time; m_HostAir = !host.Ground; m_HostJet = c.Jet;
    }
    void Reconcile(const Snapshot& latest, const Solid& solid) {
        m_StateID = latest.ID;
        // The frame that first sent the input the host reports, and the host's time with it since.
        const auto sent = std::find_if(m_Frames.begin(), m_Frames.end(), [&](const Frame& f) { return !Newer(latest.InputSequence, f.Sequence); });
        Learn(latest, sent != m_Frames.end() ? sent->Input : Control{}, solid);
        const State before = m_State;
        State state = Host(latest, solid);
        if (sent != m_Frames.end()) { const uint64_t match = sent->Time + latest.InputAge; for (const auto& frame : m_Frames) if (frame.Time > match) Step(state, frame.Input, frame.Dt, solid); }
        m_State = state;
        const float dx = Displacement(m_State.X, before.X, m_Width, m_Width > 0), dy = before.Y - m_State.Y;
        m_LastCorrection = std::hypot(dx, dy);
        // A teleport or respawn is shown as it is.
        if (m_LastCorrection > 96) m_OffsetX = m_OffsetY = 0; else { m_OffsetX += dx; m_OffsetY += dy; }
        if (m_FootKnown) m_Error = m_Error * .8f + std::min(m_LastCorrection, 96.0f) * .2f;
    }
    std::deque<Frame> m_Frames;
    State m_State;
    uint64_t m_Actor = 0, m_LastTime = 0, m_HostTime = 0;
    uint32_t m_StateID = 0;
    float m_Width = 0, m_Gravity = 0, m_Foot = 0, m_Jet = 0, m_Air = 0;
    std::array<float, 3> m_Walk{};
    bool m_FootKnown = false, m_HostAir = false, m_HostJet = false;
    float m_HostVX = 0, m_HostVY = 0;
    float m_OffsetX = 0, m_OffsetY = 0, m_Error = 0, m_LastCorrection = 0;
};
inline LocalMotion::Control MotionControl(const Input& input) {
    const auto held = [&](unsigned bit) { return bool(input.Held & (uint64_t(1) << bit)); };
    LocalMotion::Control c;
    // Right wins over left, as in the native controller.
    c.Move = held(INPUT_L_RIGHT) ? 1.0f : held(INPUT_L_LEFT) ? -1.0f : 0.0f;
    c.Jet = held(INPUT_JUMP); c.Fast = held(INPUT_MOVE_FAST); c.Crouch = held(INPUT_CROUCH) || held(INPUT_PRONE);
    return c;
}
// Moves a drawn actor and everything attached to it so its host position is at (x, y).
inline void PlaceActor(Snapshot& scene, const Snapshot& latest, float x, float y) {
    const Node* hostRoot = nullptr; for (const auto& n : latest.Nodes) if (n.ID == latest.ControlledActor) { hostRoot = &n; break; }
    const Node* root = nullptr; for (const auto& n : scene.Nodes) if (n.ID == latest.ControlledActor) { root = &n; break; }
    if (!hostRoot || !root) return;
    // The drawn root keeps its offset from the host position.
    const float dx = Displacement(root->X, x + (hostRoot->X - latest.ActorX), latest.SceneWidth, latest.Wrap & 1), dy = y + (hostRoot->Y - latest.ActorY) - root->Y;
    std::unordered_map<uint64_t, uint64_t> parents; for (const auto& n : scene.Nodes) if (!(n.Flags & ScreenSpace)) parents[n.ID] = n.Parent;
    for (auto& n : scene.Nodes) {
        if (n.Flags & ScreenSpace) continue;
        uint64_t id = n.ID; for (unsigned depth = 0; id && id != latest.ControlledActor && depth < 32; ++depth) { const auto up = parents.find(id); id = up == parents.end() ? 0 : up->second; }
        if (id == latest.ControlledActor) Translate(n, dx, dy);
    }
}
// The presentation delay follows measured delivery instead of a fixed 75 ms:
// it must cover one update interval plus the arrival jitter, or presentation
// runs past the newest state and extrapolates or freezes. Source's default
// interpolation is likewise two 20 Hz intervals (100 ms).
inline constexpr uint32_t MinimumDelayMS = 40, MaximumDelayMS = 250;
class Timeline {
public:
    bool Push(Snapshot snapshot, uint64_t received) {
        if (!snapshot.ID || !snapshot.Time || snapshot.Time > MaxTime || received > MaxTime) return false;
        if (!m_States.empty() && (!Newer(snapshot.ID, m_States.back().ID) || snapshot.Time <= m_States.back().Time)) return false;
        // Minimum arrival offset avoids translating packet jitter into animation.
        // It may rise slowly (8 ms/s), so a lasting route change does not leave
        // every later state late against an old, faster path.
        const int64_t offset = int64_t(received) - int64_t(snapshot.Time);
        if (m_States.empty()) m_Offset = offset;
        else m_Offset = std::min(offset, m_Offset + int64_t(received - std::min(received, m_LastReceived)) / 128);
        // Each state must arrive before presentation passes its predecessor.
        // Loading cadence and outages (200 ms or more) are excluded; a stalled
        // stream freezes rather than buffering every later state that long.
        if (!m_States.empty() && snapshot.Time - m_States.back().Time < 200) {
            m_Required[m_RequiredNext++ % m_Required.size()] = uint32_t(std::clamp<int64_t>(int64_t(snapshot.Time - m_States.back().Time) + offset - m_Offset, 0, MaximumDelayMS));
            const size_t count = std::min<size_t>(m_RequiredNext, m_Required.size());
            if (count >= 16) {
                std::array<uint32_t, 64> sorted{}; std::copy_n(m_Required.begin(), count, sorted.begin()); std::sort(sorted.begin(), sorted.begin() + count);
                m_TargetDelay = float(std::clamp<uint32_t>(sorted[count * 9 / 10] + 5, MinimumDelayMS, MaximumDelayMS));
            }
        }
        m_LastReceived = received;
        m_States.push_back(std::move(snapshot)); while (m_States.size() > 8) m_States.pop_front(); return true;
    }
    bool Empty() const { return m_States.empty(); }
    const Snapshot& Latest() const { return m_States.back(); }
    float Delay() const { return m_Delay; }
    std::unordered_set<uint64_t> Resources() const { std::unordered_set<uint64_t> ids; for (const auto& s : m_States) for (const auto& n : s.Nodes) if (n.Asset) ids.insert(n.Asset); return ids; }
    Snapshot Sample(uint64_t now) const {
        if (m_States.empty()) return {};
        const float elapsed = m_LastSample && now > m_LastSample ? float(std::min<uint64_t>(now - m_LastSample, 100)) : 0;
        // Change the delay gradually (at most 10% of elapsed time) so presentation
        // speeds up or slows down slightly instead of jumping.
        if (elapsed > 0) { const float step = elapsed * .1f; m_Delay += std::clamp(m_TargetDelay - m_Delay, -step, step); }
        m_LastSample = now;
        const int64_t target = int64_t(now) - m_Offset - int64_t(m_Delay);
        const auto [before, after, alpha] = Find(target);
        Snapshot result = alpha < 1 ? *before : *after;
        if (before->ControlledActor == after->ControlledActor) {
            result.AimX = Blend(before->AimX, after->AimX, std::min(alpha, 1.0f)); result.AimY = Blend(before->AimY, after->AimY, std::min(alpha, 1.0f));
            result.LookX = Blend(before->LookX, after->LookX, std::min(alpha, 1.0f)); result.LookY = Blend(before->LookY, after->LookY, std::min(alpha, 1.0f));
        }
        if (std::abs(Displacement(before->CameraX, after->CameraX, after->SceneWidth, after->Wrap & 1)) <= 256 && std::abs(Displacement(before->CameraY, after->CameraY, after->SceneHeight, after->Wrap & 2)) <= 256) {
            result.CameraX = Blend(before->CameraX, after->CameraX, alpha, after->SceneWidth, after->Wrap & 1);
            result.CameraY = Blend(before->CameraY, after->CameraY, alpha, after->SceneHeight, after->Wrap & 2);
        }
        Interpolate(*before, *after, alpha, result.Nodes, nullptr);
        PresentOwnActor(result, now, target, elapsed);
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
    // How far the controlled actor may run ahead of its newest state.
    static constexpr int64_t OwnLeadMS = 60;
    void Reset() { *this = Timeline(); }
private:
    struct Bracket { const Snapshot* Before; const Snapshot* After; float Alpha; };
    Bracket Find(int64_t target) const {
        const Snapshot* before = &m_States.front(); const Snapshot* after = before;
        for (const auto& state : m_States) { if (int64_t(state.Time) <= target) before = &state; if (int64_t(state.Time) >= target) { after = &state; break; } after = &state; }
        // After the newest update, a small bounded continuation covers jitter;
        // we freeze rather than invent unbounded motion on a lost connection.
        if (before == after && target > int64_t(after->Time) && m_States.size() > 1) before = &m_States[m_States.size() - 2];
        const float dt = float(after->Time - before->Time);
        return {before, after, dt > 0 ? std::clamp(float(target - int64_t(before->Time)) / dt, 0.0f, 1.0f + ExtrapolationMS / dt) : 1};
    }
    // Moves `nodes` (taken from the selected state) between `before` and `after`.
    // With `only`, just those node IDs are considered.
    static void Interpolate(const Snapshot& before, const Snapshot& after, float alpha, std::vector<Node>& nodes, const std::unordered_set<uint64_t>* only) {
        std::unordered_map<uint64_t, const Node*> old, next;
        for (const auto& n : before.Nodes) if (!only || only->contains(n.ID)) old[n.ID] = &n;
        for (const auto& n : after.Nodes) if (!only || only->contains(n.ID)) next[n.ID] = &n;
        for (auto& n : nodes) {
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
            if ((to.Flags & Discontinuous) || ((to.Flags & ScreenSpace) && !(to.Flags & Layer)) || from.Type != to.Type || from.Parent != to.Parent || ((from.Flags ^ to.Flags) & (FlipX | FlipY)) || std::abs(Displacement(from.X, to.X, after.SceneWidth, after.Wrap & 1)) > 256 || std::abs(Displacement(from.Y, to.Y, after.SceneHeight, after.Wrap & 2)) > 256) continue;
            n.X = Blend(from.X, to.X, alpha, after.SceneWidth, !(to.Flags & ScreenSpace) && (after.Wrap & 1)); n.Y = Blend(from.Y, to.Y, alpha, after.SceneHeight, !(to.Flags & ScreenSpace) && (after.Wrap & 2));
            n.Angle = Blend(from.Angle, to.Angle, alpha, 6.28318530718f, true);
        }
        // Keep attachments on their native parent transforms while interpolating
        // changing joint offsets. World-space interpolation alone separates a
        // limb from a rapidly rotating body between snapshots.
        std::unordered_map<uint64_t, Node*> sampled;
        for (auto& n : nodes) sampled[n.ID] = &n;
        std::unordered_set<uint64_t> resolved, visiting;
        auto resolve = [&](auto&& self, Node& n, unsigned depth) -> void {
            if (!n.Parent || depth > 32 || resolved.contains(n.ID) || visiting.contains(n.ID)) return;
            visiting.insert(n.ID);
            auto a = old.find(n.ID), b = next.find(n.ID), pa = old.find(n.Parent), pb = next.find(n.Parent); auto parent = sampled.find(n.Parent);
            if (a != old.end() && b != next.end() && pa != old.end() && pb != next.end() && parent != sampled.end() && a->second->Parent == b->second->Parent && !(n.Flags & (ScreenSpace | Discontinuous)) && ((pa->second->Flags ^ pb->second->Flags) & FlipX) == 0) {
                self(self, *parent->second, depth + 1);
                const auto& from = *a->second; const auto& to = *b->second; const auto& p0 = *pa->second; const auto& p1 = *pb->second;
                if (std::abs(Displacement(p0.X, p1.X, after.SceneWidth, after.Wrap & 1)) <= 256 && std::abs(Displacement(p0.Y, p1.Y, after.SceneHeight, after.Wrap & 2)) <= 256) {
                    auto local = [&](const Node& child, const Node& p) { const float dx = Displacement(p.X, child.X, after.SceneWidth, after.Wrap & 1), dy = Displacement(p.Y, child.Y, after.SceneHeight, after.Wrap & 2); const float c = std::cos(p.Angle), s = std::sin(p.Angle); return std::pair{dx * c - dy * s, dx * s + dy * c}; };
                    auto l0 = local(from, p0), l1 = local(to, p1); const float x = Blend(l0.first, l1.first, alpha), y = Blend(l0.second, l1.second, alpha); const auto& p = *parent->second; const float c = std::cos(p.Angle), s = std::sin(p.Angle);
                    n.X = p.X + x * c + y * s; n.Y = p.Y - x * s + y * c;
                    n.Angle = p.Angle + Blend(from.Angle - p0.Angle, to.Angle - p1.Angle, alpha, 6.28318530718f, true);
                }
            }
            visiting.erase(n.ID); resolved.insert(n.ID);
        };
        for (auto& n : nodes) resolve(resolve, n, 0);
    }
    // The player's own actor and everything attached to it are presented at
    // the host's present time, continued from the two newest states, instead
    // of the world's buffered time. Input then shows on screen one buffer
    // sooner. When a new state disagrees with that continuation, the
    // difference fades out instead of jumping.
    void PresentOwnActor(Snapshot& result, uint64_t now, int64_t target, float elapsed) const {
        const Snapshot& newest = m_States.back();
        const int64_t ownTarget = std::min<int64_t>(int64_t(now) - m_Offset, int64_t(newest.Time) + OwnLeadMS);
        if (!newest.ControlledActor || m_States.size() < 2 || ownTarget <= target) { m_OwnActor = 0; m_OwnShiftX = m_OwnShiftY = 0; return; }
        std::unordered_map<uint64_t, uint64_t> parents; for (const auto& n : newest.Nodes) if (!(n.Flags & ScreenSpace)) parents[n.ID] = n.Parent;
        std::unordered_set<uint64_t> own;
        for (const auto& [id, parent] : parents) {
            uint64_t node = id; for (unsigned depth = 0; node && depth < 32 && node != newest.ControlledActor; ++depth) { const auto up = parents.find(node); node = up == parents.end() ? 0 : up->second; }
            if (node == newest.ControlledActor) own.insert(id);
        }
        const auto [before, after, alpha] = Find(ownTarget);
        std::vector<Node> nodes; for (const auto& n : (alpha < 1 ? *before : *after).Nodes) if (own.contains(n.ID)) nodes.push_back(n);
        Interpolate(*before, *after, alpha, nodes, &own);
        const auto root = std::find_if(nodes.begin(), nodes.end(), [&](const Node& n) { return n.ID == newest.ControlledActor; });
        if (root == nodes.end()) { m_OwnActor = 0; m_OwnShiftX = m_OwnShiftY = 0; return; }
        m_OwnShiftX *= std::exp(-elapsed / 80.0f); m_OwnShiftY *= std::exp(-elapsed / 80.0f);
        if (m_OwnActor == newest.ControlledActor && m_OwnState != newest.ID) {
            const float jumpX = Displacement(root->X, m_OwnX, newest.SceneWidth, newest.Wrap & 1), jumpY = Displacement(root->Y, m_OwnY, newest.SceneHeight, newest.Wrap & 2);
            // A teleport, respawn or long outage is shown as it is.
            if (std::abs(jumpX) <= 64 && std::abs(jumpY) <= 64) { m_OwnShiftX = jumpX; m_OwnShiftY = jumpY; } else m_OwnShiftX = m_OwnShiftY = 0;
        }
        m_OwnActor = newest.ControlledActor; m_OwnState = newest.ID;
        std::unordered_map<uint64_t, const Node*> byID; for (auto& n : nodes) { Translate(n, m_OwnShiftX, m_OwnShiftY); byID[n.ID] = &n; }
        m_OwnX = root->X; m_OwnY = root->Y;
        for (auto& n : result.Nodes) if (const auto found = byID.find(n.ID); found != byID.end()) n = *found->second;
    }
    std::deque<Snapshot> m_States;
    int64_t m_Offset = 0;
    uint64_t m_LastReceived = 0;
    std::array<uint32_t, 64> m_Required{};
    size_t m_RequiredNext = 0;
    float m_TargetDelay = float(InterpolationMS);
    mutable float m_Delay = float(InterpolationMS);
    mutable uint64_t m_LastSample = 0;
    // The own actor's last presented root position and fading correction.
    mutable uint64_t m_OwnActor = 0;
    mutable uint32_t m_OwnState = 0;
    mutable float m_OwnX = 0, m_OwnY = 0, m_OwnShiftX = 0, m_OwnShiftY = 0;
};
} // namespace RTE::MP::World
