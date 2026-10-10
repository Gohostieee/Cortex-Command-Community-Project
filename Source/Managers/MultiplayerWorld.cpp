#include "MultiplayerWorld.h"
#include "MovableObject.h"
#include "MOSRotating.h"
#include "SceneMan.h"
#include "Scene.h"
#include "SLTerrain.h"
#include "SLBackground.h"
#include "ActivityMan.h"
#include "Activity.h"
#include "GameActivity.h"
#include "SceneEditorGUI.h"
#include "Actor.h"
#include "UInputMan.h"
#include "FrameMan.h"
#include "CameraMan.h"
#include "PostProcessMan.h"
#include "PresetMan.h"
#include "GLResourceMan.h"
#include "RenderTarget.h"
#include "Shader.h"
#include "GraphicalPrimitive.h"
#include "Draw.h"
#include "PieMenu.h"
#include "allegro.h"
#include "allegro/internal/aintern.h"
#include <unordered_map>
#include <chrono>
#include <atomic>
#include <mutex>
#include <ostream>

using namespace RTE;
using namespace RTE::MP::World;
namespace {
thread_local MultiplayerWorld* objectCollector = nullptr;
thread_local MultiplayerWorld* canvasCollector = nullptr;
std::atomic<MultiplayerWorld*> trailCollector = nullptr;
// Microseconds spent per composition stage since the profiler last read them.
std::array<uint64_t, 6> s_ComposeTimes{};
// Write stamps per 64-pixel tile of the scene bitmaps the host replicates.
// Registration happens only during capture on the main thread; writes may
// come from threaded scripts, so stamps are atomic and the map is read-only.
struct TrackedBitmap {
    int Width = 0, Height = 0, Columns = 0, Rows = 0;
    std::unique_ptr<std::atomic<uint64_t>[]> Stamps;
    size_t SweepStart = 0;
};
std::unordered_map<BITMAP*, TrackedBitmap> s_Tracked;
std::atomic<uint64_t> s_WriteClock{1};
std::atomic<bool> s_Tracking{false};
// Tiles re-verified per tracked bitmap per capture pass regardless of stamps,
// so a write site that does not report still reaches guests within seconds.
constexpr size_t SweepTiles = 32;
uint64_t WorldNow() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
constexpr size_t ResourceLimit = 128 * 1024 * 1024;
}
struct MultiplayerWorld::Impl {
    MultiplayerWorld* Owner = nullptr;
    std::vector<Node> Objects, Canvas;
    std::vector<uint64_t> ObjectRoots; // Root object node ID of each entry in Objects.
    std::deque<Node> Trails;
    std::mutex TrailMutex;
    uint64_t TrailSequence = 0, TrailTime = 0, TrailDuration = 17;
    size_t TrailPixels = 0;
    struct TrailPixel { uint64_t Key = 0; Node* Path = nullptr; uint32_t Index = 0, Generation = 0; };
    static constexpr size_t TrailSlots = 524288;
    std::unique_ptr<TrailPixel[]> CurrentTrailPixels;
    struct TrailBucket { uint64_t Key = 0; Node* Path = nullptr; uint32_t Generation = 0; };
    static constexpr size_t TrailBucketSlots = 16384;
    std::unique_ptr<TrailBucket[]> CurrentTrailBuckets;
    uint32_t TrailGeneration = 0;
    uint64_t HUDParent = 0;
    Vector HUDAnchor;
    Interaction HUDControl = Interaction::None;
    Vector PointerPosition;
    MP::Input LocalInput;
    LocalCamera CameraPrediction;
    LocalPointer Pointer;
    RetainedLayers SceneLayers;
    bool LocalInputEnabled = false;
    std::unordered_map<uint64_t, Vector> ObjectAnchors;
    std::shared_ptr<std::unordered_set<uint64_t>> CriticalNodes = std::make_shared<std::unordered_set<uint64_t>>();
    bool PrimitiveStyle = false;
    Node CurrentStyle;
    std::unordered_map<uint64_t, uint8_t> Ordinals;
    std::unordered_map<uint64_t, Resource> Resources;
    std::unordered_set<uint64_t> EmptyMaskedAssets;
    std::unordered_set<uint64_t> SceneAssets;
    std::unordered_map<uint64_t, Texture2D> Textures;
    std::unordered_map<BITMAP*, uint64_t> StaticAssets;
    // Clock is the write clock when the region's pixels were last verified.
    struct Region { uint64_t Asset = 0, Clock = 0; };
    std::unordered_map<BITMAP*, std::unordered_map<uint64_t, Region>> BitmapRegions;
    size_t BitmapRegionEntries = 0;
    std::unordered_map<uint64_t, uint64_t> LastUsed;
    size_t ResourceBytes = 0;
    Timeline States;
    std::unique_ptr<RenderTarget> Target;
    std::unique_ptr<Shader> SceneShader;
    uint64_t RenderCount = 0, UpdateCount = 0;
    uint64_t IntermediateCount = 0;
    Snapshot LastSample;
    std::unordered_map<uint64_t, Node> LastVisuals;
    // Recent revisions of each visible scene tile. A tile that changes on most
    // updates (fog around a moving actor) is rarely present in its newest
    // revision; the newest one that has arrived is shown instead.
    std::unordered_map<uint64_t, std::deque<Node>> TileRevisions;
    // Scene tiles in the last frame shown from an older revision, and covered black.
    unsigned StaleTiles = 0, BlackTiles = 0, BlackFogTiles = 0;
    BITMAP* GUI = nullptr;
    BITMAP* CanvasTarget = nullptr;
    GFX_VTABLE* OriginalVTable = nullptr;
    GFX_VTABLE CanvasVTable{};
    Vector Camera;

    bool Store(Resource resource) {
        const auto id = resource.ID; const uint64_t now = WorldNow(); LastUsed[id] = now;
        if (Resources.contains(id)) return true;
        if (ResourceBytes + resource.Pixels.size() > ResourceLimit) {
            auto pinned = States.Resources(); const auto retained = SceneLayers.Resources(); pinned.insert(retained.begin(), retained.end()); pinned.insert(SceneAssets.begin(), SceneAssets.end()); for (const auto& [id, visual] : LastVisuals) if (visual.Asset) pinned.insert(visual.Asset);
            std::vector<std::pair<uint64_t, uint64_t>> candidates;
            for (const auto& [key, used] : LastUsed) if (!pinned.contains(key)) candidates.emplace_back(used, key);
            std::sort(candidates.begin(), candidates.end());
            for (const auto& [used, key] : candidates) {
                auto found = Resources.find(key); if (found == Resources.end()) continue;
                ResourceBytes -= found->second.Pixels.size(); Resources.erase(found); EmptyMaskedAssets.erase(key); LastUsed.erase(key);
                if (auto texture = Textures.find(key); texture != Textures.end()) { rlUnloadTexture(texture->second.id); Textures.erase(texture); }
                if (ResourceBytes + resource.Pixels.size() <= ResourceLimit * 9 / 10) break;
            }
        }
        if (ResourceBytes + resource.Pixels.size() > ResourceLimit) { LastUsed.erase(id); return false; }
        if (resource.Depth == 8 && std::all_of(resource.Pixels.begin(), resource.Pixels.end(), [](uint8_t color) { return color == g_MaskColor; })) EmptyMaskedAssets.insert(id);
        ResourceBytes += resource.Pixels.size(); Resources.emplace(id, std::move(resource)); return true;
    }

