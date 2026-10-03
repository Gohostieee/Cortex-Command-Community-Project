#include "MultiplayerWorld.h"
#include "MovableObject.h"
#include "MOSRotating.h"
#include "SceneMan.h"
#include "Scene.h"
#include "SLTerrain.h"
#include "SLBackground.h"
#include "ActivityMan.h"
#include "Activity.h"
#include "FrameMan.h"
#include "CameraMan.h"
#include "PostProcessMan.h"
#include "PresetMan.h"
#include "GLResourceMan.h"
#include "RenderTarget.h"
#include "Shader.h"
#include "GraphicalPrimitive.h"
#include "Draw.h"
#include "allegro.h"
#include "allegro/internal/aintern.h"
#include <unordered_map>
#include <chrono>
#include <atomic>
#include <mutex>

using namespace RTE;
using namespace RTE::MP::World;
namespace {
thread_local MultiplayerWorld* objectCollector = nullptr;
thread_local MultiplayerWorld* canvasCollector = nullptr;
std::atomic<MultiplayerWorld*> trailCollector = nullptr;
uint64_t WorldNow() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
constexpr size_t ResourceLimit = 128 * 1024 * 1024;
}
struct MultiplayerWorld::Impl {
    MultiplayerWorld* Owner = nullptr;
    std::vector<Node> Objects, Canvas;
    std::deque<Node> Trails;
    std::mutex TrailMutex;
    uint64_t TrailSequence = 0, TrailTime = 0, TrailDuration = 17;
    uint64_t HUDParent = 0;
    Vector HUDAnchor;
    std::unordered_map<uint64_t, Vector> ObjectAnchors;
    bool PrimitiveStyle = false;
    Node CurrentStyle;
    std::unordered_map<uint64_t, uint8_t> Ordinals;
    std::unordered_map<uint64_t, Resource> Resources;
    std::unordered_map<uint64_t, Texture2D> Textures;
    std::unordered_map<BITMAP*, uint64_t> StaticAssets;
    std::unordered_map<uint64_t, uint64_t> LastUsed;
    size_t ResourceBytes = 0;
    Timeline States;
    std::unique_ptr<RenderTarget> Target;
    std::unique_ptr<Shader> SceneShader;
    uint64_t RenderCount = 0, UpdateCount = 0;
    uint64_t IntermediateCount = 0;
    Snapshot LastSample;
    std::unordered_map<uint64_t, Node> LastVisuals;
    BITMAP* GUI = nullptr;
    GFX_VTABLE* OriginalVTable = nullptr;
    GFX_VTABLE CanvasVTable{};
    Vector Camera;

    bool Store(Resource resource) {
        const auto id = resource.ID; const uint64_t now = WorldNow(); LastUsed[id] = now;
        if (Resources.contains(id)) return true;
        if (ResourceBytes + resource.Pixels.size() > ResourceLimit) {
            auto pinned = States.Resources(); for (const auto& [id, visual] : LastVisuals) if (visual.Asset) pinned.insert(visual.Asset);
            std::vector<std::pair<uint64_t, uint64_t>> candidates;
            for (const auto& [key, used] : LastUsed) if (now - used > 5000 && !pinned.contains(key)) candidates.emplace_back(used, key);
            std::sort(candidates.begin(), candidates.end());
            for (const auto& [used, key] : candidates) {
                auto found = Resources.find(key); if (found == Resources.end()) continue;
                ResourceBytes -= found->second.Pixels.size(); Resources.erase(found); LastUsed.erase(key);
                if (auto texture = Textures.find(key); texture != Textures.end()) { rlUnloadTexture(texture->second.id); Textures.erase(texture); }
                if (ResourceBytes + resource.Pixels.size() <= ResourceLimit * 9 / 10) break;
            }
        }
        if (ResourceBytes + resource.Pixels.size() > ResourceLimit) { LastUsed.erase(id); return false; }
        ResourceBytes += resource.Pixels.size(); Resources.emplace(id, std::move(resource)); return true;
    }

