# Multiplayer combat debris specification

Status: proposed, 2026-10-11. Branch `codex/multiplayer-rebuild`, gameplay
protocol v10. Part of the [remaining-work specification](MultiplayerRemainingWorkSpec.md).

## 1. Problem

When a dropship or other craft is destroyed, guests see little or none of its
debris: the metal hull pieces and other gibs that the host shows flying apart.
Grenade and actor gibs are affected the same way.

This is not a limitation inherited from the retired multiplayer. That
implementation streamed finished frames, so anything on the host's screen
appeared. The rebuild sends authoritative object state and content-addressed
sprite resources, and guests draw the scene locally. Debris is lost in that
path in two places. The causes below come from reading the code; they have
not yet been measured (§4 does that first).

### 1.1 The snapshot budget drops debris first

Each snapshot (every 50 ms) is fitted to the guest's upload budget by
`MP::World::FitSnapshot`. At the default 3 Mbps per player this is about
14 KB compressed. Nodes count as essential when they are screen-space, a
scene layer, or "critical". Critical means captured from an object whose
root is an actor or device (`MultiplayerWorld::Impl::AppendObject`).

Once a craft gibs, its pieces are no longer attached to an actor, so they
become optional. The fitter ranks optional nodes in only two groups: raster
trails first, then everything else in painter order. It keeps an evenly
spaced fraction of each group. A craft explosion adds a dozen rigid hull gibs
and hundreds to thousands of fire, smoke and spark particles to that same
group, so the gibs are thinned out in proportion to the particles. The
fraction is recomputed from each frame's index, so a gib that survives one
snapshot can be missing from the next. Guests interpolate only nodes present
in consecutive states, so such a gib flickers or never moves smoothly.

`MP::World::BoundSnapshot`, the hard 32,768-node and 4 MB cap, already ranks
sprites above pixels and discontinuous effects. `FitSnapshot` does not.

### 1.2 Every new sprite must be downloaded

A node references its image by `ResourceHash`, the hash of the pixels plus
width, height and depth. The guest draws a sprite node only when it holds
that resource (`MultiplayerWorld::Render`). Guests prime only backdrops and
pristine terrain from their own game data (`MultiplayerWorld::PrimeScene`).
Every object sprite (actor parts, weapons, gibs, particle frames, glows) is
downloaded the first time any guest sees it.

Hull-piece gibs are typically seen for the first time at the moment of the
explosion. Their resources queue behind the view tiles that guest still
lacks (`FillResources`: urgent, then current state, then the rest of the
map). They share the roughly quarter-of-budget left after the snapshot. On
top of that, the obsolete sweep in `SendCapture` removes queued resources not
referenced by the *fitted* snapshot. A gib thinned out by §1.1 therefore also
loses its queued sprite. Gibs live a few seconds, so many are gone before
their image arrives.

## 2. Goals and non-goals

Goals:
- Guests see craft and actor gibs, with the same pieces, motion and lifetime as the host, within one presentation delay of the host.
- Object sprites the guest already has in its own game data are never downloaded.
- Selection under budget pressure is stable from snapshot to snapshot.
- No regression in snapshot cadence, update gaps, bandwidth or prediction.

Non-goals:
- Every explosion particle at 3 Mbps. Fire, smoke and sparks may still be thinned, as long as rigid debris survives.
- Guest-side simulation of debris. Debris remains host-authoritative presentation.

## 3. Design

No change to the wire format or protocol version is required. Resource
identities are content hashes, so a guest that builds an identical resource
locally holds exactly what the host would have sent.

### 3.1 Prime object sprites from local game data

When a guest joins a match (alongside the existing `PrimeScene` call on the
Lobby announcing the match), build a **sprite index**: content hash → source
bitmap, for every sprite frame of loaded presets.

- Sources:
  - Every `MOSprite`-derived preset's `m_aSprite` frames, which covers `MOSRotating`, `MOSParticle`, attachables, gib presets and craft.
  - `Arm` hand sprites.
  - The post-effect glow bitmaps that `EndView` captures through `StaticAsset`.
  - Enumerate through `PresetMan::GetAllOfType`.
- Store only the hash and an unowned `BITMAP*`. The bitmaps already live in the ContentFile cache, so the index costs about 16 bytes per frame, not a pixel copy. Pixels are copied into a `Resource` only on demand, so the 128 MB resource cache bound still holds.
- Compute it once per process and data set, and keep it between matches like `LocalTiles`. Hash on a background-pool task so joining a match is not delayed. Snapshots that arrive before it finishes fall back to download as today.
- Hash exactly as the host does: `ResourceHash` over the bitmap's full width, height, depth and rows, matching `MultiplayerWorld::Impl::Asset` with no sub-region. A sprite modified at runtime (recoloured, generated) simply does not match and downloads as today.

When a snapshot or the scene manifest references an asset the guest does not
hold, look it up in the index first. If found, install the resource locally
and report it as held through the existing batched `WorldAck` type 4 report,
so the host never queues it. `ReceiveWorld` already reports held assets per
snapshot; extend that path to materialize from the index before computing
`Missing`.

Expected effect: almost no object-sprite downloads for guests running the
same data. Bandwidth moves to snapshots and terrain changes, and gib, weapon
and effect sprites draw on first appearance.

### 3.2 Rank and stabilize optional nodes

At capture time, classify each optional object node into a tier. Store it in
a capture-only shared set beside `CriticalNodes`; it is not sent on the wire.