    // A tracked scene tile whose last write stamp is no newer than its last
    // verification needs no pixel comparison. Untracked bitmaps (including
    // temporary GUI bitmaps, whose freed address can hold new content) pass
    // no stamp and are always compared.
    uint64_t Asset(BITMAP* bitmap, int sx = 0, int sy = 0, int width = -1, int height = -1, bool immutable = false, uint64_t stamp = 0) {
        if (!bitmap || (bitmap_color_depth(bitmap) != 8 && bitmap_color_depth(bitmap) != 32)) return 0;
        width = width < 0 ? bitmap->w : width; height = height < 0 ? bitmap->h : height;
        sx = std::clamp(sx, 0, bitmap->w); sy = std::clamp(sy, 0, bitmap->h);
        width = std::clamp(width, 0, bitmap->w - sx); height = std::clamp(height, 0, bitmap->h - sy);
        if (!width || !height || width > 4096 || height > 4096 || uint64_t(width) * height * (bitmap_color_depth(bitmap) / 8) > MaxPayload - 256) return 0;
        const uint64_t region = (uint64_t(uint16_t(sx)) << 48) | (uint64_t(uint16_t(sy)) << 32) | (uint64_t(uint16_t(width)) << 16) | uint16_t(height);
        if (BitmapRegionEntries >= 65536) { BitmapRegions.clear(); BitmapRegionEntries = 0; }
        auto& regions = BitmapRegions[bitmap];
        const uint64_t now = WorldNow(), verified = s_WriteClock.load();
        if (auto cached = regions.find(region); cached != regions.end()) if (auto old = Resources.find(cached->second.Asset); old != Resources.end() && old->second.Depth == bitmap_color_depth(bitmap)) {
            // StaticSceneLayer's native contract uploads pixels only once.
            // Its immutable backdrops need no repeated full-region comparison.
            if (immutable || (stamp && cached->second.Clock >= stamp)) { LastUsed[cached->second.Asset] = now; return cached->second.Asset; }
            const auto& previous = old->second; const int stride = width * (previous.Depth / 8);
            bool unchanged = true;
            for (int y = 0; unchanged && y < height; ++y) unchanged = std::memcmp(previous.Pixels.data() + size_t(y) * stride, bitmap->line[sy + y] + sx * (previous.Depth / 8), stride) == 0;
            if (unchanged) { LastUsed[cached->second.Asset] = now; cached->second.Clock = verified; return cached->second.Asset; }
        }
        Resource resource; resource.Width = uint16_t(width); resource.Height = uint16_t(height); resource.Depth = uint8_t(bitmap_color_depth(bitmap));
        const int stride = width * (resource.Depth / 8); resource.Pixels.resize(size_t(stride) * height);
        for (int y = 0; y < height; ++y) {
            std::memcpy(resource.Pixels.data() + size_t(y) * stride, bitmap->line[sy + y] + sx * (resource.Depth / 8), stride);
        }
        resource.ID = ResourceHash(resource); const uint64_t id = resource.ID;
        if (!Store(std::move(resource))) return 0;
        if (!regions.contains(region)) ++BitmapRegionEntries;
        regions[region] = {id, verified}; return id;
    }
    // Pristine terrain tiles built from local game data, for the scene named.
    // Kept between matches and reconnects so the scene is built once.
    std::string LocalScene;
    std::unordered_map<uint64_t, Resource> LocalTiles;
    void LocalTile(BITMAP* bitmap, int sx, int sy, int width, int height) {
        if (bitmap_color_depth(bitmap) != 8 || width <= 0 || height <= 0) return;
        Resource resource; resource.Width = uint16_t(width); resource.Height = uint16_t(height); resource.Depth = 8; resource.Pixels.resize(size_t(width) * height);
        for (int y = 0; y < height; ++y) std::memcpy(resource.Pixels.data() + size_t(y) * width, bitmap->line[sy + y] + sx, width);
        resource.ID = ResourceHash(resource); const uint64_t id = resource.ID; LocalTiles.try_emplace(id, std::move(resource));
    }
    uint64_t StaticAsset(BITMAP* bitmap) {
        if (auto found = StaticAssets.find(bitmap); found != StaticAssets.end() && Resources.contains(found->second)) { LastUsed[found->second] = WorldNow(); return found->second; }
        const auto id = Asset(bitmap); if (id) StaticAssets[bitmap] = id; return id;
    }
    void AppendCanvas(Node node) {
        node.ID = (uint64_t(1) << 63) | uint64_t(Canvas.size() + 1); node.Flags |= ScreenSpace;
        node.Control = HUDControl;
        BITMAP* target = CanvasTarget ? CanvasTarget : GUI;
        const float dx = target && GUI ? float(target->x_ofs - GUI->x_ofs) : 0;
        const float dy = target && GUI ? float(target->y_ofs - GUI->y_ofs) : 0;
        node.X += dx; node.Y += dy;
        if (node.Control == Interaction::Pointer) { node.X2 = PointerPosition.GetX() + dx; node.Y2 = PointerPosition.GetY() + dy; }
        if (node.Type == Shape::Line || node.Type == Shape::Triangle || node.Type == Shape::Spline) { node.X2 += dx; node.Y2 += dy; node.X3 += dx; node.Y3 += dy; }
        if (node.Type == Shape::Spline) { node.Width += dx; node.Height += dy; }
		if (PrimitiveStyle) { node.BlendMode = CurrentStyle.BlendMode; node.TintR = CurrentStyle.TintR; node.TintG = CurrentStyle.TintG; node.TintB = CurrentStyle.TintB; node.Alpha = uint8_t(unsigned(node.Alpha) * CurrentStyle.Alpha / 255); }
		else if (_drawing_mode == DRAW_MODE_TRANS) node.Alpha = uint8_t(unsigned(node.Alpha) * g_FrameMan.GetCurrentAlpha() / 255);
		if (HUDParent) {
			node.Parent = HUDParent; node.X -= HUDAnchor.GetX(); node.Y -= HUDAnchor.GetY();
			if (node.Type == Shape::Line || node.Type == Shape::Triangle || node.Type == Shape::Spline) { node.X2 -= HUDAnchor.GetX(); node.Y2 -= HUDAnchor.GetY(); node.X3 -= HUDAnchor.GetX(); node.Y3 -= HUDAnchor.GetY(); }
			if (node.Type == Shape::Spline) { node.Width -= HUDAnchor.GetX(); node.Height -= HUDAnchor.GetY(); }
		}
        if (target && GUI) {
            const int left = std::max(GUI->clip ? GUI->cl : 0, int(dx) + (target->clip ? target->cl : 0));
            const int top = std::max(GUI->clip ? GUI->ct : 0, int(dy) + (target->clip ? target->ct : 0));
            const int right = std::min(GUI->clip ? GUI->cr : GUI->w, int(dx) + (target->clip ? target->cr : target->w));
            const int bottom = std::min(GUI->clip ? GUI->cb : GUI->h, int(dy) + (target->clip ? target->cb : target->h));
            node.ClipX = uint16_t(std::max(0, left)); node.ClipY = uint16_t(std::max(0, top));
            node.ClipWidth = uint16_t(std::max(0, right - left)); node.ClipHeight = uint16_t(std::max(0, bottom - top));
        }
        if (Canvas.size() < MaxNodes && Valid(node)) Canvas.push_back(node);
    }
    void AppendObject(const MovableObject& owner, Node node) {
        const uint64_t id = uint64_t(owner.GetUniqueID());
        node.ID = (id << 8) | Ordinals[id]++;
        const auto* root = owner.GetRootParent();
        if (root->IsActor() || root->IsDevice()) CriticalNodes->insert(node.ID);
        if (!(node.ID & 255)) ObjectAnchors[node.ID] = Vector(node.X, node.Y);
        if (const auto* parent = owner.GetParent()) node.Parent = uint64_t(parent->GetUniqueID()) << 8;
        if (Objects.size() < MaxNodes && Valid(node)) { Objects.push_back(node); ObjectRoots.push_back(uint64_t(root->GetUniqueID()) << 8); }
    }
    static Impl* CanvasFor(BITMAP* bitmap) {
        if (!canvasCollector || !bitmap) return nullptr;
        auto& impl = *canvasCollector->m_Impl;
        if (impl.GUI != bitmap && (!impl.GUI || !(bitmap->id & BMP_ID_SUB) || !(bitmap->id & BMP_ID_MASK) || (bitmap->id & BMP_ID_MASK) != (impl.GUI->id & BMP_ID_MASK))) return nullptr;
        impl.CanvasTarget = bitmap; return &impl;
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
    static TrackedBitmap& Track(BITMAP* bitmap) {
        auto& tracked = s_Tracked[bitmap];
        if (tracked.Width != bitmap->w || tracked.Height != bitmap->h || !tracked.Stamps) {
            tracked.Width = bitmap->w; tracked.Height = bitmap->h; tracked.Columns = (bitmap->w + TileSize - 1) / TileSize; tracked.Rows = (bitmap->h + TileSize - 1) / TileSize; tracked.SweepStart = 0;
            tracked.Stamps = std::make_unique<std::atomic<uint64_t>[]>(size_t(tracked.Columns) * tracked.Rows);
            // A newly tracked bitmap is verified once in full.
            const uint64_t initial = s_WriteClock.fetch_add(1) + 1;
            for (size_t i = 0; i < size_t(tracked.Columns) * tracked.Rows; ++i) tracked.Stamps[i].store(initial, std::memory_order_relaxed);
        }
        return tracked;
    }
    template<class LayerType> void Layer(std::vector<Node>& output, LayerType* layer, uint16_t ordinal, const Vector& camera, int width, int height, bool whole = false) {
        if (!layer || !layer->GetBitmap()) return;
        BITMAP* bitmap = layer->GetBitmap(); const Vector scale = layer->GetScaleFactor(); Vector offset = layer->GetOffset();
        // Terrain colour and team fog report their writes; static backdrops never change.
        TrackedBitmap* tracked = nullptr;
        if constexpr (!std::is_base_of_v<StaticSceneLayer, LayerType>) tracked = &Track(bitmap);
        const size_t trackedTiles = tracked ? size_t(tracked->Columns) * tracked->Rows : 0;
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
        const int repeatX = !whole && layer->WrapsX() ? int(std::floor((offset.GetX() - margin) / spanX)) : 0;
        const int endX = !whole && layer->WrapsX() ? int(std::ceil((offset.GetX() + width + margin) / spanX)) : 1;
        const int repeatY = !whole && layer->WrapsY() ? int(std::floor((offset.GetY() - margin) / spanY)) : 0;
        const int endY = !whole && layer->WrapsY() ? int(std::ceil((offset.GetY() + height + margin) / spanY)) : 1;
        std::unordered_set<uint64_t> emitted;
        for (int rx = repeatX; rx < endX; ++rx) for (int ry = repeatY; ry < endY; ++ry) {
          const int firstX = whole ? 0 : std::clamp(int(std::floor((offset.GetX() - margin - rx * spanX) / (TileSize * scale.GetX()))), 0, columns);
          const int lastX = whole ? columns : std::clamp(int(std::ceil((offset.GetX() + width + margin - rx * spanX) / (TileSize * scale.GetX()))), 0, columns);
          const int firstY = whole ? 0 : std::clamp(int(std::floor((offset.GetY() - margin - ry * spanY) / (TileSize * scale.GetY()))), 0, rows);
          const int lastY = whole ? rows : std::clamp(int(std::ceil((offset.GetY() + height + margin - ry * spanY) / (TileSize * scale.GetY()))), 0, rows);
          for (int y = firstY; y < lastY; ++y) for (int x = firstX; x < lastX; ++x) {
            int sx = x * TileSize, sy = y * TileSize, w = std::min<int>(TileSize, bitmap->w - sx), h = std::min<int>(TileSize, bitmap->h - sy);
            Node n; n.ID = (uint64_t(1) << 62) | (uint64_t(ordinal) << 48) | (uint64_t(y) << 24) | uint16_t(x);
            if (!emitted.insert(n.ID).second) continue;
            uint64_t stamp = 0;
            if (tracked) {
                const size_t index = size_t(y) * tracked->Columns + x;
                stamp = (index + trackedTiles - tracked->SweepStart) % trackedTiles < SweepTiles ? UINT64_MAX : tracked->Stamps[index].load(std::memory_order_relaxed);
            }
            n.Asset = Asset(bitmap, sx, sy, w, h, std::is_base_of_v<StaticSceneLayer, LayerType>, stamp);
            // Empty tiles are explicit updates so a destroyed terrain tile
            // cannot remain in the guest's retained map.
            n.SourceWidth = float(w); n.SourceHeight = float(h); n.Width = w * scale.GetX(); n.Height = h * scale.GetY();
            n.Flags = ScreenSpace | MP::World::Layer | (layer->GetDrawMasked() ? Masked : 0);
            n.X3 = n.Y3 = 1;
            if constexpr (std::is_same_v<LayerType, SLBackground>) { n.X3 = layer->IsAutoScrolling() ? 0 : layer->GetScrollRatio().GetX(); n.Y3 = layer->IsAutoScrolling() ? 0 : layer->GetScrollRatio().GetY(); }
            n.X2 = layer->WrapsX() ? spanX : 0; n.Y2 = layer->WrapsY() ? spanY : 0;
            n.X = boxX + sx * scale.GetX() - offset.GetX(); n.Y = boxY + sy * scale.GetY() - offset.GetY();
            n.ClipX = boxX; n.ClipY = boxY; n.ClipWidth = width - boxX * 2; n.ClipHeight = height - boxY * 2;
            if (Valid(n)) output.push_back(n);
          }
        }
        if constexpr (std::is_same_v<LayerType, SLBackground>) {
            auto fill = [&](float x, float y, float w, float h, int color, uint16_t edge) {
                if (w <= 0 || h <= 0 || color == g_MaskColor) return;
                Node n; n.ID = (uint64_t(1) << 61) | (uint64_t(ordinal) << 32) | edge; n.Type = Shape::Rectangle; n.Flags = ScreenSpace | MP::World::Layer; n.X = x; n.Y = y; n.Width = w; n.Height = h; n.Color = uint8_t(color);
                n.X3 = layer->IsAutoScrolling() ? 0 : layer->GetScrollRatio().GetX(); n.Y3 = layer->IsAutoScrolling() ? 0 : layer->GetScrollRatio().GetY(); output.push_back(n);
            };
            if (!layer->WrapsX()) { fill(-32768 - offset.GetX(), -32768 - offset.GetY(), 32768, 65536, getpixel(bitmap, 0, bitmap->h / 2), 1); fill(spanX - offset.GetX(), -32768 - offset.GetY(), 32768, 65536, getpixel(bitmap, bitmap->w - 1, bitmap->h / 2), 2); }
            if (!layer->WrapsY()) { fill(-32768 - offset.GetX(), -32768 - offset.GetY(), 65536, 32768, getpixel(bitmap, bitmap->w / 2, 0), 3); fill(-32768 - offset.GetX(), spanY - offset.GetY(), 65536, 32768, getpixel(bitmap, bitmap->w / 2, bitmap->h - 1), 4); }
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
void MultiplayerWorld::MarkChanged(BITMAP* bitmap, int x, int y, int width, int height) {
    if (!s_Tracking.load(std::memory_order_relaxed) || !bitmap || width <= 0 || height <= 0) return;
    const auto found = s_Tracked.find(bitmap); if (found == s_Tracked.end()) return;
    auto& tracked = found->second; if (tracked.Width != bitmap->w || tracked.Height != bitmap->h) return;
    const uint64_t stamp = s_WriteClock.fetch_add(1, std::memory_order_relaxed) + 1;
    // Wrapped writes may start outside the bitmap; mark each wrapped piece.
    auto spans = [](int start, int length, int extent) {
        std::array<std::pair<int, int>, 2> result{}; int count = 0;
        if (length >= extent) { result[count++] = {0, extent - 1}; return std::pair{result, count}; }
        start = ((start % extent) + extent) % extent;
        if (start + length <= extent) result[count++] = {start, start + length - 1};
        else { result[count++] = {start, extent - 1}; result[count++] = {0, start + length - extent - 1}; }
        return std::pair{result, count};
    };
    const auto [columns, columnCount] = spans(x, width, tracked.Width);
    const auto [rows, rowCount] = spans(y, height, tracked.Height);
    for (int r = 0; r < rowCount; ++r) for (int c = 0; c < columnCount; ++c)
        for (int ty = rows[r].first / TileSize; ty <= rows[r].second / TileSize; ++ty) for (int tx = columns[c].first / TileSize; tx <= columns[c].second / TileSize; ++tx)
            tracked.Stamps[size_t(ty) * tracked.Columns + tx].store(stamp, std::memory_order_relaxed);
}
void MultiplayerWorld::Reset() {
    s_Tracking = false; s_Tracked.clear();
    EndObjects(); if (canvasCollector == this) { if (m_Impl->GUI) m_Impl->GUI->vtable = m_Impl->OriginalVTable; canvasCollector = nullptr; }
    if (trailCollector.load() == this) trailCollector.store(nullptr);
    for (const auto& [id, texture] : m_Impl->Textures) rlUnloadTexture(texture.id);
    m_Impl = std::make_unique<Impl>(); m_Impl->Owner = this;
}
void MultiplayerWorld::ResetPresentation() {
    auto& impl = *m_Impl;
    impl.States.Reset(); impl.LastSample = {}; impl.LastVisuals.clear(); impl.TileRevisions.clear(); impl.SceneAssets.clear();
    impl.CameraPrediction.Reset();
    impl.Pointer.Reset(); impl.SceneLayers.Reset();
    impl.RenderCount = impl.UpdateCount = impl.IntermediateCount = 0;
    impl.Target.reset();
}
void MultiplayerWorld::BeginObjects() {
    // Each capture pass moves the verification sweep across every tracked bitmap.
    s_Tracking = true;
    for (auto& [bitmap, tracked] : s_Tracked) if (const size_t tiles = size_t(tracked.Columns) * tracked.Rows) tracked.SweepStart = (tracked.SweepStart + SweepTiles) % tiles;
    m_Impl->Objects.clear(); m_Impl->ObjectRoots.clear(); m_Impl->Ordinals.clear(); m_Impl->ObjectAnchors.clear(); m_Impl->CriticalNodes = std::make_shared<std::unordered_set<uint64_t>>(); objectCollector = this; }
void MultiplayerWorld::BeginTrails() {
    auto& impl = *m_Impl; const uint64_t now = WorldNow(); std::lock_guard lock(impl.TrailMutex);
    if (!++impl.TrailGeneration) { impl.CurrentTrailPixels.reset(); impl.CurrentTrailBuckets.reset(); ++impl.TrailGeneration; }
    impl.TrailDuration = impl.TrailTime ? std::clamp<uint64_t>(now - impl.TrailTime, 8, 100) : 17; impl.TrailTime = now;
    while (!impl.Trails.empty() && impl.Trails.front().EndTime + 250 < now) { impl.TrailPixels -= impl.Trails.front().Pixels.size(); impl.Trails.pop_front(); } trailCollector.store(this);
}
void MultiplayerWorld::Trail(std::span<const std::pair<int, int>> positions, uint8_t color) {
    if (!color || positions.empty()) return;
    auto* collector = trailCollector.load(); if (!collector) return;
    auto& impl = *collector->m_Impl; std::lock_guard lock(impl.TrailMutex);
    if (!impl.CurrentTrailPixels) impl.CurrentTrailPixels = std::make_unique<Impl::TrailPixel[]>(Impl::TrailSlots);
    if (!impl.CurrentTrailBuckets) impl.CurrentTrailBuckets = std::make_unique<Impl::TrailBucket[]>(Impl::TrailBucketSlots);
    for (const auto& [x, y] : positions) {
        const uint64_t key = (uint64_t(uint32_t(x)) << 32) | uint32_t(y);
        const uint64_t mixed = (key ^ (key >> 17) ^ (key >> 29)) * 0x9e3779b185ebca87ULL;
        size_t slot = size_t(mixed >> 32) & (Impl::TrailSlots - 1);
        while (impl.CurrentTrailPixels[slot].Generation == impl.TrailGeneration && impl.CurrentTrailPixels[slot].Key != key) slot = (slot + 1) & (Impl::TrailSlots - 1);
        auto& pixel = impl.CurrentTrailPixels[slot];
        if (pixel.Generation == impl.TrailGeneration) {
            pixel.Path->PixelColors[pixel.Index] = color;
            continue;
        }
        if (impl.TrailPixels >= 262144) break;
        const int tileX = int((int64_t(x) - (x < 0 ? 255 : 0)) / 256), tileY = int((int64_t(y) - (y < 0 ? 255 : 0)) / 256);
        const uint64_t tileKey = (uint64_t(uint32_t(tileX)) << 32) | uint32_t(tileY);
        const uint64_t tileMixed = (tileKey ^ (tileKey >> 17) ^ (tileKey >> 29)) * 0x9e3779b185ebca87ULL;
        size_t bucketSlot = size_t(tileMixed >> 32) & (Impl::TrailBucketSlots - 1);
        while (impl.CurrentTrailBuckets[bucketSlot].Generation == impl.TrailGeneration && impl.CurrentTrailBuckets[bucketSlot].Key != tileKey) bucketSlot = (bucketSlot + 1) & (Impl::TrailBucketSlots - 1);
        auto& bucket = impl.CurrentTrailBuckets[bucketSlot];
        Node* path = bucket.Generation == impl.TrailGeneration ? bucket.Path : nullptr;
        if (!path || path->Pixels.size() >= 16384) {
            if (impl.Trails.size() >= 4096) break;
            Node n; n.ID = (uint64_t(1) << 59) | ++impl.TrailSequence; n.Type = Shape::PixelPath;
            n.X = float(tileX * 256); n.Y = float(tileY * 256); n.Width = n.Height = 256; n.Color = color; n.Flags = Discontinuous;
            n.StartTime = impl.TrailTime; n.EndTime = n.StartTime + impl.TrailDuration;
            impl.Trails.push_back(std::move(n)); path = &impl.Trails.back();
            bucket = {tileKey, path, impl.TrailGeneration};
        }
        pixel = {key, path, uint32_t(path->Pixels.size()), impl.TrailGeneration};
        path->Pixels.emplace_back(int16_t(x - int(path->X)), int16_t(y - int(path->Y)));
        path->PixelColors.push_back(color); ++impl.TrailPixels;
    }
}
bool MultiplayerWorld::Flash(int width, int height, uint8_t color) { if (!canvasCollector) return false; Node n; n.Type = Shape::Flash; n.Width = float(width); n.Height = float(height); n.Color = color; canvasCollector->m_Impl->AppendCanvas(n); return true; }
void MultiplayerWorld::BeginHUD(const MovableObject& owner) {
    if (!canvasCollector) return; auto& impl = *canvasCollector->m_Impl; impl.HUDParent = uint64_t(owner.GetUniqueID()) << 8;
    auto found = impl.ObjectAnchors.find(impl.HUDParent); const Vector anchor = found != impl.ObjectAnchors.end() ? found->second : owner.GetPos();
    impl.HUDAnchor = Vector(impl.GUI->w / 2 + Displacement(impl.Camera.GetX() + impl.GUI->w / 2, anchor.GetX(), g_SceneMan.GetSceneWidth(), g_SceneMan.SceneWrapsX()), impl.GUI->h / 2 + Displacement(impl.Camera.GetY() + impl.GUI->h / 2, anchor.GetY(), g_SceneMan.GetSceneHeight(), g_SceneMan.SceneWrapsY()));
}
void MultiplayerWorld::EndHUD() { if (canvasCollector) canvasCollector->m_Impl->HUDParent = 0; }
void MultiplayerWorld::BeginAim(const Actor& actor, int screen) {
    if (!canvasCollector) return;
    auto* activity = g_ActivityMan.GetActivity();
    const int player = activity ? activity->PlayerOfScreen(screen) : -1;
    if (player >= 0 && activity->GetControlledActor(player) == &actor) canvasCollector->m_Impl->HUDControl = Interaction::Aim;
}
void MultiplayerWorld::BeginRadialCursor() { if (canvasCollector) canvasCollector->m_Impl->HUDControl = Interaction::RadialCursor; }
void MultiplayerWorld::BeginRadialBackground() { if (canvasCollector) canvasCollector->m_Impl->HUDControl = Interaction::RadialBackground; }
void MultiplayerWorld::BeginPointer(float x, float y) { if (canvasCollector) { canvasCollector->m_Impl->HUDControl = Interaction::Pointer; canvasCollector->m_Impl->PointerPosition.SetXY(x, y); } }
void MultiplayerWorld::BeginWorldCursor() { if (canvasCollector) canvasCollector->m_Impl->HUDControl = Interaction::WorldCursor; }
void MultiplayerWorld::BeginWorldCursor(float x, float y) {
    if (!canvasCollector) return;
    // An invisible first node marks the exact cursor point for drawings, such
    // as the landing-zone marker, that do not start at the cursor.
    auto& impl = *canvasCollector->m_Impl; impl.HUDControl = Interaction::WorldCursor;
    Node anchor; anchor.Type = Shape::Rectangle; anchor.X = x; anchor.Y = y; anchor.Alpha = 0; impl.AppendCanvas(anchor);
}
void MultiplayerWorld::EndInteraction() { if (canvasCollector) canvasCollector->m_Impl->HUDControl = Interaction::None; }
void MultiplayerWorld::SetLocalInput(const MP::Input& input, bool enabled) { m_Impl->LocalInput = input; m_Impl->LocalInputEnabled = enabled; }
void MultiplayerWorld::ExportLocalView(MP::Input& input, bool enabled) const {
    m_Impl->CameraPrediction.Export(input, enabled); m_Impl->Pointer.Export(input, enabled && input.Device == DEVICE_MOUSE_KEYB);
    // The caller sends this input under the next sequence number.
    m_Impl->CameraPrediction.RecordSent(input.Sequence + 1);
}
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
    auto& impl = *m_Impl; impl.Canvas.clear(); impl.GUI = impl.CanvasTarget = gui; impl.Camera = camera; impl.OriginalVTable = gui->vtable; impl.CanvasVTable = *gui->vtable;
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
    auto* active = g_ActivityMan.GetActivity();
    if (active) {
        snapshot.Paused = active->IsPaused();
        snapshot.ViewMode = uint8_t(active->GetViewState(player));
        const int screen = active->ScreenOfPlayer(player);
        const Vector target = g_CameraMan.GetScrollTarget(screen) - Vector(snapshot.Width / 2, snapshot.Height / 2) - g_CameraMan.GetScreenOcclusion(screen) / 2;
        snapshot.CameraTargetX = target.GetX(); snapshot.CameraTargetY = target.GetY();
        if (active->GetViewState(player) == Activity::Observe) snapshot.MouseScale = 1.2f;
        else if (active->GetViewState(player) == Activity::ActorSelect || active->GetViewState(player) == Activity::AIGoToPoint || active->GetViewState(player) == Activity::LandingZoneSelect) snapshot.MouseScale = 1;
        if (active->GetActivityState() == Activity::Editing) if (auto* game = dynamic_cast<GameActivity*>(active); game && game->GetEditorGUI(player)) {
            const auto mode = game->GetEditorGUI(player)->GetEditorGUIMode();
            snapshot.ViewMode = uint8_t(10 + mode); snapshot.ScrollSpeed = .3f;
            snapshot.MouseScale = mode == SceneEditorGUI::PICKINGOBJECT || mode == SceneEditorGUI::DONEEDITING || mode == SceneEditorGUI::INACTIVE ? 0 : mode >= SceneEditorGUI::MOVINGOBJECT ? .5f : 1;
        }
    }
    if (auto* actor = active ? active->GetControlledActor(player) : nullptr) {
        snapshot.ControlledActor = uint64_t(actor->GetUniqueID()) << 8;
        const Vector aim = g_UInputMan.AnalogAimValues(player);
        snapshot.AimX = aim.GetX(); snapshot.AimY = aim.GetY();
        if (!actor->GetController()->IsState(PIE_MENU_ACTIVE)) { const Vector look = actor->GetViewPoint() - actor->GetPos(); snapshot.LookX = look.GetX(); snapshot.LookY = look.GetY(); }
    }
    auto* scene = g_SceneMan.GetScene(); if (!scene) return snapshot;
    auto lap = [stage = std::chrono::steady_clock::now()](uint64_t& total) mutable { const auto now = std::chrono::steady_clock::now(); total += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - stage).count()); stage = now; };
    auto& times = s_ComposeTimes;
    uint16_t layerID = 1;
    for (auto it = scene->GetBackLayers().rbegin(); it != scene->GetBackLayers().rend(); ++it) impl.Layer(snapshot.Nodes, *it, layerID++, impl.Camera, snapshot.Width, snapshot.Height);
    impl.Layer(snapshot.Nodes, scene->GetTerrain()->GetBGSceneLayer(), 100, impl.Camera, snapshot.Width, snapshot.Height);
    lap(times[0]);
    { std::lock_guard lock(impl.TrailMutex);
      for (const auto& path : impl.Trails) {
        const float dx = Displacement(impl.Camera.GetX() + snapshot.Width / 2, path.X + path.Width / 2, snapshot.SceneWidth, snapshot.Wrap & 1);
        const float dy = Displacement(impl.Camera.GetY() + snapshot.Height / 2, path.Y + path.Height / 2, snapshot.SceneHeight, snapshot.Wrap & 2);
        if (std::abs(dx) >= snapshot.Width / 2 + 128 + path.Width / 2 || std::abs(dy) >= snapshot.Height / 2 + 128 + path.Height / 2) continue;
        if (std::abs(dx) + path.Width / 2 < snapshot.Width / 2 + 128 && std::abs(dy) + path.Height / 2 < snapshot.Height / 2 + 128) { snapshot.Nodes.push_back(path); continue; }
        Node visible; visible.ID = path.ID; visible.Type = path.Type; visible.Flags = path.Flags; visible.Color = path.Color;
        visible.X = path.X; visible.Y = path.Y; visible.Width = path.Width; visible.Height = path.Height; visible.StartTime = path.StartTime; visible.EndTime = path.EndTime;
        visible.Pixels.reserve(path.Pixels.size()); visible.PixelColors.reserve(path.PixelColors.size());
        for (size_t i = 0; i < path.Pixels.size(); ++i) { const auto [x, y] = path.Pixels[i]; if (std::abs(Displacement(impl.Camera.GetX() + snapshot.Width / 2, path.X + x, snapshot.SceneWidth, snapshot.Wrap & 1)) < snapshot.Width / 2 + 128 && std::abs(Displacement(impl.Camera.GetY() + snapshot.Height / 2, path.Y + y, snapshot.SceneHeight, snapshot.Wrap & 2)) < snapshot.Height / 2 + 128) { visible.Pixels.emplace_back(x, y); visible.PixelColors.push_back(path.PixelColors[i]); } }
        if (!visible.Pixels.empty()) snapshot.Nodes.push_back(std::move(visible));
      }
    }
    lap(times[1]);
    // Relevance: each guest receives the objects around its own view, with a
    // margin for motion and local camera travel, plus its controlled actor
    // wherever it is. Whole objects stay together by their root's position.
    // The team's complete fog layer below still obscures unexplored regions.
    {
        const float marginX = std::max(256.0f, snapshot.Width * .5f), marginY = std::max(256.0f, snapshot.Height * .5f);
        const float centerX = impl.Camera.GetX() + snapshot.Width / 2.0f, centerY = impl.Camera.GetY() + snapshot.Height / 2.0f;
        std::unordered_map<uint64_t, bool> relevant;
        for (size_t i = 0; i < impl.Objects.size(); ++i) {
            const Node& node = impl.Objects[i]; const uint64_t root = impl.ObjectRoots[i];
            auto [entry, added] = relevant.try_emplace(root, false);
            if (added) {
                const auto anchor = impl.ObjectAnchors.find(root); const float x = anchor != impl.ObjectAnchors.end() ? anchor->second.GetX() : node.X, y = anchor != impl.ObjectAnchors.end() ? anchor->second.GetY() : node.Y;
                entry->second = root == snapshot.ControlledActor ||
                    (std::abs(Displacement(centerX, x, snapshot.SceneWidth, snapshot.Wrap & 1)) <= snapshot.Width / 2.0f + marginX && std::abs(Displacement(centerY, y, snapshot.SceneHeight, snapshot.Wrap & 2)) <= snapshot.Height / 2.0f + marginY);
            }
            if (entry->second) snapshot.Nodes.push_back(node);
        }
    }
    lap(times[2]);
    impl.Layer(snapshot.Nodes, scene->GetTerrain()->GetFGSceneLayer(), 101, impl.Camera, snapshot.Width, snapshot.Height);
    const auto* activity = g_ActivityMan.GetActivity(); const int team = activity->GetTeamOfPlayer(player);
    snapshot.Phase = uint8_t(activity->GetActivityState());
    if (activity->GetActivityState() != Activity::Editing) impl.Layer(snapshot.Nodes, scene->GetUnseenLayer(team), 102, impl.Camera, snapshot.Width, snapshot.Height, true);
    lap(times[3]);
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
    lap(times[4]);
    snapshot.CriticalNodes = impl.CriticalNodes;
    BoundSnapshot(snapshot);
    lap(times[5]);
    return snapshot;
}
std::array<uint64_t, 6> MultiplayerWorld::TakeComposeTimes() { const auto times = s_ComposeTimes; s_ComposeTimes = {}; return times; }
const Resource* MultiplayerWorld::FindResource(uint64_t id) const { auto found = m_Impl->Resources.find(id); return found == m_Impl->Resources.end() ? nullptr : &found->second; }
std::vector<uint64_t> MultiplayerWorld::PrepareScene() {
    auto& impl = *m_Impl;
    auto* scene = g_SceneMan.GetScene(); if (!scene) return {};
    auto cache = [&](BITMAP* bitmap) {
        if (!bitmap) return;
        for (int y = 0; y < bitmap->h; y += TileSize) for (int x = 0; x < bitmap->w; x += TileSize) {
            const auto id = impl.Asset(bitmap, x, y, std::min<int>(TileSize, bitmap->w - x), std::min<int>(TileSize, bitmap->h - y));
            if (id) impl.SceneAssets.insert(id);
        }
    };
    for (const auto* layer : scene->GetBackLayers()) cache(layer->GetBitmap());
    cache(scene->GetTerrain()->GetBGSceneLayer()->GetBitmap());
    cache(scene->GetTerrain()->GetFGSceneLayer()->GetBitmap());
    return {impl.SceneAssets.begin(), impl.SceneAssets.end()};
}
void MultiplayerWorld::PinScene(const std::unordered_set<uint64_t>& assets) { m_Impl->SceneAssets = assets; }
SceneMap MultiplayerWorld::PrepareSceneMap(int width, int height) {
    auto& impl = *m_Impl; SceneMap map; map.CameraX = impl.Camera.GetX(); map.CameraY = impl.Camera.GetY();
    auto* scene = g_SceneMan.GetScene(); if (!scene) return map;
    uint16_t ordinal = 1;
    for (auto it = scene->GetBackLayers().rbegin(); it != scene->GetBackLayers().rend(); ++it) impl.Layer(map.Nodes, *it, ordinal++, impl.Camera, width, height, true);
    impl.Layer(map.Nodes, scene->GetTerrain()->GetBGSceneLayer(), 100, impl.Camera, width, height, true);
    impl.Layer(map.Nodes, scene->GetTerrain()->GetFGSceneLayer(), 101, impl.Camera, width, height, true);
    return map;
}
void MultiplayerWorld::InstallSceneMap(const SceneMap& map) {
    m_Impl->SceneLayers.Install(map);
    for (const auto& n : map.Nodes) if (n.Type == Shape::Sprite) m_Impl->LastVisuals[n.ID] = n;
}
void MultiplayerWorld::PrimeScene(const Scene& scene, const std::unordered_set<uint64_t>& wanted) {
    // Preset backdrops already contain passive, installed bitmap resources.
    // Only matching content hashes are reused; gameplay stays on the host, and
    // this never constructs actors or runs activity scripts.
    for (const auto* layer : scene.GetBackLayers()) if (auto* bitmap = layer->GetBitmap()) {
        for (int y = 0; y < bitmap->h; y += TileSize) for (int x = 0; x < bitmap->w; x += TileSize)
            m_Impl->Asset(bitmap, x, y, std::min<int>(TileSize, bitmap->w - x), std::min<int>(TileSize, bitmap->h - y));
    }
    // Most terrain is unchanged from the scene's own data. Rebuilding it here
    // replaces downloading thousands of tiles; tiles that differ (damage,
    // placed bunkers, random debris) do not match and still stream.
    auto& impl = *m_Impl;
    if (impl.LocalScene != scene.GetPresetName()) {
        impl.LocalScene = scene.GetPresetName(); impl.LocalTiles.clear();
        if (const auto* preset = scene.GetTerrain()) {
            // Never drawn, so no GPU textures are created for these layers.
            g_SceneLayersWithoutTextures = true;
            std::unique_ptr<SLTerrain> terrain(dynamic_cast<SLTerrain*>(preset->Clone()));
            if (terrain && terrain->LoadPresentationLayers() >= 0) for (BITMAP* bitmap : {terrain->GetBGColorBitmap(), terrain->GetFGColorBitmap()}) if (bitmap) {
                for (int y = 0; y < bitmap->h; y += TileSize) for (int x = 0; x < bitmap->w; x += TileSize)
                    impl.LocalTile(bitmap, x, y, std::min<int>(TileSize, bitmap->w - x), std::min<int>(TileSize, bitmap->h - y));
            }
            terrain.reset(); g_SceneLayersWithoutTextures = false;
        }
    }
    for (const auto id : wanted) if (const auto tile = impl.LocalTiles.find(id); tile != impl.LocalTiles.end() && !impl.Resources.contains(id)) impl.Store(tile->second);
}
bool MultiplayerWorld::Install(Resource resource) {
    auto& impl = *m_Impl; if (resource.ID != ResourceHash(resource)) return false;
    return impl.Store(std::move(resource));
}
bool MultiplayerWorld::SceneryReady(const Snapshot& snapshot) const {
    // The first view waits for the terrain and scenery it shows. Objects,
    // effects and HUD images appear as they arrive, as they do afterwards;
    // combat keeps introducing new ones, so waiting for all could never end.
    for (const auto& node : snapshot.Nodes) if (node.Asset && LayerOrdinal(node) && !m_Impl->Resources.contains(node.Asset)) return false;
    std::unordered_set<uint64_t> visible; m_Impl->SceneLayers.VisibleAssets(snapshot, visible);
    return std::all_of(visible.begin(), visible.end(), [&](uint64_t id) { return m_Impl->Resources.contains(id); });
}
std::vector<uint64_t> MultiplayerWorld::MissingScenery(const Snapshot& snapshot) const {
    std::unordered_set<uint64_t> ids; for (const auto& node : snapshot.Nodes) if (node.Asset && LayerOrdinal(node) && !m_Impl->Resources.contains(node.Asset)) ids.insert(node.Asset);
    std::unordered_set<uint64_t> visible; m_Impl->SceneLayers.VisibleAssets(snapshot, visible);
    for (auto id : visible) if (!m_Impl->Resources.contains(id)) ids.insert(id);
    return {ids.begin(), ids.end()};
}
std::vector<uint64_t> MultiplayerWorld::Missing(const Snapshot& snapshot) const {
    std::unordered_set<uint64_t> ids; for (const auto& node : snapshot.Nodes) if (node.Asset && !m_Impl->Resources.contains(node.Asset)) ids.insert(node.Asset);
    // Snapshots carry only changed tiles; the retained map supplies the rest of this view.
    std::unordered_set<uint64_t> visible; m_Impl->SceneLayers.VisibleAssets(snapshot, visible);
    for (auto id : visible) if (!m_Impl->Resources.contains(id)) ids.insert(id);
    return {ids.begin(), ids.end()};
}
bool MultiplayerWorld::Install(Snapshot snapshot, uint64_t time) { if ((!Ready() && !SceneryReady(snapshot)) || !m_Impl->States.Push(std::move(snapshot), time)) return false; m_Impl->SceneLayers.Update(m_Impl->States.Latest()); ++m_Impl->UpdateCount; return true; }
bool MultiplayerWorld::Ready() const { return !m_Impl->States.Empty(); }
unsigned MultiplayerWorld::StaleTiles() const { return m_Impl->StaleTiles; }
unsigned MultiplayerWorld::BlackTiles() const { return m_Impl->BlackTiles; }
unsigned MultiplayerWorld::BlackFogTiles() const { return m_Impl->BlackFogTiles; }
bool MultiplayerWorld::RenderedCenter(Vector& center) const {
    const auto& sample = m_Impl->LastSample; if (!sample.ID) return false;
    center.SetXY(sample.CameraX + sample.Width / 2.0f, sample.CameraY + sample.Height / 2.0f); return true;
}
bool MultiplayerWorld::Paused() const { return Ready() && m_Impl->States.Latest().Paused; }
bool MultiplayerWorld::IsDeploying() const { return Ready() && m_Impl->LastSample.Phase == Activity::Editing; }
int MultiplayerWorld::Width() const { return Ready() ? m_Impl->States.Latest().Width : 0; }
int MultiplayerWorld::Height() const { return Ready() ? m_Impl->States.Latest().Height : 0; }
uint64_t MultiplayerWorld::Rendered() const { return m_Impl->RenderCount; }
uint64_t MultiplayerWorld::Updates() const { return m_Impl->UpdateCount; }
uint64_t MultiplayerWorld::IntermediateFrames() const { return m_Impl->IntermediateCount; }
bool MultiplayerWorld::VerifyPresentation(std::ostream& log) {
    bool passed = true;
    auto check = [&](bool result, const char* name) { log << (result ? "OK: " : "FAIL: ") << name << '\n'; passed &= result; };
    BITMAP* gui = create_bitmap_ex(8, 640, 360);
    Controller controller;
    auto* preset = dynamic_cast<const PieMenu*>(g_PresetMan.GetEntityPreset("PieMenu", "Default Human Pie Menu", "Base.rte"));
    check(preset != nullptr, "native radial menu fixture loaded");
    if (preset) {
        std::unique_ptr<PieMenu> pie(static_cast<PieMenu*>(preset->Clone()));
        pie->SetMenuController(&controller); pie->SetPos(Vector(320, 180));
        pie->SetEnabled(true, false); pie->FreezeAtRadius(30); pie->Update();
        BeginView(gui, Vector()); pie->Draw(gui, Vector());
        check(std::any_of(m_Impl->Canvas.begin(), m_Impl->Canvas.end(), [](const Node& n) { return n.Type == Shape::Sprite && n.Width >= 60 && n.Height >= 60; }), "native radial background reaches the guest canvas");
        gui->vtable = m_Impl->OriginalVTable; canvasCollector = nullptr; m_Impl->GUI = nullptr;
    }
    BeginView(gui, Vector());
    line(gui, 200, 90, 220, 90, g_WhiteColor);
    check(m_Impl->Canvas.size() == 1 && m_Impl->Canvas[0].X2 - m_Impl->Canvas[0].X == 20, "HUD line keeps its short native endpoints");
    BITMAP* sub = create_sub_bitmap(gui, 100, 100, 100, 100);
    line(sub, 10, 10, 30, 10, g_WhiteColor);
    check(m_Impl->Canvas.size() == 2 && m_Impl->Canvas.back().X == 110 && m_Impl->Canvas.back().Y == 110, "clipped GUI sub-bitmap commands reach the guest in screen coordinates");
    BITMAP* preview = create_bitmap_ex(8, 16, 16); clear_to_color(preview, 0); putpixel(preview, 5, 7, g_WhiteColor);
    const auto originalAsset = m_Impl->Asset(preview);
    check(originalAsset && m_Impl->Asset(preview) == originalAsset, "unchanged mutable bitmaps reuse their resource without hashing or allocating again");
    putpixel(preview, 6, 7, g_WhiteColor);
    check(m_Impl->Asset(preview) != originalAsset, "bitmap-region cache detects native terrain and GUI pixel changes");
    check(CanvasOverlay(preview, gui) && m_Impl->Canvas.back().Width == 2 && m_Impl->Canvas.back().Height == 1, "deployment preview texture reaches the guest as a cropped native overlay");
    {
        SceneLayer emptyLayer; BITMAP* transparent = create_bitmap_ex(8, 64, 64); clear_to_color(transparent, g_MaskColor);
        emptyLayer.Create(transparent, true, Vector(), false, false, Vector(-1, -1));
        std::vector<Node> tiles; m_Impl->Layer(tiles, &emptyLayer, 100, Vector(), 640, 360);
        check(tiles.size() == 1 && m_Impl->EmptyMaskedAssets.contains(tiles.front().Asset), "transparent terrain explicitly clears its retained tile");
        tiles.clear();
        putpixel(transparent, 10, 10, g_WhiteColor); MarkChanged(transparent, 10, 10, 1, 1); m_Impl->Layer(tiles, &emptyLayer, 100, Vector(), 640, 360);
        check(!tiles.empty() && !m_Impl->EmptyMaskedAssets.contains(tiles.front().Asset), "changed terrain immediately restores a formerly transparent tile");
    }
    destroy_bitmap(preview);
    destroy_bitmap(sub);
    gui->vtable = m_Impl->OriginalVTable; canvasCollector = nullptr; m_Impl->GUI = nullptr;
    destroy_bitmap(gui); Reset();
    Resource strip; strip.Width = 400; strip.Height = 1; strip.Pixels.assign(400, uint8_t(g_WhiteColor)); strip.ID = ResourceHash(strip); Install(strip);
    Snapshot first; first.ID = 1; first.Time = 1000; first.Width = first.SceneWidth = 640; first.Height = first.SceneHeight = 360;
    Node hud; hud.ID = uint64_t(1) << 63; hud.Flags = ScreenSpace | Masked; hud.Asset = strip.ID; hud.X = 20; hud.Y = 50; hud.Width = hud.SourceWidth = 400; hud.Height = hud.SourceHeight = 1; first.Nodes.push_back(hud);
    Install(first, 1020); Render(1020);
    auto second = first; second.ID = 2; second.Time = 1050; second.Nodes[0].Asset = strip.ID + 1; second.Nodes[0].X = 200; second.Nodes[0].Y = 80; second.Nodes[0].Width = second.Nodes[0].SourceWidth = 8; second.Nodes[0].Height = second.Nodes[0].SourceHeight = 8;
    Install(second, 1070); const unsigned rendered = Render(1250);
    auto* pixels = static_cast<unsigned char*>(rlReadTexturePixels(rendered, 640, 360, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8));
    size_t colored = 0; if (pixels) for (size_t i = 0; i < 640 * 360 * 4; i += 4) if (pixels[i] || pixels[i + 1] || pixels[i + 2]) ++colored;
    check(pixels && colored == 0, "missing HUD icon does not reuse an unrelated 400-pixel strip");
    MemFree(pixels); Reset();
    Install(strip); first.Nodes[0].Flags = Masked | Discontinuous; first.Nodes[0].ID = uint64_t(1) << 60;
    second = first; second.ID = 2; second.Time = 1050; second.Nodes[0].Asset = strip.ID + 1; second.Nodes[0].X = 200; second.Nodes[0].Width = second.Nodes[0].SourceWidth = 8;
    Install(first, 1020); Render(1020); Install(second, 1070);
    pixels = static_cast<unsigned char*>(rlReadTexturePixels(Render(1250), 640, 360, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8));
    colored = 0; if (pixels) for (size_t i = 0; i < 640 * 360 * 4; i += 4) if (pixels[i] || pixels[i + 1] || pixels[i + 2]) ++colored;
    check(pixels && colored == 0, "missing transient world effect does not reuse another effect ordinal as a horizontal strip");
    MemFree(pixels); Reset();
    {
        Resource fog; fog.Width = 8; fog.Height = 1; fog.Pixels = {uint8_t(g_MaskColor), uint8_t(g_MaskColor), uint8_t(g_MaskColor), uint8_t(g_MaskColor), uint8_t(g_BlackColor), uint8_t(g_BlackColor), uint8_t(g_BlackColor), uint8_t(g_BlackColor)};
        fog.ID = ResourceHash(fog); Install(fog);
        SceneMap map; Node ground; ground.ID = (uint64_t(1) << 61) | (uint64_t(100) << 32) | 1; ground.Type = Shape::Rectangle;
        ground.Flags = ScreenSpace | MP::World::Layer; ground.Width = 4096; ground.Height = 360; ground.X3 = ground.Y3 = 1; ground.Color = g_WhiteColor;
        map.Nodes = {ground}; InstallSceneMap(map);
        Snapshot view; view.ID = 1; view.Time = 1000; view.Width = 640; view.Height = view.SceneHeight = 360; view.SceneWidth = 4096; view.ViewMode = Activity::Observe; view.MouseScale = 1;
        Node veil; veil.ID = (uint64_t(1) << 62) | (uint64_t(102) << 48); veil.Flags = ScreenSpace | MP::World::Layer | Masked; veil.Asset = fog.ID;
        veil.Width = 4096; veil.Height = 360; veil.SourceWidth = 8; veil.SourceHeight = 1; veil.X3 = veil.Y3 = 1; view.Nodes = {veil}; Install(view, 1020);
        MP::Input input; SetLocalInput(input, true); Render(1020); input.MouseX = 1200; SetLocalInput(input, true);
        for (uint64_t i = 1; i <= 90; ++i) Render(1020 + i * 16);
        auto* image = static_cast<unsigned char*>(rlReadTexturePixels(Render(2476), 640, 360, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8));
        const size_t center = (180 * 640 + 320) * 4;
        check(image && (image[center] || image[center + 1] || image[center + 2]), "free flight draws cached scenery more than a screen beyond the host view without another snapshot");
        MemFree(image); input.MouseX = 2500; SetLocalInput(input, true);
        for (uint64_t i = 1; i <= 90; ++i) Render(2476 + i * 16);
        image = static_cast<unsigned char*>(rlReadTexturePixels(Render(3932), 640, 360, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8));
        check(image && !image[center] && !image[center + 1] && !image[center + 2], "local free flight keeps unexplored scenery obscured beyond the host viewport");
        MemFree(image);
        view.ID = 2; view.Time = 4000; view.Nodes[0].Asset = fog.ID + 1; Install(view, 4020);
        input.MouseX -= 1300; SetLocalInput(input, true);
        for (uint64_t i = 1; i <= 90; ++i) Render(4020 + i * 16);
        image = static_cast<unsigned char*>(rlReadTexturePixels(Render(5476), 640, 360, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8));
        check(image && (image[center] || image[center + 1] || image[center + 2]), "a missing fog revision blacks out its tile instead of keeping the team's last fog");
        MemFree(image); Reset();
    }
    BeginTrails();
    std::vector<std::pair<int, int>> trail;
    for (int i = 0; i < 512; ++i) trail.emplace_back(i + 10, 100 + i / 8);
    for (int i = 0; i < 20; ++i) Trail(trail, uint8_t(g_WhiteColor));
    check(m_Impl->Trails.size() <= 20, "twenty long bullet trails require at most twenty retained commands");
    Snapshot combat; combat.ID = 1; combat.Time = m_Impl->TrailTime; combat.Width = combat.SceneWidth = 640; combat.Height = combat.SceneHeight = 360;
    combat.Nodes.assign(m_Impl->Trails.begin(), m_Impl->Trails.end());
    MP::Writer wire(MP::Kind::WorldSnapshot); WriteSnapshot(wire, combat);
    check(wire.Data.size() < 55000, "long bullet trails stay below a 55 KB update budget");
    Install(combat, combat.Time); const auto trailTexture = Render(combat.Time + InterpolationMS);
    pixels = static_cast<unsigned char*>(rlReadTexturePixels(trailTexture, 640, 360, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8));
    colored = 0; if (pixels) for (size_t i = 0; i < 640 * 360 * 4; i += 4) if (pixels[i] || pixels[i + 1] || pixels[i + 2]) ++colored;
    check(pixels && colored == trail.size(), "packed trails retain every native raster pixel without adding connecting lines");
    MemFree(pixels); Reset();
    BeginTrails();
    for (int i = 0; i < 5000; ++i) { const std::pair<int, int> point{i % 640, i / 640}; Trail(std::span(&point, 1), uint8_t(g_WhiteColor)); Trail(std::span(&point, 1), uint8_t(g_YellowGlowColor)); }
    check(m_Impl->Trails.size() < 20 && m_Impl->TrailPixels == 5000 && std::all_of(m_Impl->Trails.begin(), m_Impl->Trails.end(), [](const auto& path) { return std::all_of(path.PixelColors.begin(), path.PixelColors.end(), [](uint8_t color) { return color == g_YellowGlowColor; }); }), "fragment raster batching deduplicates pixels and preserves the final native color");
    Reset();
    return passed;
}
unsigned MultiplayerWorld::Render(uint64_t time) {
    auto& impl = *m_Impl; if (impl.States.Empty()) return 0;
    Snapshot scene = impl.States.Sample(time);
    if (impl.LocalInput.Device == DEVICE_MOUSE_KEYB) impl.Pointer.Apply(scene, impl.States.Latest(), impl.LocalInput, impl.LocalInputEnabled);
    impl.CameraPrediction.Apply(scene, impl.States.Latest(), impl.LocalInput, time, impl.LocalInputEnabled);
    if (impl.LocalInputEnabled) {
        const bool radial = (impl.LocalInput.Held & ((uint64_t(1) << INPUT_PIEMENU_ANALOG) | (uint64_t(1) << INPUT_PIEMENU_DIGITAL))) != 0;
        PredictLocalView(scene, impl.States.Latest(), impl.LocalInput.AimX, impl.LocalInput.AimY, radial);
        if (scene.ViewMode == Activity::ActorSelect && impl.States.Latest().ViewMode != Activity::ActorSelect) {
            std::erase_if(scene.Nodes, [](const Node& n) { return n.Control == Interaction::Aim; });
            MP::Input view; impl.CameraPrediction.Export(view, true);
            Node cursor; cursor.ID = (uint64_t(1) << 63) | 0x7fffffff; cursor.Type = Shape::CircleOutline; cursor.Flags = 0; cursor.X = view.CursorX; cursor.Y = view.CursorY;
            cursor.Width = time / 150 % 2 ? 6 : 8; cursor.Color = g_YellowGlowColor; scene.Nodes.push_back(cursor);
        }
    }
    impl.SceneLayers.Compose(scene);
    if (impl.LastSample.ID == scene.ID && (impl.LastSample.CameraX != scene.CameraX || impl.LastSample.CameraY != scene.CameraY || impl.LastSample.Nodes != scene.Nodes)) ++impl.IntermediateCount;
    impl.LastSample = scene;
    // An animation frame or changed terrain tile can arrive after its pose.
    // Retain its last complete visual while still presenting fresh movement.
    std::unordered_set<uint64_t> visible; impl.StaleTiles = impl.BlackTiles = impl.BlackFogTiles = 0;
    for (auto& n : scene.Nodes) if (n.Type == Shape::Sprite) {
        visible.insert(n.ID);
        const bool fog = LayerOrdinal(n) == 102;
        // Terrain that is still streaming in is covered rather than shown as a
        // hole through to the sky or background.
        const bool terrain = LayerOrdinal(n) == 100 || LayerOrdinal(n) == 101;
        // A fog tile changes whenever an actor reveals more of it, so the tile
        // around the player's own actor is usually one revision behind. Its last
        // version shows only what this team has already seen; covering the whole
        // tile instead hid the player's own surroundings for a round trip.
        const bool retained = !(n.Flags & Discontinuous) && (!(n.Flags & ScreenSpace) || (n.Flags & MP::World::Layer));
        const Node* arrived = nullptr;
        if (LayerOrdinal(n)) {
            auto& revisions = impl.TileRevisions[n.ID];
            if (revisions.empty() || revisions.back().Asset != n.Asset) { revisions.push_back(n); if (revisions.size() > 6) revisions.pop_front(); }
            if (retained && !impl.Resources.contains(n.Asset)) for (auto revision = revisions.rbegin(); revision != revisions.rend() && !arrived; ++revision) if (impl.Resources.contains(revision->Asset)) arrived = &*revision;
        }
        if (impl.Resources.contains(n.Asset)) { if (retained) impl.LastVisuals[n.ID] = n; }
        else if (arrived) {
            impl.LastVisuals[n.ID] = *arrived; ++impl.StaleTiles;
            n.Asset = arrived->Asset; n.SourceX = arrived->SourceX; n.SourceY = arrived->SourceY; n.SourceWidth = arrived->SourceWidth; n.SourceHeight = arrived->SourceHeight;
            n.Width = arrived->Width; n.Height = arrived->Height; n.PivotX = arrived->PivotX; n.PivotY = arrived->PivotY;
        }
        // The scene map seeds these visuals before every tile has streamed in,
        // so a previous visual is only usable once its own resource is present.
        else if (auto previous = impl.LastVisuals.find(n.ID); retained && previous != impl.LastVisuals.end() && impl.Resources.contains(previous->second.Asset)) {
            const auto& old = previous->second;
            n.Asset = old.Asset; n.SourceX = old.SourceX; n.SourceY = old.SourceY; n.SourceWidth = old.SourceWidth; n.SourceHeight = old.SourceHeight;
            n.Width = old.Width; n.Height = old.Height; n.PivotX = old.PivotX; n.PivotY = old.PivotY;
            if (LayerOrdinal(n)) ++impl.StaleTiles;
        } else if (fog || terrain) { n.Type = Shape::Rectangle; n.Asset = 0; n.Color = g_BlackColor; n.Flags &= ~Masked; ++(fog ? impl.BlackFogTiles : impl.BlackTiles); }
        else n.Asset = 0;
    }
    std::erase_if(impl.LastVisuals, [&](const auto& entry) { return !visible.contains(entry.first) && !LayerOrdinal(entry.second); });
    std::erase_if(impl.TileRevisions, [&](const auto& entry) { return !visible.contains(entry.first); });
    if (!impl.Target || impl.Target->GetSize().w != scene.Width || impl.Target->GetSize().h != scene.Height) impl.Target = std::make_unique<RenderTarget>(FloatRect(0, 0, scene.Width, scene.Height), FloatRect(0, 0, scene.Width, scene.Height));
    impl.Target->Begin(); rlDisableDepthTest(); rlEnableColorBlend(); rlSetBlendMode(RL_BLEND_ALPHA);
    if (!impl.SceneShader) impl.SceneShader = std::make_unique<Shader>("Base.rte/Shaders/Blit8.vert", "Base.rte/Shaders/Replica.frag");
    Shader& shader = *impl.SceneShader;
    shader.Begin(); shader.Enable(); rlSetUniformSampler(shader.GetUniformLocation("rtePalette"), g_PostProcessMan.GetPaletteTexture());
    std::array<int, 13> lastStyle{}; lastStyle.fill(-1);
    std::unordered_map<uint64_t, const Node*> anchors; for (const auto& n : scene.Nodes) if (!(n.Flags & ScreenSpace)) anchors[n.ID] = &n;
    for (const Node& n : scene.Nodes) {
        if (n.Type == Shape::Sprite && !n.Asset) continue;
        if (n.Type == Shape::Sprite && !(n.Flags & ScreenSpace) &&
            (std::abs(Displacement(scene.CameraX + scene.Width / 2, n.X, scene.SceneWidth, scene.Wrap & 1)) > scene.Width / 2 + n.Width + n.Height ||
             std::abs(Displacement(scene.CameraY + scene.Height / 2, n.Y, scene.SceneHeight, scene.Wrap & 2)) > scene.Height / 2 + n.Width + n.Height)) continue;
        Texture2D texture{};
        const auto resource = n.Type == Shape::Sprite ? impl.Resources.find(n.Asset) : impl.Resources.end();
        if (n.Type == Shape::Sprite && resource == impl.Resources.end()) continue;
        const bool trueColor = n.Type == Shape::Sprite && resource->second.Depth == 32;
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
            const float spanX = n.Flags & MP::World::Layer ? n.X2 : 0, spanY = n.Flags & MP::World::Layer ? n.Y2 : 0;
            const int firstX = spanX > 0 ? int(std::ceil((-x - n.Width) / spanX)) : 0, lastX = spanX > 0 ? int(std::floor((scene.Width - x) / spanX)) : 0;
            const int firstY = spanY > 0 ? int(std::ceil((-y - n.Height) / spanY)) : 0, lastY = spanY > 0 ? int(std::floor((scene.Height - y) / spanY)) : 0;
            for (int ry = firstY; ry <= lastY; ++ry) for (int rx = firstX; rx <= lastX; ++rx)
                DrawTexturePro(texture, source, {x + rx * spanX, y + ry * spanY, n.Width, n.Height}, {n.PivotX, n.PivotY}, n.Angle, {255, 255, 255, n.Alpha});
        } else {
            RLColor color{n.Color, 0, 0, n.Alpha};
            if (n.Type == Shape::PixelPath) {
                for (size_t i = 0; i < n.Pixels.size(); ++i) { const auto [px, py] = n.Pixels[i];
                    const int tx = int(scene.Width / 2 + Displacement(scene.CameraX + scene.Width / 2, n.X + px, scene.SceneWidth, scene.Wrap & 1));
                    const int ty = int(scene.Height / 2 + Displacement(scene.CameraY + scene.Height / 2, n.Y + py, scene.SceneHeight, scene.Wrap & 2));
                    if (tx >= 0 && ty >= 0 && tx < scene.Width && ty < scene.Height) DrawRectangle(tx, ty, 1, 1, {n.PixelColors[i], 0, 0, n.Alpha});
                }
            }
            else if (n.Type == Shape::Pixel) DrawRectangle(int(x), int(y), 1, 1, color);
            else if (n.Type == Shape::Rectangle) {
                const float spanX = LayerOrdinal(n) >= 100 ? n.X2 : 0, spanY = LayerOrdinal(n) >= 100 ? n.Y2 : 0;
                const int firstX = spanX > 0 ? int(std::ceil((-x - n.Width) / spanX)) : 0, lastX = spanX > 0 ? int(std::floor((scene.Width - x) / spanX)) : 0;
                const int firstY = spanY > 0 ? int(std::ceil((-y - n.Height) / spanY)) : 0, lastY = spanY > 0 ? int(std::floor((scene.Height - y) / spanY)) : 0;
                for (int ry = firstY; ry <= lastY; ++ry) for (int rx = firstX; rx <= lastX; ++rx)
                    DrawRectangle(int(x + rx * spanX), int(y + ry * spanY), int(n.Width), int(n.Height), color);
            }
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
    rlDrawRenderBatchActive(); glDisable(GL_SCISSOR_TEST); rlSetBlendMode(RL_BLEND_ALPHA); shader.End(); impl.Target->End(); ++impl.RenderCount;
    return impl.Target->GetColorTexture().id;
}
bool MultiplayerWorld::CanvasSprite(BITMAP* bitmap, Rectangle source, Rectangle dest, Vector2 pivot, float angle, RLColor tint, BITMAP* target) {
    if (!canvasCollector || !Impl::CanvasFor(target ? target : canvasCollector->m_Impl->GUI)) return false;
    auto& impl = *canvasCollector->m_Impl; Node n; n.Asset = impl.Asset(bitmap); n.X = dest.x; n.Y = dest.y; n.Width = std::abs(dest.width); n.Height = std::abs(dest.height); n.PivotX = pivot.x; n.PivotY = pivot.y; n.Angle = angle;
    n.SourceX = source.x; n.SourceY = source.y; n.SourceWidth = std::abs(source.width); n.SourceHeight = std::abs(source.height); n.Flags = Masked | (source.width < 0 || dest.width < 0 ? FlipX : 0) | (source.height < 0 || dest.height < 0 ? FlipY : 0); n.Alpha = tint.a; impl.AppendCanvas(n); return true;
}
size_t MultiplayerWorld::CanvasCheckpoint(BITMAP* target) { const auto* impl = Impl::CanvasFor(target); return impl ? impl->Canvas.size() : SIZE_MAX; }
bool MultiplayerWorld::CanvasOverlay(BITMAP* bitmap, BITMAP* target, size_t before) {
    auto* impl = Impl::CanvasFor(target); if (!impl) return false;
    int left = bitmap->w, top = bitmap->h, right = -1, bottom = -1;
    for (int y = 0; y < bitmap->h; ++y) for (int x = 0; x < bitmap->w; ++x) if (bitmap->line[y][x] != g_MaskColor) { left = std::min(left, x); top = std::min(top, y); right = std::max(right, x); bottom = std::max(bottom, y); }
    if (right < left) return true;
    Node n; n.Asset = impl->Asset(bitmap, left, top, right - left + 1, bottom - top + 1); if (!n.Asset) return true;
    if (impl->Canvas.size() >= MaxNodes) return true;
    n.X = float(left); n.Y = float(top); n.Width = n.SourceWidth = float(right - left + 1); n.Height = n.SourceHeight = float(bottom - top + 1);
    const auto previous = impl->HUDControl; impl->HUDControl = Interaction::WorldOverlay; impl->AppendCanvas(n); impl->HUDControl = previous;
    if (before < impl->Canvas.size() - 1) std::rotate(impl->Canvas.begin() + before, std::prev(impl->Canvas.end()), impl->Canvas.end());
    return true;
}
bool MultiplayerWorld::Primitive(const GraphicalPrimitive& primitive, const Vector& offset) {
    if (!canvasCollector) return false;
    using P = GraphicalPrimitive::PrimitiveType;
    auto& impl = *canvasCollector->m_Impl; impl.CanvasTarget = impl.GUI; Node n; n.X = primitive.m_StartPos.GetX() + offset.GetX(); n.Y = primitive.m_StartPos.GetY() + offset.GetY(); n.X2 = primitive.m_EndPos.GetX() + offset.GetX(); n.Y2 = primitive.m_EndPos.GetY() + offset.GetY(); n.Color = primitive.m_Color;
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