    uint64_t Asset(BITMAP* bitmap, int sx = 0, int sy = 0, int width = -1, int height = -1) {
        if (!bitmap || (bitmap_color_depth(bitmap) != 8 && bitmap_color_depth(bitmap) != 32)) return 0;
        width = width < 0 ? bitmap->w : width; height = height < 0 ? bitmap->h : height;
        sx = std::clamp(sx, 0, bitmap->w); sy = std::clamp(sy, 0, bitmap->h);
        width = std::clamp(width, 0, bitmap->w - sx); height = std::clamp(height, 0, bitmap->h - sy);
        if (!width || !height || width > 4096 || height > 4096 || uint64_t(width) * height * (bitmap_color_depth(bitmap) / 8) > MaxPayload - 256) return 0;
        Resource resource; resource.Width = uint16_t(width); resource.Height = uint16_t(height); resource.Depth = uint8_t(bitmap_color_depth(bitmap));
        const int stride = width * (resource.Depth / 8); resource.Pixels.resize(size_t(stride) * height);
        for (int y = 0; y < height; ++y) {
            std::memcpy(resource.Pixels.data() + size_t(y) * stride, bitmap->line[sy + y] + sx * (resource.Depth / 8), stride);
        }
        resource.ID = ResourceHash(resource); const uint64_t id = resource.ID;
        return Store(std::move(resource)) ? id : 0;
    }
    uint64_t StaticAsset(BITMAP* bitmap) {
        if (auto found = StaticAssets.find(bitmap); found != StaticAssets.end() && Resources.contains(found->second)) { LastUsed[found->second] = WorldNow(); return found->second; }
        const auto id = Asset(bitmap); if (id) StaticAssets[bitmap] = id; return id;
    }
    void AppendCanvas(Node node) {
        node.ID = (uint64_t(1) << 63) | uint64_t(Canvas.size() + 1); node.Flags |= ScreenSpace;
		if (PrimitiveStyle) { node.BlendMode = CurrentStyle.BlendMode; node.TintR = CurrentStyle.TintR; node.TintG = CurrentStyle.TintG; node.TintB = CurrentStyle.TintB; node.Alpha = uint8_t(unsigned(node.Alpha) * CurrentStyle.Alpha / 255); }
		else if (_drawing_mode == DRAW_MODE_TRANS) node.Alpha = uint8_t(unsigned(node.Alpha) * g_FrameMan.GetCurrentAlpha() / 255);
		if (HUDParent) {
			node.Parent = HUDParent; node.X -= HUDAnchor.GetX(); node.Y -= HUDAnchor.GetY();
			if (node.Type == Shape::Line || node.Type == Shape::Triangle || node.Type == Shape::Spline) { node.X2 -= HUDAnchor.GetX(); node.Y2 -= HUDAnchor.GetY(); node.X3 -= HUDAnchor.GetX(); node.Y3 -= HUDAnchor.GetY(); }
			if (node.Type == Shape::Spline) { node.Width -= HUDAnchor.GetX(); node.Height -= HUDAnchor.GetY(); }
		}
		if (GUI && GUI->clip) { node.ClipX = uint16_t(std::max(0, GUI->cl)); node.ClipY = uint16_t(std::max(0, GUI->ct)); node.ClipWidth = uint16_t(std::max(0, GUI->cr - GUI->cl)); node.ClipHeight = uint16_t(std::max(0, GUI->cb - GUI->ct)); }
        if (Canvas.size() < MaxNodes && Valid(node)) Canvas.push_back(node);
    }
    void AppendObject(const MovableObject& owner, Node node) {
        const uint64_t id = uint64_t(owner.GetUniqueID());
        node.ID = (id << 8) | Ordinals[id]++;
        if (!(node.ID & 255)) ObjectAnchors[node.ID] = Vector(node.X, node.Y);
        if (const auto* parent = owner.GetParent()) node.Parent = uint64_t(parent->GetUniqueID()) << 8;
        if (Objects.size() < MaxNodes && Valid(node)) Objects.push_back(node);
    }
    static Impl* CanvasFor(BITMAP* bitmap) {
        return canvasCollector && canvasCollector->m_Impl->GUI == bitmap ? canvasCollector->m_Impl.get() : nullptr;
    }
    static void Rectangle(BITMAP* bitmap, int x1, int y1, int x2, int y2, int color) {
        if (auto* impl = CanvasFor(bitmap)) { Node n; n.Type = Shape::Rectangle; n.X = float(x1); n.Y = float(y1); n.Width = float(x2 - x1 + 1); n.Height = float(y2 - y1 + 1); n.Color = uint8_t(color); impl->AppendCanvas(n); }
    }
    static void PutPixel(BITMAP* bitmap, int x, int y, int color) { Rectangle(bitmap, x, y, x, y, color); }
    static void HLine(BITMAP* bitmap, int x1, int y, int x2, int color) { Rectangle(bitmap, std::min(x1, x2), y, std::max(x1, x2), y, color); }
    static void VLine(BITMAP* bitmap, int x, int y1, int y2, int color) { Rectangle(bitmap, x, std::min(y1, y2), x, std::max(y1, y2), color); }
    static void Line(BITMAP* bitmap, int x1, int y1, int x2, int y2, int color) {
        if (auto* impl = CanvasFor(bitmap)) { Node n; n.Type = Shape::Line; n.X = float(x1); n.Y = float(y1); n.X2 = float(x2); n.Y2 = float(y2); n.Color = uint8_t(color); impl->AppendCanvas(n); }
    }
    static void Triangle(BITMAP* bitmap, int x1, int y1, int x2, int y2, int x3, int y3, int color) {
        if (auto* impl = CanvasFor(bitmap)) { Node n; n.Type = Shape::Triangle; n.X = float(x1); n.Y = float(y1); n.X2 = float(x2); n.Y2 = float(y2); n.X3 = float(x3); n.Y3 = float(y3); n.Color = uint8_t(color); impl->AppendCanvas(n); }
    }
    static void Blit(BITMAP* source, BITMAP* dest, int sx, int sy, int dx, int dy, int width, int height, bool masked = false) {
        if (auto* impl = CanvasFor(dest)) {
            Node n; n.Asset = impl->Asset(source); if (!n.Asset) return;
            // Match Allegro source clipping before recording the retained quad.
            if (sx < 0) { dx -= sx; width += sx; sx = 0; } if (sy < 0) { dy -= sy; height += sy; sy = 0; }
            width = std::min(width, source->w - sx); height = std::min(height, source->h - sy);
            if (width <= 0 || height <= 0) return;
            n.X = float(dx); n.Y = float(dy); n.Width = n.SourceWidth = float(width); n.Height = n.SourceHeight = float(height); n.SourceX = float(sx); n.SourceY = float(sy); n.Flags = masked ? Masked : 0;
            impl->AppendCanvas(n);
        }
    }
    static void PlainBlit(BITMAP* s, BITMAP* d, int sx, int sy, int dx, int dy, int w, int h) { Blit(s, d, sx, sy, dx, dy, w, h); }
    static void MaskedBlit(BITMAP* s, BITMAP* d, int sx, int sy, int dx, int dy, int w, int h) { Blit(s, d, sx, sy, dx, dy, w, h, true); }
    static void Sprite(BITMAP* dest, BITMAP* source, int x, int y, uint8_t flags = Masked, int color = -1) {
        if (auto* impl = CanvasFor(dest)) { Node n; n.Asset = impl->Asset(source); n.X = float(x); n.Y = float(y); n.Width = n.SourceWidth = float(source->w); n.Height = n.SourceHeight = float(source->h); n.Flags = flags; if (color >= 0) { n.Flags |= SolidColor; n.Color = uint8_t(color); } impl->AppendCanvas(n); }
    }
    static void PlainSprite(BITMAP* d, BITMAP* s, int x, int y) { Sprite(d, s, x, y); }
    static void TransSprite(BITMAP* d, BITMAP* s, int x, int y) { if (auto* impl = CanvasFor(d)) { const size_t first = impl->Canvas.size(); Sprite(d, s, x, y); if (impl->Canvas.size() > first) impl->Canvas.back().Alpha = g_FrameMan.GetCurrentAlpha(); } }
    static void Clear(BITMAP* d, int color) { if (auto* impl = CanvasFor(d)) { impl->Canvas.clear(); if (color != g_MaskColor) Rectangle(d, 0, 0, d->w - 1, d->h - 1, color); } }
    static void LitSprite(BITMAP* d, BITMAP* s, int x, int y, int color) {
        BITMAP* decoded = create_bitmap_ex(bitmap_color_depth(s), s->w, s->h); if (!decoded) return;
        clear_to_color(decoded, bitmap_mask_color(decoded)); draw_lit_sprite(decoded, s, 0, 0, color); Sprite(d, decoded, x, y); destroy_bitmap(decoded);
    }
    static void RLE(BITMAP* d, const RLE_SPRITE* s, int x, int y) {
        BITMAP* decoded = create_bitmap_ex(s->color_depth, s->w, s->h); if (!decoded) return;
        clear_to_color(decoded, bitmap_mask_color(decoded)); draw_rle_sprite(decoded, s, 0, 0); Sprite(d, decoded, x, y); destroy_bitmap(decoded);
    }
    static void TransRLE(BITMAP* d, const RLE_SPRITE* s, int x, int y) { if (auto* impl = CanvasFor(d)) { const size_t first = impl->Canvas.size(); RLE(d, s, x, y); if (impl->Canvas.size() > first) impl->Canvas.back().Alpha = g_FrameMan.GetCurrentAlpha(); } }
    static void LitRLE(BITMAP* d, const RLE_SPRITE* s, int x, int y, int color) {
        BITMAP* decoded = create_bitmap_ex(s->color_depth, s->w, s->h); if (!decoded) return;
        clear_to_color(decoded, bitmap_mask_color(decoded)); draw_lit_rle_sprite(decoded, s, 0, 0, color); Sprite(d, decoded, x, y); destroy_bitmap(decoded);
    }
    static void Glyph(BITMAP* d, const FONT_GLYPH* glyph, int x, int y, int color, int bg) {
        BITMAP* decoded = create_bitmap_ex(8, glyph->w, glyph->h); if (!decoded) return;
        clear_to_color(decoded, g_MaskColor); _linear_draw_glyph8(decoded, glyph, 0, 0, 1, -1);
        if (bg >= 0 && bg != g_MaskColor) Rectangle(d, x, y, x + glyph->w - 1, y + glyph->h - 1, bg);
        Sprite(d, decoded, x, y, Masked, color); destroy_bitmap(decoded);
    }
    static void HFlip(BITMAP* d, BITMAP* s, int x, int y) { Sprite(d, s, x, y, Masked | FlipX); }
    static void VFlip(BITMAP* d, BITMAP* s, int x, int y) { Sprite(d, s, x, y, Masked | FlipY); }
    static void VHFlip(BITMAP* d, BITMAP* s, int x, int y) { Sprite(d, s, x, y, Masked | FlipX | FlipY); }
    static void Character(BITMAP* d, BITMAP* s, int x, int y, int color, int bg) { if (bg >= 0) Rectangle(d, x, y, x + s->w - 1, y + s->h - 1, bg); Sprite(d, s, x, y, Masked, color); }
    static void Pivot(BITMAP* d, BITMAP* s, fixed x, fixed y, fixed cx, fixed cy, fixed angle, fixed scale, int flip) {
        if (auto* impl = CanvasFor(d)) { Node n; n.Asset = impl->Asset(s); n.X = fixtof(x); n.Y = fixtof(y); n.Width = s->w * fixtof(scale); n.Height = s->h * fixtof(scale); n.PivotX = fixtof(cx) * fixtof(scale); n.PivotY = fixtof(cy) * fixtof(scale); n.Angle = -fixtof(angle) * 6.28318530718f / 256; n.SourceWidth = float(s->w); n.SourceHeight = float(s->h); n.Flags = Masked | (flip ? FlipY : 0); impl->AppendCanvas(n); }
    }
    template<class LayerType> void Layer(std::vector<Node>& output, LayerType* layer, uint16_t ordinal, const Vector& camera, int width, int height) {
        if (!layer || !layer->GetBitmap()) return;
        BITMAP* bitmap = layer->GetBitmap(); const Vector scale = layer->GetScaleFactor(); Vector offset = layer->GetOffset();
        if (scale.GetX() <= 0 || scale.GetY() <= 0) return;
        if constexpr (std::is_same_v<LayerType, SLBackground>) {
            if (!layer->IsAutoScrolling()) { const Vector ratio = layer->GetScrollRatio(); offset.SetXY(std::floor(offset.GetX() * ratio.GetX()), std::floor(offset.GetY() * ratio.GetY())); }
        }
        const int boxX = !g_SceneMan.SceneWrapsX() && width > g_SceneMan.GetSceneWidth() ? (width - g_SceneMan.GetSceneWidth()) / 2 : 0;
        const int boxY = !g_SceneMan.SceneWrapsY() && height > g_SceneMan.GetSceneHeight() ? (height - g_SceneMan.GetSceneHeight()) / 2 : 0;
        if (!layer->WrapsX() && boxX) offset.SetX(0);
        if (!layer->WrapsY() && boxY) offset.SetY(0);
        offset -= layer->GetOriginOffset(); layer->WrapPosition(offset);
        const float spanX = bitmap->w * scale.GetX(), spanY = bitmap->h * scale.GetY();
        const int margin = 128;
        const int columns = (bitmap->w + TileSize - 1) / TileSize, rows = (bitmap->h + TileSize - 1) / TileSize;
        const int repeatX = layer->WrapsX() ? int(std::floor((offset.GetX() - margin) / spanX)) : 0;
        const int endX = layer->WrapsX() ? int(std::ceil((offset.GetX() + width + margin) / spanX)) : 1;
        const int repeatY = layer->WrapsY() ? int(std::floor((offset.GetY() - margin) / spanY)) : 0;
        const int endY = layer->WrapsY() ? int(std::ceil((offset.GetY() + height + margin) / spanY)) : 1;
        for (int rx = repeatX; rx < endX; ++rx) for (int ry = repeatY; ry < endY; ++ry) {
          const int firstX = std::clamp(int(std::floor((offset.GetX() - margin - rx * spanX) / (TileSize * scale.GetX()))), 0, columns);
          const int lastX = std::clamp(int(std::ceil((offset.GetX() + width + margin - rx * spanX) / (TileSize * scale.GetX()))), 0, columns);
          const int firstY = std::clamp(int(std::floor((offset.GetY() - margin - ry * spanY) / (TileSize * scale.GetY()))), 0, rows);
          const int lastY = std::clamp(int(std::ceil((offset.GetY() + height + margin - ry * spanY) / (TileSize * scale.GetY()))), 0, rows);
          for (int y = firstY; y < lastY; ++y) for (int x = firstX; x < lastX; ++x) {
            int sx = x * TileSize, sy = y * TileSize, w = std::min<int>(TileSize, bitmap->w - sx), h = std::min<int>(TileSize, bitmap->h - sy);
            Node n; n.ID = (uint64_t(1) << 62) | (uint64_t(ordinal) << 48) | (uint64_t(uint16_t(ry * rows + y)) << 24) | uint16_t(rx * columns + x);
            n.Asset = Asset(bitmap, sx, sy, w, h); n.SourceWidth = float(w); n.SourceHeight = float(h); n.Width = w * scale.GetX(); n.Height = h * scale.GetY();
            n.Flags = ScreenSpace | MP::World::Layer | (layer->GetDrawMasked() ? Masked : 0);
            n.X2 = layer->WrapsX() ? spanX : 0; n.Y2 = layer->WrapsY() ? spanY : 0;
            n.X = boxX + rx * spanX + sx * scale.GetX() - offset.GetX(); n.Y = boxY + ry * spanY + sy * scale.GetY() - offset.GetY();
            n.ClipX = boxX; n.ClipY = boxY; n.ClipWidth = width - boxX * 2; n.ClipHeight = height - boxY * 2;
            if (Valid(n)) output.push_back(n);
          }
        }
        if constexpr (std::is_same_v<LayerType, SLBackground>) {
            auto fill = [&](float x, float y, float w, float h, int color, uint16_t edge) {
                if (w <= 0 || h <= 0 || color == g_MaskColor) return;
                Node n; n.ID = (uint64_t(1) << 61) | (uint64_t(ordinal) << 32) | edge; n.Type = Shape::Rectangle; n.Flags = ScreenSpace | MP::World::Layer; n.X = x; n.Y = y; n.Width = w; n.Height = h; n.Color = uint8_t(color); output.push_back(n);
            };
            if (!layer->WrapsX() && spanX <= width) { fill(0, 0, -offset.GetX(), height, getpixel(bitmap, 0, bitmap->h / 2), 1); fill(spanX - offset.GetX(), 0, width - spanX + offset.GetX(), height, getpixel(bitmap, bitmap->w - 1, bitmap->h / 2), 2); }
            if (!layer->WrapsY() && spanY <= height) { fill(0, 0, width, -offset.GetY(), getpixel(bitmap, bitmap->w / 2, 0), 3); fill(0, spanY - offset.GetY(), width, height - spanY + offset.GetY(), getpixel(bitmap, bitmap->w / 2, bitmap->h - 1), 4); }
        }
        (void)camera;
    }
    Texture2D Texture(uint64_t id) {
        if (auto found = Textures.find(id); found != Textures.end()) return found->second;
        auto found = Resources.find(id); if (found == Resources.end()) return {};
        const auto& r = found->second; glPixelStorei(GL_UNPACK_ALIGNMENT, r.Depth == 8 ? 1 : 4);
        Texture2D texture{rlLoadTexture(r.Pixels.data(), r.Width, r.Height, r.Depth == 8 ? PIXELFORMAT_UNCOMPRESSED_GRAYSCALE : PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1), r.Width, r.Height, 1, 0};
        Textures[id] = texture; return texture;
    }
};
MultiplayerWorld::MultiplayerWorld() : m_Impl(std::make_unique<Impl>()) { m_Impl->Owner = this; }
MultiplayerWorld::~MultiplayerWorld() { Reset(); }
void MultiplayerWorld::Reset() {
    EndObjects(); if (canvasCollector == this) { if (m_Impl->GUI) m_Impl->GUI->vtable = m_Impl->OriginalVTable; canvasCollector = nullptr; }
    if (trailCollector.load() == this) trailCollector.store(nullptr);
    for (const auto& [id, texture] : m_Impl->Textures) rlUnloadTexture(texture.id);
    m_Impl = std::make_unique<Impl>(); m_Impl->Owner = this;
}
void MultiplayerWorld::ResetPresentation() {
    auto& impl = *m_Impl;
    impl.States.Reset(); impl.LastSample = {}; impl.LastVisuals.clear();
    impl.RenderCount = impl.UpdateCount = impl.IntermediateCount = 0;
    impl.Target.reset();
}
void MultiplayerWorld::BeginObjects() { m_Impl->Objects.clear(); m_Impl->Ordinals.clear(); m_Impl->ObjectAnchors.clear(); objectCollector = this; }
void MultiplayerWorld::BeginTrails() {
    auto& impl = *m_Impl; const uint64_t now = WorldNow(); std::lock_guard lock(impl.TrailMutex);
    impl.TrailDuration = impl.TrailTime ? std::clamp<uint64_t>(now - impl.TrailTime, 8, 100) : 17; impl.TrailTime = now;
    while (!impl.Trails.empty() && impl.Trails.front().EndTime + 250 < now) impl.Trails.pop_front(); trailCollector.store(this);
}
void MultiplayerWorld::Trail(std::span<const std::pair<int, int>> positions, uint8_t color) {
    if (!color) return;
    auto* collector = trailCollector.load(); if (!collector) return; auto& impl = *collector->m_Impl; std::lock_guard lock(impl.TrailMutex);
    for (const auto& [x, y] : positions) { if (impl.Trails.size() >= MaxNodes / 2) break;
        Node n; n.ID = (uint64_t(1) << 59) | ++impl.TrailSequence; n.Type = Shape::Pixel; n.X = float(x); n.Y = float(y); n.Color = color; n.Flags = Discontinuous; n.StartTime = impl.TrailTime; n.EndTime = n.StartTime + impl.TrailDuration; impl.Trails.push_back(n);
    }
}
bool MultiplayerWorld::Flash(int width, int height, uint8_t color) { if (!canvasCollector) return false; Node n; n.Type = Shape::Flash; n.Width = float(width); n.Height = float(height); n.Color = color; canvasCollector->m_Impl->AppendCanvas(n); return true; }
void MultiplayerWorld::BeginHUD(const MovableObject& owner) {
    if (!canvasCollector) return; auto& impl = *canvasCollector->m_Impl; impl.HUDParent = uint64_t(owner.GetUniqueID()) << 8;
    auto found = impl.ObjectAnchors.find(impl.HUDParent); const Vector anchor = found != impl.ObjectAnchors.end() ? found->second : owner.GetPos();
    impl.HUDAnchor = Vector(impl.GUI->w / 2 + Displacement(impl.Camera.GetX() + impl.GUI->w / 2, anchor.GetX(), g_SceneMan.GetSceneWidth(), g_SceneMan.SceneWrapsX()), impl.GUI->h / 2 + Displacement(impl.Camera.GetY() + impl.GUI->h / 2, anchor.GetY(), g_SceneMan.GetSceneHeight(), g_SceneMan.SceneWrapsY()));
}
void MultiplayerWorld::EndHUD() { if (canvasCollector) canvasCollector->m_Impl->HUDParent = 0; }
void MultiplayerWorld::EndObjects() { if (objectCollector == this) objectCollector = nullptr; }
void MultiplayerWorld::Sprite(const MovableObject& owner, BITMAP* bitmap, const Vector& pos, const Vector& pivot, float angle, float scale, bool flip, bool white, uint8_t alpha) {
    if (!objectCollector || !bitmap) return;
    auto& impl = *objectCollector->m_Impl; Node n; n.Asset = impl.StaticAsset(bitmap); n.X = pos.GetX(); n.Y = pos.GetY(); n.Angle = angle; n.Width = bitmap->w * scale; n.Height = bitmap->h * scale; n.PivotX = pivot.GetX() * scale; n.PivotY = pivot.GetY() * scale; n.SourceWidth = float(bitmap->w); n.SourceHeight = float(bitmap->h); n.Flags = Masked | (flip ? FlipX : 0) | (white ? SolidColor : 0); n.Color = uint8_t(g_WhiteColor); n.Alpha = alpha; impl.AppendObject(owner, n);
}
void MultiplayerWorld::Pixel(const MovableObject& owner, const Vector& pos, uint8_t color) {
    if (!objectCollector || !color) return;
    Node n; n.Type = Shape::Pixel; n.X = pos.GetX(); n.Y = pos.GetY(); n.Color = color; objectCollector->m_Impl->AppendObject(owner, n);
}
void MultiplayerWorld::BeginView(BITMAP* gui, const Vector& camera) {
    auto& impl = *m_Impl; impl.Canvas.clear(); impl.GUI = gui; impl.Camera = camera; impl.OriginalVTable = gui->vtable; impl.CanvasVTable = *gui->vtable;
    auto& v = impl.CanvasVTable; v.putpixel = Impl::PutPixel; v.hline = v.hfill = Impl::HLine; v.vline = Impl::VLine; v.line = v.fastline = Impl::Line; v.rectfill = Impl::Rectangle; v.triangle = Impl::Triangle;
    v.clear_to_color = Impl::Clear; v.draw_sprite = v.draw_256_sprite = Impl::PlainSprite; v.draw_trans_sprite = v.draw_trans_rgba_sprite = Impl::TransSprite; v.draw_sprite_h_flip = Impl::HFlip; v.draw_sprite_v_flip = Impl::VFlip; v.draw_sprite_vh_flip = Impl::VHFlip; v.draw_character = Impl::Character;
    v.draw_rle_sprite = Impl::RLE; v.draw_trans_rle_sprite = v.draw_trans_rgba_rle_sprite = Impl::TransRLE; v.draw_lit_rle_sprite = Impl::LitRLE; v.draw_lit_sprite = Impl::LitSprite; v.draw_glyph = Impl::Glyph;
    v.blit_from_memory = v.blit_to_memory = v.blit_from_system = v.blit_to_system = v.blit_to_self = v.blit_to_self_forward = v.blit_to_self_backward = v.blit_between_formats = Impl::PlainBlit; v.masked_blit = Impl::MaskedBlit; v.pivot_scaled_sprite_flip = Impl::Pivot;
    gui->vtable = &v; canvasCollector = this;
}
Snapshot MultiplayerWorld::EndView(int player, uint32_t id, uint32_t inputSequence, uint64_t time) {
    auto& impl = *m_Impl; if (impl.GUI) impl.GUI->vtable = impl.OriginalVTable; canvasCollector = nullptr;
    Snapshot snapshot; snapshot.ID = id; snapshot.InputSequence = inputSequence; snapshot.Time = time; snapshot.Width = uint16_t(impl.GUI->w); snapshot.Height = uint16_t(impl.GUI->h); snapshot.CameraX = impl.Camera.GetX(); snapshot.CameraY = impl.Camera.GetY();
    snapshot.SceneWidth = uint16_t(g_SceneMan.GetSceneWidth()); snapshot.SceneHeight = uint16_t(g_SceneMan.GetSceneHeight()); snapshot.Wrap = (g_SceneMan.SceneWrapsX() ? 1 : 0) | (g_SceneMan.SceneWrapsY() ? 2 : 0);
    auto* scene = g_SceneMan.GetScene(); if (!scene) return snapshot;
    uint16_t layerID = 1;
    for (auto it = scene->GetBackLayers().rbegin(); it != scene->GetBackLayers().rend(); ++it) impl.Layer(snapshot.Nodes, *it, layerID++, impl.Camera, snapshot.Width, snapshot.Height);
    impl.Layer(snapshot.Nodes, scene->GetTerrain()->GetBGSceneLayer(), 100, impl.Camera, snapshot.Width, snapshot.Height);
    { std::lock_guard lock(impl.TrailMutex); for (const auto& n : impl.Trails) if (std::abs(Displacement(impl.Camera.GetX() + snapshot.Width / 2, n.X, snapshot.SceneWidth, snapshot.Wrap & 1)) < snapshot.Width / 2 + 128 && std::abs(Displacement(impl.Camera.GetY() + snapshot.Height / 2, n.Y, snapshot.SceneHeight, snapshot.Wrap & 2)) < snapshot.Height / 2 + 128) snapshot.Nodes.push_back(n); }
    for (const auto& node : impl.Objects) {
        const float dx = Displacement(impl.Camera.GetX() + snapshot.Width / 2, node.X, snapshot.SceneWidth, snapshot.Wrap & 1), dy = Displacement(impl.Camera.GetY() + snapshot.Height / 2, node.Y, snapshot.SceneHeight, snapshot.Wrap & 2);
        if (std::abs(dx) < snapshot.Width / 2 + node.Width + 128 && std::abs(dy) < snapshot.Height / 2 + node.Height + 128) snapshot.Nodes.push_back(node);
    }
    impl.Layer(snapshot.Nodes, scene->GetTerrain()->GetFGSceneLayer(), 101, impl.Camera, snapshot.Width, snapshot.Height);
    const auto* activity = g_ActivityMan.GetActivity(); const int team = activity->GetTeamOfPlayer(player);
    snapshot.Phase = uint8_t(activity->GetActivityState());
    if (activity->GetActivityState() != Activity::Editing) impl.Layer(snapshot.Nodes, scene->GetUnseenLayer(team), 102, impl.Camera, snapshot.Width, snapshot.Height);
    snapshot.Nodes.insert(snapshot.Nodes.end(), impl.Canvas.begin(), impl.Canvas.end()); impl.GUI = nullptr;
    std::list<PostEffect> effects;
    g_PostProcessMan.GetPostScreenEffectsWrapped(impl.Camera, snapshot.Width, snapshot.Height, effects, team);
    uint32_t effectID = 0;
    for (const auto& effect : effects) if (effect.m_Bitmap) {
        Node n; n.ID = (uint64_t(1) << 60) | ++effectID; n.Asset = impl.StaticAsset(effect.m_Bitmap);
        n.X = effect.m_Pos.GetX() + impl.Camera.GetX(); n.Y = effect.m_Pos.GetY() + impl.Camera.GetY(); n.Angle = effect.m_Angle;
        n.Width = n.SourceWidth = float(effect.m_Bitmap->w); n.Height = n.SourceHeight = float(effect.m_Bitmap->h); n.PivotX = n.Width / 2; n.PivotY = n.Height / 2;
        n.Flags = Additive | Discontinuous; n.TintR = n.TintG = n.TintB = uint8_t(std::clamp(effect.m_Strength, 0, 255)); if (Valid(n)) snapshot.Nodes.push_back(n);
    }
    if (snapshot.Nodes.size() > MaxNodes) snapshot.Nodes.resize(MaxNodes);
    return snapshot;
}
const Resource* MultiplayerWorld::FindResource(uint64_t id) const { auto found = m_Impl->Resources.find(id); return found == m_Impl->Resources.end() ? nullptr : &found->second; }
bool MultiplayerWorld::Install(Resource resource) {
    auto& impl = *m_Impl; if (resource.ID != ResourceHash(resource)) return false;
    return impl.Store(std::move(resource));
}
std::vector<uint64_t> MultiplayerWorld::Missing(const Snapshot& snapshot) const {
    std::unordered_set<uint64_t> ids; for (const auto& node : snapshot.Nodes) if (node.Asset && !m_Impl->Resources.contains(node.Asset)) ids.insert(node.Asset); return {ids.begin(), ids.end()};
}
bool MultiplayerWorld::Install(Snapshot snapshot, uint64_t time) { if ((!Ready() && !Missing(snapshot).empty()) || !m_Impl->States.Push(std::move(snapshot), time)) return false; ++m_Impl->UpdateCount; return true; }
bool MultiplayerWorld::Ready() const { return !m_Impl->States.Empty(); }
bool MultiplayerWorld::IsDeploying() const { return Ready() && m_Impl->LastSample.Phase == Activity::Editing; }
int MultiplayerWorld::Width() const { return Ready() ? m_Impl->States.Latest().Width : 0; }
int MultiplayerWorld::Height() const { return Ready() ? m_Impl->States.Latest().Height : 0; }
uint64_t MultiplayerWorld::Rendered() const { return m_Impl->RenderCount; }
uint64_t MultiplayerWorld::Updates() const { return m_Impl->UpdateCount; }
uint64_t MultiplayerWorld::IntermediateFrames() const { return m_Impl->IntermediateCount; }
unsigned MultiplayerWorld::Render(uint64_t time) {
    auto& impl = *m_Impl; if (impl.States.Empty()) return 0;
    Snapshot scene = impl.States.Sample(time);
    if (impl.LastSample.ID == scene.ID && (impl.LastSample.CameraX != scene.CameraX || impl.LastSample.CameraY != scene.CameraY || impl.LastSample.Nodes != scene.Nodes)) ++impl.IntermediateCount;
    impl.LastSample = scene;
    // An animation frame or changed terrain tile can arrive after its pose.
    // Retain its last complete visual while still presenting fresh movement.
    std::unordered_set<uint64_t> visible;
    for (auto& n : scene.Nodes) if (n.Type == Shape::Sprite) {
        visible.insert(n.ID);
        if (impl.Resources.contains(n.Asset)) impl.LastVisuals[n.ID] = n;
        else if (auto previous = impl.LastVisuals.find(n.ID); previous != impl.LastVisuals.end()) {
            const auto& old = previous->second;
            n.Asset = old.Asset; n.SourceX = old.SourceX; n.SourceY = old.SourceY; n.SourceWidth = old.SourceWidth; n.SourceHeight = old.SourceHeight;
            n.Width = old.Width; n.Height = old.Height; n.PivotX = old.PivotX; n.PivotY = old.PivotY;
        } else n.Asset = 0;
    }
    std::erase_if(impl.LastVisuals, [&](const auto& entry) { return !visible.contains(entry.first); });
    if (!impl.Target || impl.Target->GetSize().w != scene.Width || impl.Target->GetSize().h != scene.Height) impl.Target = std::make_unique<RenderTarget>(FloatRect(0, 0, scene.Width, scene.Height), FloatRect(0, 0, scene.Width, scene.Height));
    impl.Target->Begin(); rlDisableDepthTest(); rlEnableColorBlend(); rlSetBlendMode(RL_BLEND_ALPHA);
    if (!impl.SceneShader) impl.SceneShader = std::make_unique<Shader>("Base.rte/Shaders/Blit8.vert", "Base.rte/Shaders/Replica.frag");
    Shader& shader = *impl.SceneShader;
    shader.Begin(); shader.Enable(); rlSetUniformSampler(shader.GetUniformLocation("rtePalette"), g_PostProcessMan.GetPaletteTexture());
    std::array<int, 13> lastStyle{}; lastStyle.fill(-1);
    std::unordered_map<uint64_t, const Node*> anchors; for (const auto& n : scene.Nodes) if (!(n.Flags & ScreenSpace)) anchors[n.ID] = &n;
    for (const Node& n : scene.Nodes) {
        if (n.Type == Shape::Sprite && !n.Asset) continue;
        Texture2D texture{};
        const bool trueColor = n.Type == Shape::Sprite && impl.Resources.at(n.Asset).Depth == 32;
        if (n.Asset) impl.LastUsed[n.Asset] = time;
        if (n.Type == Shape::Sprite) { if (!impl.Textures.contains(n.Asset)) rlDrawRenderBatchActive(); texture = impl.Texture(n.Asset); if (!texture.id) continue; }
        float x = n.X, y = n.Y;
        float dx = 0, dy = 0;
        if ((n.Flags & ScreenSpace) && n.Parent) {
            auto parent = anchors.find(n.Parent); if (parent == anchors.end()) continue;
            dx = scene.Width / 2 + Displacement(scene.CameraX + scene.Width / 2, parent->second->X, scene.SceneWidth, scene.Wrap & 1); dy = scene.Height / 2 + Displacement(scene.CameraY + scene.Height / 2, parent->second->Y, scene.SceneHeight, scene.Wrap & 2); x += dx; y += dy;
        }
        if (!(n.Flags & ScreenSpace)) { x = scene.Width / 2 + Displacement(scene.CameraX + scene.Width / 2, x, scene.SceneWidth, scene.Wrap & 1); y = scene.Height / 2 + Displacement(scene.CameraY + scene.Height / 2, y, scene.SceneHeight, scene.Wrap & 2); }
        const std::array<int, 13> style{bool(n.Flags & Masked), bool(n.Flags & SolidColor), n.Flags & SolidColor ? n.Color : 0, trueColor, n.ClipX, n.ClipY, n.ClipWidth, n.ClipHeight, n.BlendMode, n.TintR, n.TintG, n.TintB, bool(n.Flags & Additive)};
        if (style != lastStyle) {
            rlDrawRenderBatchActive(); shader.Enable();
            const int left = std::min<int>(n.ClipX, scene.Width), top = std::min<int>(n.ClipY, scene.Height);
            const int right = std::min<int>(scene.Width, int(n.ClipX) + n.ClipWidth), bottom = std::min<int>(scene.Height, int(n.ClipY) + n.ClipHeight);
            glEnable(GL_SCISSOR_TEST); glScissor(left, scene.Height - bottom, std::max(0, right - left), std::max(0, bottom - top));
            shader.SetInt("drawMasked", style[0]); shader.SetInt("solidColor", style[1]); shader.SetFloat("paletteColor", n.Color / 255.0f); shader.SetInt("trueColor", trueColor);
            shader.SetBool("dissolve", n.BlendMode == BlendDissolve);
            if (n.Flags & Additive) { rlSetBlendFactorsSeparate(GL_ONE, GL_ONE_MINUS_SRC_COLOR, GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_FUNC_ADD, GL_FUNC_ADD); rlSetBlendMode(RL_BLEND_CUSTOM_SEPARATE); }
            else if (n.BlendMode == BlendDissolve) rlSetBlendMode(RL_BLEND_ALPHA);
            else g_FrameMan.SetBlendMode(n.BlendMode ? DrawBlendMode(n.BlendMode) : BlendTransparency);
            shader.Enable(); glUniform4f(shader.GetUniformLocation("rteColor"), n.TintR / 255.0f, n.TintG / 255.0f, n.TintB / 255.0f, 1.0f);
            lastStyle = style;
        }
        if (n.Type == Shape::Sprite) {
            auto source = Rectangle{n.SourceX, n.SourceY, n.Flags & FlipX ? -n.SourceWidth : n.SourceWidth, n.Flags & FlipY ? -n.SourceHeight : n.SourceHeight};
            DrawTexturePro(texture, source, {x, y, n.Width, n.Height}, {n.PivotX, n.PivotY}, n.Angle, {255, 255, 255, n.Alpha});
        } else {
            RLColor color{n.Color, 0, 0, n.Alpha};
            if (n.Type == Shape::Pixel) DrawRectangle(int(x), int(y), 1, 1, color);
            else if (n.Type == Shape::Rectangle) DrawRectangle(int(x), int(y), int(n.Width), int(n.Height), color);
            else if (n.Type == Shape::Line) DrawLineEx({x, y}, {n.X2 + dx, n.Y2 + dy}, n.Width, color);
            else if (n.Type == Shape::Triangle) DrawTriangle({x, y}, {n.X2 + dx, n.Y2 + dy}, {n.X3 + dx, n.Y3 + dy}, color);
            else if (n.Type == Shape::Circle) DrawCircle(int(x), int(y), n.Width, color);
            else if (n.Type == Shape::CircleOutline) DrawCircleLines(int(x), int(y), n.Width, color);
            else if (n.Type == Shape::Ellipse) DrawEllipse(int(x), int(y), n.Width, n.Height, color);
            else if (n.Type == Shape::EllipseOutline) DrawEllipseLines(int(x), int(y), n.Width, n.Height, color);
            else if (n.Type == Shape::Arc) DrawRing({x, y}, n.Width - std::max(1.0f, n.Height) / 2, n.Width + std::max(1.0f, n.Height) / 2, n.Angle, n.X2, int(std::abs(n.X2 - n.Angle)), color);
            else if (n.Type == Shape::Spline) { const std::array<Vector2, 4> points{{{x, y}, {n.X2 + dx, n.Y2 + dy}, {n.X3 + dx, n.Y3 + dy}, {n.Width + dx, n.Height + dy}}}; DrawSplineBasis(points.data(), int(points.size()), 1, color); }
            else if (n.Type == Shape::RoundedRectangle) DrawRectangleRounded({x, y, n.Width, n.Height}, std::min(1.0f, 2 * n.Angle / std::max(1.0f, std::min(n.Width, n.Height))), 90, color);
            else if (n.Type == Shape::RoundedOutline) DrawRectangleRoundedLinesEx({x, y, n.Width, n.Height}, std::min(1.0f, 2 * n.Angle / std::max(1.0f, std::min(n.Width, n.Height))), 90, 1, color);
            else if (n.Type == Shape::Flash) {
                const std::array<Vector2, 4> outside{{{0,0}, {0,n.Height}, {n.Width,n.Height}, {n.Width,0}}}, inside{{{n.Width*.25f,n.Height*.25f}, {n.Width*.25f,n.Height*.75f}, {n.Width*.75f,n.Height*.75f}, {n.Width*.75f,n.Height*.25f}}};
                rlSetTexture(0); rlBegin(RL_QUADS);
                for (size_t i = 0; i < 4; ++i) { size_t j = (i + 1) % 4; rlColor4ub(n.Color, 0, 0, 50); rlVertex2f(inside[i].x, inside[i].y); rlVertex2f(inside[j].x, inside[j].y); rlColor4ub(n.Color, 0, 0, 255); rlVertex2f(outside[j].x, outside[j].y); rlVertex2f(outside[i].x, outside[i].y); } rlEnd();
            }
        }
    }
    rlDrawRenderBatchActive(); glDisable(GL_SCISSOR_TEST); rlSetBlendMode(RL_BLEND_ALPHA); shader.End(); impl.Target->End(); ++impl.RenderCount; return impl.Target->GetColorTexture().id;
}
bool MultiplayerWorld::CanvasSprite(BITMAP* bitmap, Rectangle source, Rectangle dest, Vector2 pivot, float angle, RLColor tint, BITMAP* target) {
    if (!canvasCollector || (target && canvasCollector->m_Impl->GUI != target)) return false;
    auto& impl = *canvasCollector->m_Impl; Node n; n.Asset = impl.Asset(bitmap); n.X = dest.x; n.Y = dest.y; n.Width = std::abs(dest.width); n.Height = std::abs(dest.height); n.PivotX = pivot.x; n.PivotY = pivot.y; n.Angle = angle;
    n.SourceX = source.x; n.SourceY = source.y; n.SourceWidth = std::abs(source.width); n.SourceHeight = std::abs(source.height); n.Flags = Masked | (source.width < 0 || dest.width < 0 ? FlipX : 0) | (source.height < 0 || dest.height < 0 ? FlipY : 0); n.Alpha = tint.a; impl.AppendCanvas(n); return true;
}
bool MultiplayerWorld::Primitive(const GraphicalPrimitive& primitive, const Vector& offset) {
    if (!canvasCollector) return false;
    using P = GraphicalPrimitive::PrimitiveType;
    auto& impl = *canvasCollector->m_Impl; Node n; n.X = primitive.m_StartPos.GetX() + offset.GetX(); n.Y = primitive.m_StartPos.GetY() + offset.GetY(); n.X2 = primitive.m_EndPos.GetX() + offset.GetX(); n.Y2 = primitive.m_EndPos.GetY() + offset.GetY(); n.Color = primitive.m_Color;
    n.BlendMode = uint8_t(primitive.m_BlendMode);
    if (n.BlendMode) { const auto& blend = primitive.m_ColorChannelBlendAmounts; n.TintR = uint8_t(std::clamp(blend[0], 0, 100) * 255 / 100); n.TintG = uint8_t(std::clamp(blend[1], 0, 100) * 255 / 100); n.TintB = uint8_t(std::clamp(blend[2], 0, 100) * 255 / 100); n.Alpha = uint8_t(std::clamp(blend[3], 0, 100) * 255 / 100); }
    if (primitive.GetPrimitiveType() == P::Text || primitive.GetPrimitiveType() == P::Bitmap) { impl.CurrentStyle = n; impl.PrimitiveStyle = true; return false; }
    switch (primitive.GetPrimitiveType()) {
        case P::Line: n.Type = Shape::Line; n.Width = static_cast<const LinePrimitive&>(primitive).m_Thickness; impl.AppendCanvas(n); return true;
        case P::BoxFill: n.Type = Shape::Rectangle; n.Width = n.X2 - n.X; n.Height = n.Y2 - n.Y; impl.AppendCanvas(n); return true;
        case P::Box: {
            n.Type = Shape::Line; Node edge = n; edge.Y2 = n.Y; impl.AppendCanvas(edge); edge = n; edge.X2 = n.X; impl.AppendCanvas(edge); edge = n; edge.X = n.X2; impl.AppendCanvas(edge); edge = n; edge.Y = n.Y2; impl.AppendCanvas(edge); return true;
        }
        case P::Circle: n.Type = Shape::CircleOutline; n.Width = static_cast<const CirclePrimitive&>(primitive).m_Radius; break;
        case P::CircleFill: n.Type = Shape::Circle; n.Width = static_cast<const CircleFillPrimitive&>(primitive).m_Radius; break;
        case P::Ellipse: { const auto& p = static_cast<const EllipsePrimitive&>(primitive); n.Type = Shape::EllipseOutline; n.Width = p.m_HorizRadius; n.Height = p.m_VertRadius; break; }
        case P::EllipseFill: { const auto& p = static_cast<const EllipseFillPrimitive&>(primitive); n.Type = Shape::Ellipse; n.Width = p.m_HorizRadius; n.Height = p.m_VertRadius; break; }
        case P::Arc: { const auto& p = static_cast<const ArcPrimitive&>(primitive); n.Type = Shape::Arc; n.Width = p.m_Radius; n.Height = p.m_Thickness; n.Angle = p.m_StartAngle; n.X2 = p.m_EndAngle; break; }
        case P::Spline: { const auto& p = static_cast<const SplinePrimitive&>(primitive); n.Type = Shape::Spline; n.Width = n.X2; n.Height = n.Y2; n.X2 = p.m_GuidePointAPos.GetX() + offset.GetX(); n.Y2 = p.m_GuidePointAPos.GetY() + offset.GetY(); n.X3 = p.m_GuidePointBPos.GetX() + offset.GetX(); n.Y3 = p.m_GuidePointBPos.GetY() + offset.GetY(); break; }
        case P::RoundedBox: n.Type = Shape::RoundedOutline; n.Width = std::abs(n.X2 - n.X); n.Height = std::abs(n.Y2 - n.Y); n.X = std::min(n.X, n.X2); n.Y = std::min(n.Y, n.Y2); n.Angle = static_cast<const RoundedBoxPrimitive&>(primitive).m_CornerRadius; break;
        case P::RoundedBoxFill: n.Type = Shape::RoundedRectangle; n.Width = std::abs(n.X2 - n.X); n.Height = std::abs(n.Y2 - n.Y); n.X = std::min(n.X, n.X2); n.Y = std::min(n.Y, n.Y2); n.Angle = static_cast<const RoundedBoxFillPrimitive&>(primitive).m_CornerRadius; break;
        case P::Triangle:
        case P::TriangleFill: {
            Vector a, b, c;
            if (primitive.GetPrimitiveType() == P::Triangle) { const auto& p = static_cast<const TrianglePrimitive&>(primitive); a = p.m_PointAPos + offset; b = p.m_PointBPos + offset; c = p.m_PointCPos + offset; }
            else { const auto& p = static_cast<const TriangleFillPrimitive&>(primitive); a = p.m_PointAPos + offset; b = p.m_PointBPos + offset; c = p.m_PointCPos + offset; }
            n.X = a.GetX(); n.Y = a.GetY(); n.X2 = b.GetX(); n.Y2 = b.GetY(); n.X3 = c.GetX(); n.Y3 = c.GetY();
            if (primitive.GetPrimitiveType() == P::TriangleFill) { n.Type = Shape::Triangle; break; }
            n.Type = Shape::Line; impl.AppendCanvas(n); n.X = b.GetX(); n.Y = b.GetY(); n.X2 = c.GetX(); n.Y2 = c.GetY(); impl.AppendCanvas(n); n.X = c.GetX(); n.Y = c.GetY(); n.X2 = a.GetX(); n.Y2 = a.GetY(); impl.AppendCanvas(n); return true;
        }
        case P::Polygon: {
            const auto& p = static_cast<const PolygonPrimitive&>(primitive); n.Type = Shape::Line;
            for (size_t i = 0; i < p.m_Vertices.size(); ++i) { const Vector a = p.m_StartPos + *p.m_Vertices[i], b = p.m_StartPos + *p.m_Vertices[(i + 1) % p.m_Vertices.size()]; n.X = a.GetX(); n.Y = a.GetY(); n.X2 = b.GetX(); n.Y2 = b.GetY(); impl.AppendCanvas(n); } return true;
        }
        case P::PolygonFill: {
            const auto& p = static_cast<const PolygonFillPrimitive&>(primitive); n.Type = Shape::Triangle;
            for (size_t i = 0; i + 2 < p.m_Vertices.size(); ++i) { Vector a = p.m_StartPos + offset + *p.m_Vertices[i], b = p.m_StartPos + offset + *p.m_Vertices[i + 1], c = p.m_StartPos + offset + *p.m_Vertices[i + 2]; if (i & 1) std::swap(a, b); n.X = a.GetX(); n.Y = a.GetY(); n.X2 = b.GetX(); n.Y2 = b.GetY(); n.X3 = c.GetX(); n.Y3 = c.GetY(); impl.AppendCanvas(n); } return true;
        }
        default: return false;
    }
    impl.AppendCanvas(n); return true;
}
void MultiplayerWorld::EndPrimitive() { if (canvasCollector) canvasCollector->m_Impl->PrimitiveStyle = false; }
