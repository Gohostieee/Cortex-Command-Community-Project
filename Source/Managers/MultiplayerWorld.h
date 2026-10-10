#pragma once
#include "MultiplayerWorldProtocol.h"
#include "Vector.h"
#include "raylib/raylib.h"
#include <memory>
#include <iosfwd>

struct BITMAP;
namespace RTE {
class MovableObject;
class SceneLayer;
class GraphicalPrimitive;
class Actor;
class Scene;

// Passive native presentation replicas. Gameplay objects and scripts stay on
// the host; guests own retained resources and render state, not actor clones.
class MultiplayerWorld {
public:
    MultiplayerWorld();
    ~MultiplayerWorld();
    void Reset();
    void ResetPresentation();
    void BeginObjects();
    void BeginTrails();
    void EndObjects();
    void BeginView(BITMAP* gui, const Vector& camera);
    MP::World::Snapshot EndView(int player, uint32_t id, uint32_t inputSequence, uint64_t time);
    // Composition time by stage (backdrops, trails, objects, foreground and fog, canvas and effects, bounding), for profiling.
    static std::array<uint64_t, 6> TakeComposeTimes();
    const MP::World::Resource* FindResource(uint64_t id) const;
    bool Install(MP::World::Resource resource);
    bool Install(MP::World::Snapshot snapshot, uint64_t time);
    std::vector<uint64_t> Missing(const MP::World::Snapshot& snapshot) const;
    bool SceneryReady(const MP::World::Snapshot& snapshot) const;
    // The assets SceneryReady still waits for.
    std::vector<uint64_t> MissingScenery(const MP::World::Snapshot& snapshot) const;
    std::vector<uint64_t> PrepareScene();
    MP::World::SceneMap PrepareSceneMap(int width, int height);
    void InstallSceneMap(const MP::World::SceneMap& map);
    void PrimeSceneBackdrops(const Scene& scene);
    void PinScene(const std::unordered_set<uint64_t>& assets);
    unsigned Render(uint64_t time);
    // Center of the most recently rendered local camera, for the audio listener.
    bool RenderedCenter(Vector& center) const;
    bool Ready() const;
    bool Paused() const;
    bool IsDeploying() const;
    int Width() const;
    int Height() const;
    uint64_t Rendered() const;
    uint64_t Updates() const;
    uint64_t IntermediateFrames() const;
    bool VerifyPresentation(std::ostream& log);
    void SetLocalInput(const MP::Input& input, bool enabled);
    void ExportLocalView(MP::Input& input, bool enabled) const;
    // Replicated scene bitmaps (terrain colour layers and team fog) record the
    // 64-pixel tiles each write touches, so capture compares only those tiles
    // plus a slow verification sweep instead of every visible tile per guest.
    static void MarkChanged(BITMAP* bitmap, int x, int y, int width, int height);
    static void BeginAim(const Actor& actor, int screen);
    static void BeginRadialCursor();
    static void BeginRadialBackground();
    static void BeginPointer(float x, float y);
    static void BeginWorldCursor();
    static void EndInteraction();
    static void Sprite(const MovableObject& owner, BITMAP* bitmap, const Vector& position, const Vector& pivot, float angle, float scale, bool flip, bool white = false, uint8_t alpha = 255);
    static void Pixel(const MovableObject& owner, const Vector& position, uint8_t color);
    static void Trail(std::span<const std::pair<int, int>> positions, uint8_t color);
    static bool Flash(int width, int height, uint8_t color);
    static void BeginHUD(const MovableObject& owner);
    static void EndHUD();
    // Native GPU/GUI adapters use the same semantic sprite representation.
    static bool CanvasSprite(BITMAP* bitmap, Rectangle source, Rectangle dest, Vector2 pivot, float angle, RLColor tint, BITMAP* target = nullptr);
    static size_t CanvasCheckpoint(BITMAP* target);
    static bool CanvasOverlay(BITMAP* bitmap, BITMAP* target, size_t before = SIZE_MAX);
    static bool Primitive(const GraphicalPrimitive& primitive, const Vector& offset);
    static void EndPrimitive();
private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};
}