| Tier | Contents | Rationale |
| --- | --- | --- |
| 0 | Critical, screen-space, layers (unchanged) | Always sent |
| 1 | Rigid debris: sprite nodes whose root is an `MOSRotating` that is not an actor or device (craft and actor gibs, dropped items) | Few, large, long-lived, meaningful |
| 2 | Raster trails (`PixelPath`) | Existing priority |
| 3 | Other sprites: `MOSParticle` frames such as fire, smoke and flashes, and additive glows | Many, short-lived |
| 4 | Single pixels (`MOPixel`) and discontinuous effects | Most numerous, least individually visible |

`FitSnapshot` fills tiers in order. Within the tier that does not fit
completely, keep nodes by a **stable key** instead of index stride. Keep a
node when `hash(node.ID) mod 1024 < threshold`, with the threshold chosen to
fit. The same objects then stay selected while the budget is steady, and
selection changes gradually as it moves. Painter order of the kept nodes is
unchanged.

Rigid debris that alone exceeds the budget is thinned by the same stable key.
It must never displace tier 0.

### 3.3 Keep downloads alive across thinning

The obsolete-resource sweep in `SendCapture` must treat as needed every asset
referenced by the **unfitted** capture of the last second, not only by the
fitted snapshot. Keep a small ring of the per-capture asset sets, about 20
captures, and union them. With §3.1 in place this mainly affects guests
whose data differs, and runtime-generated sprites.

## 4. Measurement before the change

Add capture and receive counters, written to the existing benchmark CSVs only
when `CCCP_MPBENCHMARK` is set.

Host, per capture and per guest:
- optional nodes by tier before and after fitting;
- the number of tier-1 (rigid debris) nodes captured and sent;
- resources removed by the obsolete sweep.

Guest, per snapshot:
- object nodes received;
- object nodes whose asset is not held;
- of those, how many were satisfied from the local sprite index (after §3.1);
- for each newly seen object asset, the time from first reference to first draw.

Fixture: extend the hosted combat stage (`HostedSmokeTick`, stage 3) to
destroy a `Dropship MK1` near each player once, in the window where it
already detonates grenades. Spawn it through the simulation-safe path; the
fixtures already wait for Lua collection. The player-hosted `-ExplosionBurst`
fixture (twelve grenades and two dropships, four waves) is the second
fixture.

Record a baseline with the current build in both `RunLocalMultiplayerBenchmark.ps1`
and `-Wan` before implementing §3, so each part's effect is attributable.

## 5. Acceptance

Measured in the four-client hosted benchmark, stock and `-Wan` (3 Mbps,
60 ms delay, 20 ms jitter, 3% loss), with every guest running the same data:

1. **Debris delivered.** For each destroyed craft, at least 90% of its rigid
   gib nodes within the guest's view are present in every snapshot while
   visible. A kept gib appears in at least 95% of consecutive snapshots until
   it leaves the view or expires.
2. **No first-sight downloads.** At least 99% of object-sprite references are
   satisfied locally or already held. Any newly seen object asset is drawn
   within one presentation delay plus 50 ms of its first reference.
3. **Visual check.** Native guest captures taken during each craft
   destruction show its hull pieces. Add a screenshot capture to the fixture,
   as the radial-menu checks do.
4. **No regressions** against the same build without §3:
   - median world Hz and p95 update gap within 5%;
   - snapshot bytes within budget;
   - black terrain tiles unchanged;
   - prediction coverage and corrections unchanged;
   - every lifecycle check still passes.
5. **Fallback still works.** With the guest's sprite index disabled (a test
   switch), debris sprites still arrive by download, and §3.3 keeps them
   queued.

Unit tests (`Tests/MultiplayerTests.cpp`):
- `FitSnapshot` keeps every tier-1 node before any tier-3 or tier-4 node.
- Stable selection keeps the same IDs across two snapshots with the same budget and slightly different particle sets.
- The native presentation check (`VerifyWorldFlow`) installs an object sprite from the local index and reports it held without a resource message.

## 6. Risks

- **Index build time and memory.** Expect tens of thousands of frames across
  the bundled modules. Measure the build time on the background task and the
  index size, and report both in the benchmark log. If hashing every module
  is too slow, index the presets of the activity's factions first.
- **Runtime-altered sprites.** Content addressing makes a mismatch safe, as
  it falls back to download, but those sprites gain nothing. Count them via
  the "not held after index lookup" counter.
- **Tier classification cost.** Classification is a pointer check on the root
  object during capture; keep it out of the per-pixel trail path.
- **Mods.** A guest whose mods differ from the host's gets partial index hits
  and downloads the rest. Behaviour is unchanged from today.

## 7. Files

- `Source/Managers/MultiplayerWorld.cpp/.h`: sprite index build and lookup, tier classification at capture, guest counters.
- `Source/System/MultiplayerWorldProtocol.h`: tiered, stable `FitSnapshot`.
- `Source/Managers/MultiplayerMan.cpp`:
  - index-first resolution in `ReceiveWorld`;
  - the multi-capture needed set for the obsolete sweep;
  - host counters;
  - the hosted dropship fixture.
- `Tests/MultiplayerTests.cpp` and `Tests/AnalyzeMultiplayerBenchmark.py`: tests and new metrics.
- `Documentation/Multiplayer.md`: describe local sprite priming and debris priority once accepted.
