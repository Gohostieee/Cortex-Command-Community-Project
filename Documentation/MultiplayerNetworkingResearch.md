# Multiplayer networking research: replicating a physics-heavy game

Date: 2026-10-09. Source baseline: `f1582e24d` (the multiplayer source is unchanged in the working tree). This document extends the [client authority audit](MultiplayerClientAuthorityAudit.md). It does not repeat that audit's UI, command-protocol, loading or authority analysis. It adds four things: how shipped games solve the same problems, a replication-model decision, physics- and server-specific strategies, and one ordered build-up plan. No code, build or game was changed or run for this report.

## Evidence levels

Each claim carries one tag. The tags are not repeated as caveats in the prose.

| Tag | Meaning |
| --- | --- |
| **[M]** | Measured: a recorded result in this repository ([benchmark](MultiplayerBenchmark.md), [Multiplayer.md](Multiplayer.md), `build-mp/aws-benchmark/result.json`). |
| **[S]** | Read in source at the baseline commit, with a line link. |
| **[D]** | Derived by arithmetic from [M] and [S] facts. The working is shown. |
| **[P]** | Primary external source: official docs, engine source, or a talk, paper or blog by the people who built the system. |
| **[P\*]** | Primary content read through a mirror or a search excerpt, because the original was blocked or video-only. |
| **[H]** | Hypothesis. It is plausible from source, but its size has not been measured. |

## Executive summary

**Verdict.** The user's "not an efficient playable MP" has two causes, and both are visible in the evidence.

1. **The server's single main thread is overloaded, and replication work runs on that same thread.** In the 128-AI case the simulation ran at 37.8% of real time. The process used about 2 vCPUs at p95 on a 4-vCPU host, and the host's p95 load was 50.7% [M]. The machine had spare cores; the serial main path did not. The engine slows the game clock rather than drop work, and every millisecond of capture, compression and sending on that thread is a millisecond of lost simulation time. [S] [TimerMan.cpp#L84](../Source/Managers/TimerMan.cpp#L84), [Main.cpp#L425](../Source/Main.cpp#L425).
2. **The wire format replicates draw calls rather than game state.** Every visible particle becomes a 117-byte drawing node in every guest's complete snapshot. A 3 Mbps link is then too small, even though Halo: Reach ran 16-player matches sending at most 45 kbit/s to any one client [P\* Reach]. [S] [MultiplayerWorldProtocol.h#L91](../Source/System/MultiplayerWorldProtocol.h#L91), [MultiplayerWorld.cpp#L444](../Source/Managers/MultiplayerWorld.cpp#L444).

**Recommended replication model.** Keep the server authoritative and use state synchronization, which is what Source, Quake 3, Tribes, Halo and Teardown do. Change what is synchronized:

- state deltas against an acknowledged baseline, quantized, scoped by relevance and scheduled by priority;
- cosmetic particles as compact events that clients simulate locally;
- destructible terrain as versioned tile deltas;
- later, prediction of the controlled actor only.

Do **not** move to deterministic lockstep. The source has at least eight independent determinism blockers, including MSVC `/fp:fast` clients against a GCC Linux server, standard-library distributions that differ between compilers, a simulation random generator that HUD drawing also consumes, a Lua state count that depends on the core count, and Lua real-time timers. See [§3](#3-replication-model-decision).

**Do first (all small; see the [quick-wins table](#6-quick-wins)):**

- instrument the server main loop;
- schedule snapshots on simulation ticks and stagger them across guests;
- stop drawing the scene-wide object colour layer on the headless server;
- stop the send pump from losing bandwidth credit when the loop slows;
- cut per-guest scene-composition cost.

Then follow the [build-up plan](#7-one-ordered-build-up-plan). Its order differs from the audit's: server-loop and replication work come **before** most UI migration. The reasons are in [§7.2](#72-reconciliation-with-the-audits-work-packages).

## Status on 2026-10-10

Implemented on `codex/multiplayer-rebuild` (protocol v9), measured with `Tests/RunLocalMultiplayerBenchmark.ps1` on one computer; see [Multiplayer.md](Multiplayer.md#verification). The AWS service has not been redeployed with these changes.

| Plan step (§7.1) | Status |
| --- | --- |
| 1 Measure | Per-stage server timing CSV and client traces. No Tracy procedure yet. |
| 2 Loop and cadence fixes | Done: quick wins 2, 3, 4, 6, 7 and 8, plus catch-up simulation steps and a RakNet window floor with a delay-based send rate. |
| 3 Cheap capture | Done: write-marked terrain and fog tiles with a verification sweep. |
| 4 Encoding off the main thread | Partly: per-guest encoding runs in parallel; composition stays on the main thread. Snapshots drop to 15 Hz under overload. |
| 5 Delta-compressed snapshots | Done, against the acknowledged baseline with a full-state fallback. |
| 6 Relevance and priority | Relevance scoping done; no priority accumulator. Resources use a priority-ordered short backlog. |
| 7, 8 Particle roles, AI level of detail | Not started. At 64 AI, actor update and actor/particle travel dominate the simulation step, ahead of AI scripts. |
| 9–11 Semantic state, prediction, local weapon feedback | Not started. As an interim, the guest's own actor is shown at the host's present time instead of behind the interpolation buffer. |
| U Parallel UI track | Lobby pending edits (WP1) done. Landing zone, inventory cursor and pointer hit testing are local; the shop and pie menus are not. |
| Audit §15 content agreement | Guests build pristine terrain from local game data; matching tiles are not downloaded. |

## 1. Where the server's time goes

### 1.1 One loop iteration on the dedicated server

| Stage (in order) | Thread | Scales with | Source |
| --- | --- | --- | --- |
| Receive packets, lobby, resource queueing (LZ4 per resource), **send pump for every guest**, audio | main | guests, resources | [Main.cpp#L350](../Source/Main.cpp#L350), [MultiplayerMan.cpp#L1065](../Source/Managers/MultiplayerMan.cpp#L1065) |
| Apply remote input; server-side native menus (Buy, Inventory, Pie, Editor) | main | guests | [Main.cpp#L367](../Source/Main.cpp#L367) |
| Wait for the previous step's asynchronous MOID-layer redraw | main waits | all MOs | [Main.cpp#L372](../Source/Main.cpp#L372), [MovableMan.cpp#L1685](../Source/Managers/MovableMan.cpp#L1685) |
| Activity and scene update, including path-cost refresh | main | activity, terrain changes | [Scene.cpp#L2384](../Source/Entities/Scene.cpp#L2384) |
| **Travel**: actors, then items, then particles, with atom collision against terrain and the MOID layer | main, serial | particles × speed | [MovableMan.cpp#L1704](../Source/Managers/MovableMan.cpp#L1704) |
| Controllers; `ThreadedUpdateAI` (base `HumanAI.lua`); then serial `UpdateAI` | parallel per Lua state, then main | actors | [MovableMan.cpp#L1757](../Source/Managers/MovableMan.cpp#L1757) |
| `ThreadedUpdate` scripts (parallel), then `SyncedUpdate` scripts (serial) | mixed | scripted MOs | [MovableMan.cpp#L1341](../Source/Managers/MovableMan.cpp#L1341) |
| Actor, item and particle `Update` plus per-object `Update` scripts, rest detection | main, serial | all MOs | [MovableMan.cpp#L1401](../Source/Managers/MovableMan.cpp#L1401) |
| Add, remove and settle particles into terrain | main | particles | [MovableMan.cpp#L1641](../Source/Managers/MovableMan.cpp#L1641) |
| See-rays and Lua garbage collection | async | actors | [MovableMan.cpp#L1673](../Source/Managers/MovableMan.cpp#L1673) |
| **Draw every MO into the scene-sized colour layer, which also fires the capture hooks** | main | all MOs | [MovableMan.cpp#L1692](../Source/Managers/MovableMan.cpp#L1692), [MovableMan.cpp#L1922](../Source/Managers/MovableMan.cpp#L1922) |
| Headless draw when any guest is due: draw every MO **again** to collect nodes | main | all MOs | [FrameMan.cpp#L1008](../Source/Managers/FrameMan.cpp#L1008) |
| Per due guest: HUD, activity GUI, primitives → compose layers (memcmp of every visible terrain tile and the whole fog layer) → `FitSnapshot` (serialize and LZ4, up to 8 passes) → chunk and parity | main | guests × (tiles + nodes) | [FrameMan.cpp#L1010](../Source/Managers/FrameMan.cpp#L1010), [MultiplayerWorld.cpp#L126](../Source/Managers/MultiplayerWorld.cpp#L126), [MultiplayerWorldProtocol.h#L59](../Source/System/MultiplayerWorldProtocol.h#L59) |
| `SDL_Delay(1)`, unconditionally | main sleeps | none | [Main.cpp#L428](../Source/Main.cpp#L428) |

### 1.2 Findings

| # | Finding | Evidence |
| --- | --- | --- |
| F1 | **Overload becomes time dilation.** Each loop adds at most one 16.67 ms step of real time ([TimerMan.cpp#L84](../Source/Managers/TimerMan.cpp#L84)). The accumulator is also capped by the measured gap between steps, and that gap includes capture and networking ([TimerMan.cpp#L93](../Source/Managers/TimerMan.cpp#L93), [PerformanceMan.h#L120](../Source/Managers/PerformanceMan.h#L120)). Simulation speed ≈ 16.67 ms ÷ loop period. This is EVE Online's time dilation [P TiDi], with the same benefit (no dropped work) and the same cost (the whole match slows). | [S] |
| F2 | Implied average loop period: 16.7 ms (stock), **18.2 ms** (24 AI, 91.6%), 26.5–27.6 ms (64 AI), **44.2 ms** (128 AI with bursts, 37.75%). | [D] from [M] using F1 |
| F3 | **The server is bound by its main thread, not its cores.** At 128 AI the process p95 was 206% of one vCPU, and the 4-vCPU host p95 was 50.7%. | [M] `result.json` |
| F4 | **Per-guest capture is expensive.** Earlier local measurements recorded 6.7 ms mean (10.2 ms p95) per guest update, about 5 ms of it in scene composition, and 15.4 ms mean (23.2 ms p95) at 1920 × 1080. An AWS room has 4 remote guests at about 17 Hz, so 6.7 ms each would use about 456 ms of main thread per second. | [M] [Multiplayer.md#L111](Multiplayer.md#L111), [#L113](Multiplayer.md#L113); [D] 4 × 17 × 6.7 ms. AWS cost [H]. |
| F5 | **Objects are drawn up to twice per loop on the headless server.** The colour-layer draw on every drawn step also triggers object capture. Nothing on a dedicated server displays that layer. | [S] [MovableMan.cpp#L1285](../Source/Managers/MovableMan.cpp#L1285), [#L1692](../Source/Managers/MovableMan.cpp#L1692), [SceneMan.cpp#L2702](../Source/Managers/SceneMan.cpp#L2702). Cost [H]. |
| F6 | **A nominal 20 Hz runs at about 17 Hz at full speed.** Eligibility re-arms from the capture time (`Now() - LastWorld >= 50`), and captures only happen on 16.67 ms step boundaries, so gaps alternate between 50 and 66.7 ms. That matches the measured 16–17 Hz. | [S] [MultiplayerMan.cpp#L584](../Source/Managers/MultiplayerMan.cpp#L584), [#L606](../Source/Managers/MultiplayerMan.cpp#L606); [D]; [M] |
| F7 | **Under load, freshness collapses.** At a 44 ms loop, a 50 ms rule allows at most about 11 Hz before any bandwidth limit. The send pump runs once per loop with credit capped at 20 ms. At 3 Mbps that is 6,375 B per loop, or about 1.16 Mbps actually sent at a 44 ms loop. A snapshot that has started sending blocks its replacement. The measured result was about 5 Hz. | [S] [MultiplayerMan.cpp#L1075](../Source/Managers/MultiplayerMan.cpp#L1075), [#L704](../Source/Managers/MultiplayerMan.cpp#L704); [D] 6,375 B ÷ 44 ms; [M] |
| F8 | **The client buffer is too short for the real cadence.** The 75 ms interpolation delay covers about 1.3 real snapshot intervals; Source defaults to 100 ms, two intervals at 20 Hz. The measured p95 gaps of 102–119 ms therefore spend time in extrapolation. | [S] [MultiplayerWorldProtocol.h#L17](../Source/System/MultiplayerWorldProtocol.h#L17); [P\* Source networking]; [M] |
| F9 | **Profiling hooks exist but are not used on the server.** `PerformanceMan` has per-stage counters (AI, travel, update, scripts), and Tracy zones mark most simulation stages. The Linux dedicated build compiles Tracy in on-demand mode. Capture timing exists only in the smoke-test encounter path ([MultiplayerMan.cpp#L676](../Source/Managers/MultiplayerMan.cpp#L676)); there is no capture, compose or send counter for normal matches. | [S] [PerformanceMan.h#L25](../Source/Managers/PerformanceMan.h#L25), [Build.sh#L64](../Services/DedicatedServer/Build.sh#L64), [meson_options.txt#L15](../meson_options.txt#L15) |

The simulation itself is still heavy. It grows with particles (8,489 peak and 9,581 MOIDs in the 128-AI case [M]), and particle travel, MO updates and per-object scripts are serial. But F4–F7 show that **presentation and transport work on the simulation thread** is a first-order cost that can be removed without touching gameplay. The audit's §16 asks for this measurement. §1 says where to look first.

## 2. Industry techniques mapped to Cortex Command

### 2.1 Representation and bandwidth

| Technique | Who uses it | How it applies to CC | What changes in this codebase |
| --- | --- | --- | --- |
| **Replicate entity state, not drawing** | Source sends networked entity fields through SendTables [P src-dt]. Tribes ghosts objects with per-class state masks [P\* Tribes]. Halo: Reach splits state, events and control data [P\* Reach]. | CC sends 117-byte drawing nodes: three 64-bit IDs, two 64-bit times and fifteen floats ([MultiplayerWorldProtocol.h#L132](../Source/System/MultiplayerWorldProtocol.h#L132)). An `MOPixel` needs about three values. | Keep node IDs (unique ID << 8 \| ordinal, [MultiplayerWorld.cpp#L170](../Source/Managers/MultiplayerWorld.cpp#L170)) and content-hashed asset IDs, but send **changed fields only** (step 5 of the plan). Add semantic actor records later (step 9). |
| **Delta against the last acknowledged snapshot** | Quake 3 picks the client's acknowledged frame as baseline and falls back to a full snapshot when it is too old [P q3-snap]. Source sends full snapshots only at start or after heavy loss [P\* Source networking]. | CC sends a complete snapshot every time, which self-repairs but repeats unchanged state. Guests already ACK snapshot IDs ([MultiplayerMan.cpp#L803](../Source/Managers/MultiplayerMan.cpp#L803)). | Keep a per-guest ring of sent snapshots, delta against the acknowledged one, and send a full snapshot when the baseline expires. This keeps the audit's §14 requirement of loss recovery. |
| **Track changes at write time** | `CNetworkVar` marks the entity changed only when a value really changes [P src-nv]. Tribes sets state-mask bits [P\* Tribes]. | CC finds terrain changes by memcmp of each visible tile, per guest, per snapshot ([MultiplayerWorld.cpp#L120](../Source/Managers/MultiplayerWorld.cpp#L120)). | Mark dirty tiles at terrain write sites; the legacy hooks show where (§4.2). Mark entity field changes during capture. |
| **Quantize** | `SendProp` takes bit counts and ranges [P src-dt]. Gaffer cut 901 cubes from 17.37 Mbps to about 256 kbps with smallest-three quaternions, bounded positions, deltas and at-rest flags [P gaffer-compress]. | Positions, angles and sizes are all 32-bit floats. | 1/8 px positions within the scene bounds (17 bits absolute, about 10 as a delta), 12-bit angles, 8-bit frames, and preset indices from a per-match table. |
| **Relevance scoping** | Quake 3 filters entities by PVS clusters and areas [P q3-snap]. Tribes scopes ghosts per client [P\* Tribes]. GMod NW2 values go only to clients in the entity's PVS [P gmod-net]. | All objects are appended to every guest ([MultiplayerWorld.cpp#L444](../Source/Managers/MultiplayerWorld.cpp#L444)). The audit's §14 already flags this. | Per-guest scope: view plus margin, the controlled actor, the brain, team assets and "always" objects. 2D side-on terrain has no PVS, so use a view box with a motion margin. |
| **Priority accumulator and most-recent-state** | Tribes resends only the latest state for lost bits [P\* Tribes]. Halo prioritizes per object per client by distance with size, speed and damage modifiers [P\* Reach]. Fiedler's accumulator lets low-priority objects still bubble up [P gaffer-sync]. Teardown uses per-client priority queues in about 1 Mbit/s per client [P teardown]. | Under budget, `FitSnapshot` evenly samples optional nodes and keeps actor roots ([MultiplayerWorldProtocol.h#L77](../Source/System/MultiplayerWorldProtocol.h#L77)). | Replace sampling with an accumulator: own actor > nearby actors > projectiles > debris > far objects. Send critical state every snapshot; let the rest rotate. |
| **Entity limits** | Source networks at most 2,048 edicts (11 bits) [P src-const]. GMod raises this to about 8,192 [P gmod-ents]. | CC peaks above 9,500 MOIDs [M]. | Shipped engines never network particle swarms as entities. Neither should CC (§4.1). |

### 2.2 Particles and destruction

| Technique | Who uses it | How it applies to CC | What changes in this codebase |
| --- | --- | --- | --- |
| **Transient effects as events** | Source breaks props with a temp-entity event to an audibility-filtered set of clients [P src-props]. GMod `util.Effect` effects are not entities [P gmod-effect]. Halo has a separate unreliable event channel [P\* Reach]. | Smoke, sparks, blood, flashes and dirt sprays are full nodes today. | Add a "spawn burst" event (emitter or preset, position, velocity, count, seed), and let clients spawn local cosmetic particles. |
| **Client-only cosmetic physics** | Source `C_PhysPropClientside` (capped by `cl_phys_props_max`, default 300) and `PHYSICS_MULTIPLAYER_CLIENTSIDE` gibs [P src-cprops, src-propsh]. GMod debris collision groups collide only with the world [P gmod-cg]. | Terrain debris is already created without MO collision ([SceneMan.cpp#L635](../Source/Managers/SceneMan.cpp#L635)). Most `MOPixel` presets do not set `HitsMOs`. | Classify particle roles and simulate the cosmetic role only on clients (§4.1). |
| **Deterministic commands for destruction, state sync for bodies** | Teardown sends fixed-point voxel-cut commands on a reliable ordered stream and syncs dynamic bodies by server state [P teardown]. | CC terrain damage is emergent: every particle hit runs `TryPenetrate` with float impulses and `RandomNum` ([SceneMan.cpp#L570](../Source/Managers/SceneMan.cpp#L570), [#L644](../Source/Managers/SceneMan.cpp#L644)). Clients cannot reproduce it. | Replicate terrain as **versioned tile state**, not commands (§4.2). Commands remain an option for explicit operations such as placing a terrain object. |

### 2.3 Responsiveness

| Technique | Who uses it | How it applies to CC | What changes in this codebase |
| --- | --- | --- | --- |
| **Shared movement code and prediction with replay** | Source runs `gamemovement.cpp` on both sides with swept-box collision, not VPhysics [P src-gm]. The player's VPhysics shadow follows game movement [P src-player]. GMod `IsFirstTimePredicted` stops effects repeating during re-prediction [P gmod-iftp]. | AHuman movement uses limb paths, atom groups and terrain material. Jetpack thrust comes from **randomized emission impulses** ([AEmitter.cpp#L494](../Source/Entities/AEmitter.cpp#L494)–[#L513](../Source/Entities/AEmitter.cpp#L513)). | Prediction needs the material layer on the client, a movement path free of rendering side effects, and **tolerant** reconciliation with smoothing, not exact replay (§4.3). |
| **Predict, then rewind and resimulate on correction** | Rocket League records input and physics history per frame, rewinds every physics actor to the corrected frame and resimulates; 200 ms at 120 Hz is 24 frames [P rl]. Unreal "Resimulation" mode does the same at high CPU and memory cost [P ue-phys]. | Full-world rewind is infeasible for thousands of CC MOs. | Rewind only the controlled actor (Unreal's guidance: resimulate pawns, interpolate the rest). |
| **Server input buffer and adaptive client rate** | Rocket League buffers input on the server and tells clients to run faster or slower to keep it filled, crediting Overwatch's GDC 2017 talk [P rl], [P\* ow]. | CC merges whatever input has arrived at each step ([MultiplayerProtocol.h#L135](../Source/System/MultiplayerProtocol.h#L135)). Input is not tied to a tick. | Number inputs by client tick, buffer them on the server, and put the server tick in snapshots (step 9). |
| **Lag compensation** | Source rewinds **players only**, up to `sv_maxunlag` = 1 s [P src-lc]. Rocket League chose no server-side lag compensation [P rl]. | CC weapons fire physical particles, not hitscan traces ([HDFirearm.cpp#L797](../Source/Entities/HDFirearm.cpp#L797)). | No rewind. Give cosmetic local fire feedback only (the audit's §13 already proposes this). |
| **Client ownership of nearby physics** | Roblox hands nearby unanchored parts to a client; the server cannot verify them [P roblox]. Fiedler's GDC 2010 talk uses interaction authority for co-op [P fiedler10]. | It removes server cost and latency, but trusts clients. | Not recommended for gameplay objects. It is acceptable only for the cosmetic role. |
| **Latency state** | Factorio clients keep a separate approximate state for their own pending actions and avoid it for combat [P fff83, fff302]. | This is the model behind the audit's local UI drafts. | Already covered by the audit's §10. |

### 2.4 Server scaling

| Technique | Who uses it | How it applies to CC | What changes in this codebase |
| --- | --- | --- | --- |
| **Sleep and rest states; per-step budgets** | GMod/VPhysics sleep idle objects. `MaxCollisionsPerObjectPerTimestep` is 10 and `MaxCollisionChecksPerTimestep` is 50,000 [P gmod-sleep, gmod-perf]. | CC already settles resting particles into terrain and dropped items beyond 100 ([MovableMan.cpp#L1451](../Source/Managers/MovableMan.cpp#L1451), [#L75](../Source/Managers/MovableMan.cpp#L75)). | Add a server particle budget and skip cosmetic particles on the server (§5). |
| **AI level of detail** | Halo gives distant objects lower priority [P\* Reach]. | `AIUpdateInterval` (default 2) already staggers AI ticks ([Controller.cpp#L196](../Source/System/Controller.cpp#L196), [SettingsMan.cpp#L51](../Source/Managers/SettingsMan.cpp#L51)). | Scale the interval by distance from every human player's view. |
| **Time dilation** | EVE Online [P TiDi]. | CC already does this (F1). | Keep it, report simulation speed to clients, and alert when it drops. |

## 3. Replication model decision

### 3.1 Options

| Model | Bandwidth | Server CPU | Latency feel | Fit for CC |
| --- | --- | --- | --- | --- |
| **A. Current drawing-node streaming** (complete per-guest snapshots of draw calls) | High, and grows with visible particles | High: capture and compression per guest on the main thread (F4) | Remote objects 75 ms behind; own actor not predicted | Works, and preserves arbitrary visuals, but does not scale (§1). |
| **B. Server-authoritative state sync**: delta + relevance + priority + events, then own-actor prediction | Bounded by budget; cosmetics move to events | Capture becomes encoding of changed state, and can run off the main thread | Same as A at first; prediction later gives immediate own movement | **Recommended.** Shipped practice in Source, Quake 3, Tribes, Halo and Teardown [P]. |
| **C. Deterministic lockstep** (input-only; every peer simulates) | Smallest: inputs only, whatever the particle count [P gaffer-lockstep] | Every client simulates the whole world | Input delay at least the slowest peer's RTT, or rollback [P aoe, ggpo] | **Rejected.** Blockers in §3.2. |
| **D. Hybrids** | — | — | — | Use two hybrid pieces inside B: (1) Teardown-style determinism only where divergence is harmless (seeded cosmetic effects), and (2) Factorio-style local latency state for UI (the audit). Reject Roblox-style client ownership of gameplay physics. |

### 3.2 Why lockstep does not fit this codebase

The particle count is the strongest argument *for* lockstep, since sending only input makes bandwidth independent of object count [P gaffer-lockstep]. These source facts each break bit-identical simulation:

| # | Blocker | Source |
| --- | --- | --- |
| 1 | Windows clients build with MSVC `/fp:fast` (Final x64); the Linux server builds with GCC through Meson. Cross-compiler floating-point reproducibility is impractical [P gaffer-fp]. | [RTEA.vcxproj#L699](../RTEA.vcxproj#L699), [Build.sh#L64](../Services/DedicatedServer/Build.sh#L64) |
| 2 | `std::uniform_real_distribution` and the integer distribution are not portable across standard libraries even with a seeded `mt19937` [P p2059]. | [RTETools.h#L44](../Source/System/RTETools.h#L44) |
| 3 | HUD drawing consumes the **simulation** RNG, so the sequence depends on how many guests are captured. | [HDFirearm.cpp#L1048](../Source/Entities/HDFirearm.cpp#L1048), [AHuman.cpp#L2673](../Source/Entities/AHuman.cpp#L2673) via [#L2764](../Source/Entities/AHuman.cpp#L2764) |
| 4 | There is one Lua state per hardware thread. Objects are assigned to states round-robin, and each state's RNG is seeded from the global generator. Script random streams therefore depend on the core count and creation order. | [LuaMan.cpp#L326](../Source/Managers/LuaMan.cpp#L326), [#L379](../Source/Managers/LuaMan.cpp#L379), [#L245](../Source/Managers/LuaMan.cpp#L245) |
| 5 | AI scripts run in parallel. Pathfinding completes asynchronously on a background pool, and path-cost refresh waits until no requests are outstanding. Both make timing-dependent results. | [MovableMan.cpp#L1775](../Source/Managers/MovableMan.cpp#L1775), [PathFinder.cpp#L236](../Source/System/PathFinder.cpp#L236), [Scene.cpp#L2384](../Source/Entities/Scene.cpp#L2384) |
| 6 | Lua can read wall-clock timers; 7 base scripts do. `io`, `os` and `ffi` are loaded. | [LuaBindingsSystem.cpp#L144](../Source/Lua/LuaBindingsSystem.cpp#L144), [LuaMan.cpp#L49](../Source/Managers/LuaMan.cpp#L49) |
| 7 | Unique IDs come from a process-wide atomic counter that threads also use. | [MovableObject.h#L1146](../Source/Entities/MovableObject.h#L1146), [MovableObject.cpp#L152](../Source/Entities/MovableObject.cpp#L152) |
| 8 | Late join and desync recovery need full-state transfer (Factorio re-downloads the map [P fff188]). Per-object Lua state lives in `_ScriptedObjects` and is not saved; `Save` writes script paths only. | [MovableObject.cpp#L448](../Source/Entities/MovableObject.cpp#L448), [#L605](../Source/Entities/MovableObject.cpp#L605) |

Shipped lockstep games had to remove exactly these sources of divergence:

- Factorio replaced `math.random`, made `pairs` follow insertion order, removed `io` and `os`, and restricted `on_load` [P factorio-libs, factorio-lifecycle].
- Age of Empires called out-of-sync bugs the project's most stubborn bug class, and had to keep cosmetic randomness out of the simulation [P aoe].
- Rollback (GGPO) also needs full state saved and restored every frame [P ggpo], which is out of reach for thousands of MOs plus scene bitmaps.

Fixing items 1–8 would be an XL refactor across engine, Lua and content. It would still leave 4-player combat with either input delay or rollback.

### 3.3 Recommendation and trade-offs

**Model B.** The server stays authoritative for everything that changes the match, as the audit's §16 requires. Change the payload in this order:

1. remove presentation work from the simulation thread;
2. replicate state, not draw calls: deltas against acknowledged baselines, quantized, relevance-scoped and priority-scheduled;
3. turn cosmetic particles into events;
4. turn terrain into versioned tile deltas;
5. add semantic actor state, tick-indexed input and own-actor prediction.

The current drawing-node path remains the **compatibility channel** for HUD, script primitives and anything not yet modelled.

**Trade-offs accepted:**

- Cosmetic particles look different on each client (harmless by definition).
- A predicted own actor will sometimes correct visibly, especially under jetpack thrust (§4.3).
- Engineering moves from one generic capture path to per-class schemas.

## 4. Physics-specific strategies

### 4.1 Particles

| Role | Definition (from existing flags) | Server | Client |
| --- | --- | --- | --- |
| **Gameplay** | `HitsMOs` or `GetsHitByMOs`, scripted, nonzero collision or penetration damage, mission-critical, or any actor, item or device | Simulate; replicate as state, prioritized and scoped | Interpolate |
| **Debris** | Not MO-colliding and unscripted, but can dig or settle into terrain (most dislodged terrain pixels, [SceneMan.cpp#L625](../Source/Managers/SceneMan.cpp#L625)–[#L641](../Source/Managers/SceneMan.cpp#L641)) | Simulate; do not replicate per step; the outcome reaches clients as terrain tile deltas | Cosmetic flight from a spawn event; snap to the server's tile state |
| **Cosmetic** | Debris conditions, plus it cannot change terrain: ignores terrain, or is short-lived with settling disabled for it | **Do not simulate on the headless server** (an option, gated by an A/B outcome check) | Spawn from an event with a client seed |

Relevant facts:

- **The flags exist.** `HitsMOs` and `GetsHitByMOs` both default to false ([MovableObject.cpp#L58](../Source/Entities/MovableObject.cpp#L58)). There is also `IgnoreTerrain` ([MovableObject.h#L332](../Source/Entities/MovableObject.h#L332)) and script presence ([MovableObject.h#L110](../Source/Entities/MovableObject.h#L110)). [S]
- **Static scan of `Data/` INI top-level presets** (`CopyOf` inheritance not resolved): 121 of 173 `MOPixel` presets (70%) and 48 of 85 `MOSParticle` presets (56%) do not set `HitsMOs = 1`. Most particle kinds are candidates for the debris or cosmetic role. The runtime mix is unmeasured, so add a per-role counter. [S] [H]
- **Pitfalls.** A non-MO-colliding particle can still dig terrain through `TryPenetrate` if its impulse beats the material's integrity, and any resting particle settles into terrain ([MovableMan.cpp#L1641](../Source/Managers/MovableMan.cpp#L1641)). The classifier therefore needs an INI override (for example `NetworkRole`) and a validation run: same seed, same machine, server-side cosmetic culling on versus off, then compare damage, deaths and terrain hashes.
- **Events.** Emitters attached to replicated actors (jetpacks, muzzle flashes, wounds) can be driven on the client from replicated state ("emitting", throttle), the way Source clients render effects from entity state. Gibbing sends one event, and its gameplay-role gibs replicate as state. Keep the existing pixel-path trail for bullet trails ([MultiplayerWorldProtocol.h#L36](../Source/System/MultiplayerWorldProtocol.h#L36)).

### 4.2 Destructible terrain and fog

- **State, not commands.** Destruction is emergent and random (§2.2), so replicate results. Teardown's command stream works because its operations are explicit and fixed-point [P teardown]; CC's are not.
- **Dirty tiles at write sites.** The legacy netcode called `SceneMan::RegisterTerrainChange` from `DrawToTerrain`, MOPixel settling, `EraseSilhouette`, terrain-object placement and `TryPenetrate`. Upstream removed those hooks in commit `91fe85dd8` ("RIP Multiplayer 2019-2025"; see `git show 91fe85dd8`). Restore them as **dirty bits on 64-pixel tiles** with a revision counter per tile, not as per-pixel messages. This replaces the per-guest memcmp scan ([MultiplayerWorld.cpp#L126](../Source/Managers/MultiplayerWorld.cpp#L126)) and the full fog-layer scan on every snapshot ([MultiplayerWorld.cpp#L448](../Source/Managers/MultiplayerWorld.cpp#L448)). The existing `UpdatedMaterialAreas` list, used for path costs, records sprite settling, silhouette erasure, terrain objects and doors, but not single-pixel settling or penetration ([SLTerrain.h#L164](../Source/Entities/SLTerrain.h#L164), [MovableObject.cpp#L1117](../Source/Entities/MovableObject.cpp#L1117), [SLTerrain.cpp#L479](../Source/Entities/SLTerrain.cpp#L479)).
- **Wire.** Snapshots carry tile revisions for scoped tiles. Tile content travels on the existing reliable resource lane. Encode it as XOR against the guest's acknowledged revision, then LZ4: mostly zeros. Prioritize by distance from the guest's view. Tribes' rule applies: guarantee only the latest revision.
- **Material layer.** Clients render only colour today. Prediction (step 10) needs the material layer, but only near the controlled actor.
- **Reconnect and late join.** Send pristine scene content (already cached by hash) plus every tile whose revision is not zero. This is the same principle as the audit's §15.

### 4.3 Actors and AI

- **AI stays on the server.** Base AI runs in `ThreadedUpdateAI`, already parallel across Lua states ([MovableMan.cpp#L1775](../Source/Managers/MovableMan.cpp#L1775); `Data/Base.rte/AI/HumanAI.lua`). For level of detail, multiply `AIUpdateInterval` for actors far from every human player's view and brain. Actor IDs are contiguous, so the existing stagger keeps the load spread ([Controller.cpp#L204](../Source/System/Controller.cpp#L204)).
- **Replicated actor state, in two stages.**
  - Stage 1: delta-compressed attachable nodes. They already have stable IDs and parent-relative interpolation ([MultiplayerWorldProtocol.h#L567](../Source/System/MultiplayerWorldProtocol.h#L567)).
  - Stage 2 (prediction prerequisite): per-actor semantic records:
    - position, velocity, rotation and angular velocity;
    - status and health;
    - aim angle and controller state bits;
    - jetpack fuel and throttle;
    - held device ID;
    - the server tick and last input applied.
- **Prediction scope.** Predict the controlled `AHuman` or `ACrab` only.
  - **Error tolerance is mandatory.** Jetpack push comes from randomized particle velocities, so client and server thrust will differ every tick ([AEmitter.cpp#L494](../Source/Entities/AEmitter.cpp#L494)). Use the expected thrust when predicting, and Fiedler-style smoothing of small errors [P gaffer-sync].
  - **Rendering side effects.** Do not replay HUD drawing during prediction: it consumes RNG and registers glows (§3.2 #3). This is CC's version of `IsFirstTimePredicted`.
- **Clients never construct gameplay actors.** Cloning a preset loads and runs its script files in a Lua state; script functions then initialize lazily on first call. A predicted proxy needs a presentation-only construction path. [S] [MovableObject.cpp#L229](../Source/Entities/MovableObject.cpp#L229), [#L553](../Source/Entities/MovableObject.cpp#L553), [#L662](../Source/Entities/MovableObject.cpp#L662); feasibility [H], a spike is needed.

### 4.4 Lua

- **The server runs all gameplay Lua.** There is no client gameplay Lua. Script drawing continues through the compatibility channel. If client-side cosmetic scripting is ever wanted, Teardown's split of client and server parts inside one script file is a working precedent [P teardown]. Effort XL; not required.
- **Cost.** Per-object `Update` scripts and `SyncedUpdate` run serially on the main thread; `ThreadedUpdate` runs in parallel ([MovableMan.cpp#L1341](../Source/Managers/MovableMan.cpp#L1341)–[#L1411](../Source/Managers/MovableMan.cpp#L1411)). Script timings are collected ([Main.cpp#L383](../Source/Main.cpp#L383)) but not exported; add them to the benchmark trace.
- **Determinism is not required by Model B.** Fix §3.2 #3 anyway: it makes same-seed A/B comparisons possible, which step 7 needs.

## 5. Server simulation scaling plan

The plan is grounded in §1. Each item names its measurement.

| Order | Action | Expected effect | Proof |
| --- | --- | --- | --- |
| 1 | Add timers for each main-loop stage (`MultiplayerMan.Update`, simulation stages from the existing `PerformanceMan` counters, headless draw, per-guest GUI, compose, fit, compress, queue, sleep) to the benchmark trace. Run a Tracy session on an AWS worker over an SSH tunnel. | Attribution (audit §16) | Stages cover ≥ 95% of loop wall time in all four load cases |
| 2 | Remove presentation work from the simulation thread: no colour-layer draw on the headless server (F5); tick-scheduled, staggered snapshots (F6); a send pump that does not lose credit (F7). | Fewer ms per loop | Simulation-speed delta at 24 and 64 AI |
| 3 | Make capture cheap: dirty tiles (§4.2); one shared tile-ID pass per tick for all guests; a single compression pass in `FitSnapshot`. | Per-guest compose ≪ 5 ms | Per-stage trace |
| 4 | Move per-guest encoding to workers: copy the immutable per-tick inputs (object nodes, tile revisions, canvas list), then compose, fit and compress on the existing priority pool ([ThreadMan.cpp#L15](../Source/Managers/ThreadMan.cpp#L15)). GUI drawing stays on the main thread until the audit's UI migration removes it. | Main-thread capture ≤ 1 ms per guest | Per-stage trace |
| 5 | Reduce simulation work: server cosmetic culling (§4.1), a particle budget per scene, AI level of detail (§4.3), and per-worker settings (`NumberOfLuaStatesOverride`, `AIUpdateInterval`, [SettingsMan.cpp#L175](../Source/Managers/SettingsMan.cpp#L175)). | Fewer MOs, fewer AI ticks | Particle and AI counters; same-seed outcome A/B |
| 6 | Parallelize serial MO work only where the profile shows it. Particle travel reads the MOID layer and writes terrain, so it needs spatial partitions with deferred terrain writes. XL, high risk. | Uses idle cores (F3) | Only if items 2–5 leave travel above 50% of the step |
| 7 | Capacity: one room per `c7i.xlarge` until item 4 lands. Two rooms means two main threads, each with its own Lua and thread pools sized to every core ([LuaMan.cpp#L326](../Source/Managers/LuaMan.cpp#L326)). | Predictable capacity | Two-room speed equals one-room speed |

## 6. Quick wins

Each item is small and can ship on its own. Effort: **S** ≤ 1 engineer-week, **M** 1–3 weeks.

| # | Fix | Files | Effort | Expected result |
| --- | --- | --- | --- | --- |
| 1 | **Instrument the server loop.** Add per-stage timers to the benchmark trace and document a Tracy attach procedure for workers. | [Main.cpp#L341](../Source/Main.cpp#L341), [FrameMan.cpp#L996](../Source/Managers/FrameMan.cpp#L996), [MultiplayerMan.cpp#L605](../Source/Managers/MultiplayerMan.cpp#L605), [PerformanceMan.h#L25](../Source/Managers/PerformanceMan.h#L25) | S | Turns every other row from [H] into [M] |
| 2 | **Tick-scheduled, staggered snapshots.** Capture on every third simulation tick, offset per guest; or advance `LastWorld` by 50 instead of setting it to `Now()`. | [MultiplayerMan.cpp#L581](../Source/Managers/MultiplayerMan.cpp#L581)–[#L606](../Source/Managers/MultiplayerMan.cpp#L606) | S | 20 Hz instead of about 17 Hz at full speed [D]; capture cost spread evenly across ticks |
| 3 | **Skip the object colour layer on the headless server.** Keep `BeginWorldTrails`, which collects bullet trails. | [MovableMan.cpp#L1285](../Source/Managers/MovableMan.cpp#L1285), [#L1692](../Source/Managers/MovableMan.cpp#L1692) | S | One full MO draw and capture removed per step [S]; size [H] |
| 4 | **Do not lose send credit under load.** Allow credit of `max(20 ms, last loop period)`, and pump again after capture. | [MultiplayerMan.cpp#L1075](../Source/Managers/MultiplayerMan.cpp#L1075)–[#L1078](../Source/Managers/MultiplayerMan.cpp#L1078), [Main.cpp#L425](../Source/Main.cpp#L425) | S | Sent bandwidth tracks the configured rate (about 1.16 → 2.55 Mbps at a 44 ms loop) [D] |
| 5 | **Shared tile pass and single-pass fit.** Compute layer tile asset IDs once per tick for all guests, and size `FitSnapshot` from node counts and the last compression ratio. | [MultiplayerWorld.cpp#L111](../Source/Managers/MultiplayerWorld.cpp#L111), [#L271](../Source/Managers/MultiplayerWorld.cpp#L271), [MultiplayerWorldProtocol.h#L59](../Source/System/MultiplayerWorldProtocol.h#L59) | M | Lower composition cost (F4) [H] |
| 6 | **Do not sleep when behind.** Skip `SDL_Delay(1)` when a step is already due. | [Main.cpp#L428](../Source/Main.cpp#L428) | S | At least 1 ms per loop back under overload: about 2–6% simulation speed at 17–44 ms loops [D] |
| 7 | **Adaptive interpolation delay.** Use about 2 × the measured interval, or the p95 gap, instead of a fixed 75 ms. | [MultiplayerWorldProtocol.h#L17](../Source/System/MultiplayerWorldProtocol.h#L17), [#L529](../Source/System/MultiplayerWorldProtocol.h#L529) | S | Less extrapolation and freezing; remote objects show slightly later (Source's default trade-off [P\*]) |
| 8 | **Separate presentation RNG** for HUD glows and reticles. | [HDFirearm.cpp#L1048](../Source/Entities/HDFirearm.cpp#L1048), [AHuman.cpp#L2673](../Source/Entities/AHuman.cpp#L2673) | S | Server simulation no longer depends on guest count; enables same-seed A/B |
| 9 | **Put the server tick and simulation speed in the snapshot header** (protocol bump). | [MultiplayerWorldProtocol.h#L42](../Source/System/MultiplayerWorldProtocol.h#L42), [MultiplayerProtocol.h#L18](../Source/System/MultiplayerProtocol.h#L18) | S | Prerequisite for steps 9–10 of the plan; clients can show slow-motion state |
| 10 | **Per-worker settings.** Set Lua states to vCPUs ÷ rooms; evaluate `AIUpdateInterval` 3 under load, which changes AI reaction time. | [SettingsMan.cpp#L175](../Source/Managers/SettingsMan.cpp#L175), [#L176](../Source/Managers/SettingsMan.cpp#L176) | S | Less contention with two rooms per host [H] |
| 11 | **Lobby pending state** (the audit's WP1). | the audit's §1 | S–M | Removes lobby snapping |

## 7. One ordered build-up plan

Effort: S ≤ 1 week, M 1–3 weeks, L 1–2 months, XL > 2 months, for one engineer who knows the codebase. Run every acceptance metric with the existing harness (`Tests/RunMultiplayerBenchmark.ps1`, `Tests/AnalyzeMultiplayerBenchmark.py`) at the ordinary 3 Mbps setting, plus the audit's separate-PC and WAN conditions. The targets are proposals, not results.

### 7.1 Steps

| Step | Work | Effort | Acceptance metric |
| --- | --- | --- | --- |
| 1 | **Measure.** Quick win 1; record stock, 24, 64 and 128-AI traces with per-stage timing. | S | Stages account for ≥ 95% of loop time; Tracy capture archived |
| 2 | **Loop and cadence fixes.** Quick wins 2, 3, 4, 6, 7 and 8. | S–M | Stock at 3 Mbps: median fresh state ≥ 19 Hz, p95 gap ≤ 75 ms. 24 AI: simulation ≥ 97% of real time |
| 3 | **Cheap capture.** Quick win 5, dirty terrain and fog tiles (§4.2). | M | No per-guest tile scans; compose time independent of viewport tile count |
| 4 | **Encoding off the main thread.** Scaling item 4. | M–L | Main-thread capture ≤ 1 ms per guest at p95; 64 AI (one room) ≥ 90% of real time |
| 5 | **Delta-compressed, quantized snapshots** against an acknowledged baseline, with a full-snapshot fallback. | L | Bytes per snapshot ≤ 30% of today on stock and burst traces; no loss-recovery regressions |
| 6 | **Relevance scoping and a priority accumulator.** | M | In 128-AI bursts, own actor and on-screen actors in 100% of snapshots; median ≥ 15 Hz at 3 Mbps. The WAN case (60/20 ms, 3% loss) reaches a playable battlefield |
| 7 | **Particle roles, cosmetic events and server cosmetic culling.** | L | Burst-case server particle count and bytes ≥ 50% lower; same-seed A/B shows identical damage, deaths and terrain hash |
| 8 | **Ongoing server simulation scaling.** AI level of detail, particle budget, migrating scripts to threaded hooks; parallel travel only if justified. | L, XL for parallel travel | One room: 64 AI ≥ 95% of real time; 128 AI with bursts ≥ 75% |
| 9 | **Semantic actor state, tick-indexed input and a server input buffer.** | L | Under 20 ms jitter, < 2% of server ticks run without that player's input; snapshots report server tick and last applied input |
| 10 | **Controlled-actor prediction and reconciliation** (audit §13), including the material layer near the actor. | XL | Own movement responds on the next client frame; correction p95 < 4 px at 120 ms RTT in walk, jump and jetpack fixtures |
| 11 | **Local weapon feedback** with server-effect deduplication (audit §13). | M | No duplicate muzzle flash or sound; feedback ≤ 1 frame |
| U | **Parallel UI track:** the audit's WP1 lobby (S–M), WP2 shop, landing zone and placement (L), WP3 pie, inventory, HUD, text and audio (L). WP3's HUD facts reuse step 9's semantic state. | — | The audit's §12 and §17 gates |

### 7.2 Reconciliation with the audit's work packages

| Audit item | This plan | Position |
| --- | --- | --- |
| WP1 lobby pending state (Thesis, §11 Stage A, §17 #1) | Quick win 11, track U | **Agree.** Small and independent; ship it early. |
| WP2 semantic shop, landing zone and placement (§11 B, D; §17 #2) | Track U | **Agree** on content. **Disagree** that it precedes replication work: it does not change simulation speed or world freshness. |
| WP3 local pie, inventory, HUD and audio (§11 C, E; §17 #3) | Track U; HUD facts after step 9 | **Agree.** Removing per-guest GUI capture also helps step 4. |
| WP4 loading, relevance and snapshots (§17 #4; ranked after UI in the Thesis) | Steps 3–7 | **Disagree with the order.** The benchmarks fail on simulation speed and freshness, so this work goes first. **Agree** with §14: deltas only against acknowledged baselines, with full recovery. |
| WP5 prediction (§13, §17 #5) | Steps 9–11 | **Agree** that it comes last and needs semantic state. Adds the tolerance requirement from randomized jetpack thrust. |
| "Profile in parallel" (Thesis, §16, §17) | Steps 1–2 at the front | **Disagree** that it runs in parallel: the measurements decide steps 3–8. |

The audit gives three different orders (Thesis, §11 and §17). This table is meant to replace all three for planning.

## 8. Notes on the legacy netcode

The legacy system rendered each player's view on the server into 8-bit world and GUI bitmaps, delta-encoded and LZ4/LZ4HC-compressed them in boxes (optionally interlaced), and sent them from one thread per player at a fixed encoding rate. It sent terrain colour once and then sent rectangle terrain-change events ([NetworkServer.cpp#L38](../Source/Managers/NetworkServer.cpp#L38), [#L725](../Source/Managers/NetworkServer.cpp#L725), [#L1138](../Source/Managers/NetworkServer.cpp#L1138), [NetworkServer.h#L52](../Source/Managers/NetworkServer.h#L52)). It is excluded from the build ([meson.build#L16](../Source/Managers/meson.build#L16)).

Upstream deleted its integration in January 2025 (commit `91fe85dd8`). This fork's commits `d0de4cea3` and `648c2e6e5` replaced it, citing unsafe packet streaming and the goal of local GPU rendering with interpolation. Two inferences from the design: per-player server rendering cost and bandwidth scaled with screen area, and frames could not be interpolated. The one reusable idea is its terrain-change hook placement (§4.2).

## 9. Sources

**Primary, read directly [P]**

- [src-const] Source SDK 2013, [`src/public/const.h`](https://github.com/ValveSoftware/source-sdk-2013/blob/master/src/public/const.h): `MAX_EDICT_BITS` 11, `DEFAULT_TICK_INTERVAL` 0.015.
- [src-nv] Source SDK 2013, [`src/public/networkvar.h`](https://github.com/ValveSoftware/source-sdk-2013/blob/master/src/public/networkvar.h): change detection with `NetworkStateChanged`.
- [src-dt] Source SDK 2013, [`src/public/dt_send.h`](https://github.com/ValveSoftware/source-sdk-2013/blob/master/src/public/dt_send.h): `SendPropFloat` bit counts and ranges.
- [src-gm] Source SDK 2013, [`src/game/shared/gamemovement.cpp`](https://github.com/ValveSoftware/source-sdk-2013/blob/master/src/game/shared/gamemovement.cpp): shared client/server movement with swept-box collision.
- [src-player] Source SDK 2013, [`src/game/server/player.cpp`](https://github.com/ValveSoftware/source-sdk-2013/blob/master/src/game/server/player.cpp): the VPhysics shadow follows game movement.
- [src-lc] Source SDK 2013, [`src/game/server/player_lagcompensation.cpp`](https://github.com/ValveSoftware/source-sdk-2013/blob/master/src/game/server/player_lagcompensation.cpp): players only; `sv_maxunlag` 1.0.
- [src-props] Source SDK 2013, [`src/game/server/props.cpp`](https://github.com/ValveSoftware/source-sdk-2013/blob/master/src/game/server/props.cpp): break via temp entity with an audibility filter.
- [src-propsh] Source SDK 2013, [`src/game/shared/props_shared.h`](https://github.com/ValveSoftware/source-sdk-2013/blob/master/src/game/shared/props_shared.h) and [`props_shared.cpp`](https://github.com/ValveSoftware/source-sdk-2013/blob/master/src/game/shared/props_shared.cpp): `PHYSICS_MULTIPLAYER_*`, `MULTIPLAYER_BREAK_*`.
- [src-cprops] Source SDK 2013, [`src/game/client/physpropclientside.cpp`](https://github.com/ValveSoftware/source-sdk-2013/blob/master/src/game/client/physpropclientside.cpp): `cl_phys_props_max` 300.
- [q3-snap] id Software, Quake III Arena, [`code/server/sv_snapshot.c`](https://github.com/id-Software/Quake-III-Arena/blob/master/code/server/sv_snapshot.c): delta from the acknowledged frame; PVS and area culling.
- [gmod-net] Garry's Mod wiki, [Networking Options, Limits and Errors](https://wiki.facepunch.com/gmod/Networking_Usage) and [net library](https://wiki.facepunch.com/gmod/net).
- [gmod-iftp] Garry's Mod wiki, [IsFirstTimePredicted](https://wiki.facepunch.com/gmod/Global.IsFirstTimePredicted).
- [gmod-effect] Garry's Mod wiki, [util.Effect](https://wiki.facepunch.com/gmod/util.Effect).
- [gmod-perf] Garry's Mod wiki, [PhysEnvPerformanceSettings](https://wiki.facepunch.com/gmod/Structures/PhysEnvPerformanceSettings).
- [gmod-sleep] Garry's Mod wiki, [PhysObj:Sleep](https://wiki.facepunch.com/gmod/PhysObj:Sleep).
- [gmod-cg] Garry's Mod wiki, [COLLISION_GROUP](https://wiki.facepunch.com/gmod/Enums/COLLISION_GROUP).
- [gmod-ents] Garry's Mod wiki, [ents](https://wiki.facepunch.com/gmod/ents) (networked entity limit).
- [gaffer-lockstep] Glenn Fiedler, [Deterministic Lockstep](https://gafferongames.com/post/deterministic_lockstep/).
- [gaffer-interp] Glenn Fiedler, [Snapshot Interpolation](https://gafferongames.com/post/snapshot_interpolation/).
- [gaffer-compress] Glenn Fiedler, [Snapshot Compression](https://gafferongames.com/post/snapshot_compression/).
- [gaffer-sync] Glenn Fiedler, [State Synchronization](https://gafferongames.com/post/state_synchronization/).
- [gaffer-fp] Glenn Fiedler, [Floating Point Determinism](https://gafferongames.com/post/floating_point_determinism/).
- [fiedler10] Glenn Fiedler, GDC 2010, [Networking for Physics Programmers (slides)](https://media.gdcvault.com/gdc10/slides/Fiedler_Glenn_PhysicsForProgrammers_NetworkingForPhysicsProgrammers.pdf).
- [rl] Jared Cone (Psyonix), GDC 2018, [It IS Rocket Science! (slides)](https://media.gdcvault.com/gdc2018/presentations/Cone_Jared_It_Is_Rocket.pdf).
- [teardown] Dennis Gustafsson, [The unlikely story of Teardown Multiplayer](https://blog.voxagon.se/2026/03/13/teardown-multiplayer.html), 13 March 2026.
- [roblox] Roblox Creator Docs, [Network ownership](https://create.roblox.com/docs/physics/network-ownership).
- [ue-phys] Epic Games, [Networked Physics Overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/networked-physics-overview).
- [fff83] Factorio, [Friday Facts #83: Hide the latency](https://www.factorio.com/blog/post/fff-83).
- [fff188] Factorio, [Friday Facts #188: Bug, Bug, Desync](https://factorio.com/blog/post/fff-188).
- [fff302] Factorio, [Friday Facts #302: The multiplayer megapacket](https://www.factorio.com/blog/post/fff-302).
- [factorio-libs] Factorio Lua API, [Libraries and functions](https://lua-api.factorio.com/latest/auxiliary/libraries.html).
- [factorio-lifecycle] Factorio Lua API, [Data lifecycle](https://lua-api.factorio.com/latest/auxiliary/data-lifecycle.html).
- [ggpo] GGPO, [Developer Guide](https://github.com/pond3r/ggpo/blob/master/doc/DeveloperGuide.md) and [ggpo.net](https://www.ggpo.net/).
- [p2059] WG21 P2059R0, [Make Pseudo-random Numbers Portable](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2020/p2059r0.pdf).
- [TiDi] CCP Veritas, [Introducing Time Dilation](https://www.eveonline.com/news/view/introducing-time-dilation-tidi), 22 April 2011.

**Primary content through a mirror or search excerpt [P\*]**

- [Source networking] Valve Developer Community, [Source Multiplayer Networking](https://developer.valvesoftware.com/wiki/Source_Multiplayer_Networking) and [Lag Compensation](https://developer.valvesoftware.com/wiki/Lag_compensation). Direct fetch returned HTTP 403; facts used (15 ms tick, `cl_updaterate` 20, 100 ms interpolation, deltas from the acknowledged snapshot) come from search excerpts of these pages and agree with the SDK source.
- [Tribes] Mark Frohnmayer and Tim Gift, [The TRIBES Engine Networking Model](https://www.gamedevs.org/uploads/tribes-networking-model.pdf) (copy hosted by gamedevs.org).
- [Reach] David Aldridge (Bungie), GDC 2011, [I Shot You First](https://www.gdcvault.com/play/1014345/I-Shot-You-First-Networking). The Vault entry is video; slide text was read from a [slidetodoc mirror](https://slidetodoc.com/i-shot-you-first-gameplay-networking-in-halo/).
- [aoe] Paul Bettner and Mark Terrano, GDC 2001, [1500 Archers on a 28.8](https://zoo.cs.yale.edu/classes/cs538/readings/papers/terrano_1500arch.pdf) (copy hosted by a Yale course page).
- [ow] Timothy Ford (Blizzard), GDC 2017, ['Overwatch' Gameplay Architecture and Netcode](https://gdcvault.com/play/1024001/-Overwatch-Gameplay-Architecture-and). Video only. Claims here are limited to the Vault abstract and the credit in Cone's slides.

**Not accessible, or not used**

- Valve Developer Community pages *Prediction*, *Networking Entities*, *PVS* and the Yahn Bernier paper page: HTTP 403. Engine source was used instead.
- Glenn Fiedler, GDC 2015, [Networking for Physics Programmers](https://www.gdcvault.com/play/1022195/Physics-for-Game-Programmers-Networking): video only. The 2010 slides of the same talk title were used.
- Unity and Photon physics networking docs: not needed; Unreal and Roblox cover the same ground.
- Terraria, Noita: no primary engineering material on networking terrain or particles was found. Teardown's author blog was used instead.
