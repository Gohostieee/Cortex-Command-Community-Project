# Multiplayer continuation specification

Status: development checkpoint, 2026-10-10. Branch: `codex/multiplayer-rebuild`.
Accepted baseline before this checkpoint: `0d5cb267c` (gameplay protocol v9).
Prediction checkpoint: `d7f857a55` (native acceptance still pending).
The experimental movement snapshot format now uses gameplay protocol **v10**.

## 1. Goal and current playability

Make ordinary four-player hosted matches playable with responsive local controls,
reliable loading/rejoining, and continuous authoritative world updates at the
normal 3 Mbps per-player allowance. Establish this on separate computers over a
real internet connection before calling it ready for friends.

The accepted v9 baseline is close for ordinary matches in controlled local
testing. Big battles remain slower than real time. This checkpoint preserves
unfinished movement prediction so work can continue; it is not a release or an
AWS deployment. A passing unit suite does not make prediction accepted.

The latest recorded AWS verification used gameplay protocol v7. The deployment
has not been inspected or changed for this handoff. Recheck its actual revision
before deploying; neither v9 nor v10 clients can join a v7 worker. All game clients
and workers must use a matching protocol/build. The room relay envelope remains
v1 and does not require a protocol bump for the movement change.

## 2. Completed work

These changes are already committed on this branch and are included in the push:

| Change | Commit | Result and evidence boundary |
| --- | --- | --- |
| Dedicated AWS authority and published endpoint | `cb388786a`, `10df4a6ca` | Each hosted room has a headless server; the creator is a client like the other players. Earlier v7 live AWS checks passed. |
| Native AWS benchmark tooling | `f1582e24d` | Measures loading, fresh-state cadence, simulation speed, reconnection and capacity independently of rendering FPS. Historical results are in `MultiplayerBenchmark.md`. |
| Loading and world delivery over impaired links | `3c0993b80` | Resource receipts, pacing and queue control keep downloads and current world states progressing. |
| Compact snapshots and overload behavior | `3f27e9778` | Delta encoding reduces repeated state; busy servers reduce snapshot cadence while keeping lifecycle checks working. |
| Immediate menus and pointers | `994fbf74d` | Lobby edits, camera, pointers and landing-zone feedback are local while the server validates outcomes. |
| Earlier own-actor presentation and retained scenery | `bdfe0744a` | Removes the interpolation buffer from the local actor, smooths corrections and retains scenery. Input still waits for the server in this baseline. |
| Networking/authority documentation | `18feb2a9b` | Records the v9 behavior, remaining authority boundaries and engine constraints. |
| Quiet test launchers | `efc918dc9` | Automatically launched game instances mute their master audio. |
| Native buy menu on every guest | `0d5cb267c` | Shop browsing, search and menu feedback run locally; purchases and deliveries remain server-owned. The supplied handoff reports a purchase-to-delivery pass for every player. |

The recorded v9 acceptance in `Multiplayer.md`, under Verification, reports:

- Four clients and a dedicated worker on one Windows PC passed stock, impaired
  WAN and 64-AI lifecycle checks, including creator Leave/Join during combat.
- Stock fresh-state cadence: 20 Hz, with p95 update gaps of 61-70 ms.
- Impaired WAN: 60 ms one-way delay, 20 ms jitter, 3% packet loss and 3 Mbps per
  player; first interaction after 2.6-3.1 seconds, 19-20 Hz, p95 gaps 78-85 ms.
  Terrain covered while missing was zero at the 95th percentile during combat.
  This is not a claim that every frame had zero missing tiles.
- With 64 added AIs, simulation ran at about 52% of real time on the shared PC;
  overloaded snapshot delivery was 15 Hz and lifecycle checks passed.
- Player-hosted relay/loss, deployment, presentation and menu/cursor regressions
  passed. The pre-prediction server and unit suite also built/passed on Ubuntu.

Those figures describe earlier v9 runs, not a fresh native acceptance of this
v10 checkpoint. Earlier public AWS runs existed, but these v9 improvements have
not been demonstrated with players on separate PCs over a real connection.

## 3. Work saved in this checkpoint

The following prediction changes were present but uncommitted at handoff:

- `MultiplayerMan.cpp`: acknowledge the input actually applied by simulation;
  export its age and measured simulation speed; log movement activity and
  correction distance.
- `MultiplayerWorld.cpp/.h`: export actor position, velocity, gravity and
  eligibility; apply local movement during guest rendering and move attachments
  with the actor; reset prediction with presentation state.
- `MultiplayerWorldProtocol.h`: extend snapshots, query retained foreground
  terrain, learn walking/air/jet motion from server observations, replay local
  input history and fade server corrections. Falling back to authoritative
  presentation is part of the current model.
- `MultiplayerTests.cpp`: checks walking, stopping, gradual correction, wall
  blocking and landing on a synthetic floor.
- `AnalyzeMultiplayerBenchmark.py`: summarize prediction activity share and
  correction percentiles. The CSV stores correction distance in tenths of a
  pixel; the analyzer converts it to pixels.

The handoff reports that the first native prediction run activated prediction
for only 12-33% of sampled time for three of four clients. It also failed the
creator's mid-combat rejoin: the returning client did not receive the map data
needed to resume. The cause and relationship to prediction are **unresolved**.
These are handoff-reported findings; their raw traces are not included in this
commit, and the failed run is not a new acceptance result.

This checkpoint also bumps gameplay protocol v9 to v10 and updates the wire
fixture, explicitly rejects v9, and verifies nondefault motion fields survive
serialization. The snapshot layout changed by 24 bytes, so reusing v9 would let
incompatible peers pass the handshake.

The movement model approximates actor physics against visible foreground pixels;
it does not replicate native collision materials, full actor bodies, dynamic
obstacles or deterministic physics. Predictability currently requires a stable
AHuman/ACrab in normal player control. It does not predict authoritative shots,
damage, inventory changes or terrain destruction.

## 4. Next work: restore rejoin reliability

Priority: release blocker. Diagnose before tuning prediction or deploying.

1. Preserve the failed run's client, server, service and benchmark traces if they
   are still available. Record exact source revision, executable hash, protocol,
   resolutions, impairment parameters, AI load and whether prediction was active.
2. Compare the accepted `0d5cb267c` baseline and this checkpoint in separate
   checkouts with matching client/worker binaries and the same hosted fixture.
   Use a focused stock reproduction first; add WAN impairment when it helps
   reproduce the failure. Do not mix v9 and v10 binaries.
3. Trace the rejoining slot's welcome/epoch, resource-cache inventory, scene
   manifest, required/available resource IDs, queued/retried/acknowledged
   resources, snapshot baseline and the exact readiness condition still false.
   Check whether server-side cache assumptions survive a client presentation
   reset, whether old receipts suppress necessary retransmission, and whether
   obsolete-queue removal drops resources still required by a rejoining client.
4. Fix the demonstrated failure and add one targeted regression at its actual
   boundary. Do not assume movement prediction caused a reliable map transfer
   stall, or mark it solved because a retry happened to succeed.

Acceptance: all four slots automatically reconnect and the creator uses native
Leave/room-code Join during combat, regains the original slot and controls, and
receives a complete playable scene while other clients continue. Changed terrain
must reload correctly. The group completes a second match in the same lobby and
the owner receives room-close acknowledgment. No readiness timeout, stale-epoch
acceptance or incomplete-scene activation is allowed.

## 5. Next work: make prediction consistently useful

Priority: finish after the rejoin failure is understood.

- Measure both eligibility and actual use per rendered frame, with explicit
  fallback reasons: actor/view/status unsupported, terrain unavailable, body
  extent unknown, model error too high, paused or local input disabled.
  Count actor-less frames and transitions correctly; a retained predictor's
  `Active()` flag alone does not prove a predicted actor was drawn that frame.
- Report prediction coverage for eligible controlled walking time separately
  from whole-match sampled activity. Menus, death, free flight and deployment
  must not make successful walking prediction look like failure.
- Investigate learned foot placement, terrain lookup/wrapping, gait learning,
  correction accumulation and re-entry after fallback. Check input sequence
  matching, acknowledgment timing and history trimming. `InputAge` is currently
  wall-clock age even though its comment describes simulated time; establish
  consistent timing when the server runs below real time.
- Stop/reset prediction appropriately on pause, lost control, actor switch,
  teleport, death, reconnect and new match. Preserve server authority and safe
  fallback where the local model lacks collision knowledge.
- Validate ordinary movement first, then jump/jetpack, slopes, obstacles and
  scene seams with actual native actors. Synthetic floor tests are insufficient.

Proposed acceptance gates, to be measured rather than assumed:

- Movement visibly responds on the next rendered frame after input.
- At least 95% coverage during eligible walking after model warm-up; every
  excluded interval has a reason. Total combat activity is reported separately.
- Correction p95 below 4 pixels at 120 ms RTT in the supported movement fixtures,
  following the target in `MultiplayerNetworkingResearch.md`. Record maximum
  corrections and fallback events as well as percentiles.
- No walking through known solid terrain, attachment separation, repeated
  correction oscillation, menu-driven actor motion or regression in rejoin.

If jetpack or unusual actor support cannot meet these gates, keep explicit
fallback and document the supported scope; do not present approximate physics
as complete gameplay prediction.

## 6. Next work: matching deployment and real-player acceptance

Priority: after the local release blockers pass. Cloud rollout remains a separate
action requiring the deployment go-ahead described in the original handoff.

1. Build Windows clients and a Linux worker from the same accepted revision.
   Run the Linux protocol/broker checks and headless startup. Package matching
   assets, record source revision and archive hashes, and verify mixed versions
   are rejected cleanly.
2. Inspect the actual AWS revision, capacity and active rooms. Prepare the
   deployment/change set and a rollback package; deploying replaces the worker
   host and ends running rooms, as described in the deployment guide.
3. After authorization, deploy, verify the live revision, then run four-client
   hosted acceptance against the real endpoint. Remove temporary verification
   hooks and confirm worker cleanup when testing ends.
4. Have players on separate PCs run an ordinary continuous match at the normal
   bandwidth budget: create/join/ready, placement, shop purchase and delivery,
   movement/firing, pie/inventory use, creator rejoin, rematch and room closure.
   Record fresh-state cadence/age, input response and simulation speed alongside
   FPS and ping. Include a realistic higher-latency connection.

Release gate: the full flow passes without loading/rejoin stalls or sustained
world freezes, and players judge normal combat usable. Save a concise result
table per client and server, with configuration and evidence references. Local
simulated WAN and same-PC public-server tests do not replace this acceptance.

## 7. Later work and scope boundaries

- Local pie-menu hover, descriptions, submenus and immediate selection feedback;
  local inventory hover, selection, drag and scrolling. Keep action availability,
  item ownership, equip/drop/reload and script execution server-authoritative.
  Reuse native menus and semantic state; see `MultiplayerClientAuthorityAudit.md`.
- Heavy-fight performance: measure the real dedicated host with 24/64 AI before
  selecting optimizations. Compare simulated time to real time, main-loop stage
  costs, capture/encoding time, state age, bandwidth and queue growth. The shared
  PC's 52% result does not predict AWS performance. Profile before changing AI,
  particles, terrain scanning or threading. The research document's long-term
  64-AI target is at least 95% of real time; it is not an achieved result.
- Full local HUD facts, immediate predicted weapon feedback, arbitrary mods and
  physical controller acceptance remain separate follow-ups.

Updater work is outside this checkpoint by the supplied scope: `.gitignore`,
`README.md`, `Tools/PackageMultiplayer.ps1`, `.github/workflows/updater.yml`,
`Documentation/AutoUpdater.md`, `Tests/RunUpdaterTests.ps1`, `Tests/UpdaterTests.cs`,
`Tools/BuildUpdater.ps1`, `Tools/InstallUpdater.ps1` and `Tools/Updater/` remain
uncommitted. Generated caches, binaries, logs and packages are also excluded.
Do not use the dirty packaging script to imply a reproducible release from this
checkpoint; use packaging from the intended committed revision or finish the
updater work separately.

## 8. Continuation commands and validation record

Run from the repository with Visual Studio 2022 C++ tools and Python available:

```powershell
.\Tests\RunMultiplayerTests.ps1
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe' RTEA.sln /m:4 /p:Configuration=Final /p:Platform=x64 /p:TargetName='Cortex Command.p4' "/p:ForceImportAfterCppTargets=$PWD\Tests\MultiplayerBuild.props"
.\Services\RoomService\Build.ps1 -Test
.\Tests\RunLocalMultiplayerBenchmark.ps1 -Label continue-stock-01 -GameExecutable 'Cortex Command.p4.exe'
.\Tests\RunLocalMultiplayerBenchmark.ps1 -Label continue-wan-01 -GameExecutable 'Cortex Command.p4.exe' -Wan
```

Use fresh benchmark labels. The native benchmark creates isolated runtime data,
starts a headless worker and four hidden clients, and runs lifecycle checks. Read
`PASSED=` and client/server failures as well as the summaries: a valid combat
interval is not proof that the creator rejoined. Keep the full evidence outside
Git and commit the concise findings after each meaningful change. Rebuild the
broker for the changed game protocol before native checks.

Checkpoint validation on 2026-10-10:

- `RunMultiplayerTests.ps1` rebuilt and passed the standalone protocol, input,
  retained-world, motion and real-UDP suite with v10. This includes v9 rejection
  and a round-trip check of the new motion fields.
- The Windows Final x64 game build completed successfully as
  `Cortex Command.p4.exe`. Existing project-import and third-party warnings
  remained; there were no build errors. The build log is the ignored
  `build-mp/continuation-build.log`.
- The benchmark analyzer passed Python syntax validation, and the scoped diff
  passed whitespace checks.

No fresh native prediction acceptance, Linux v10 validation, cloud deployment or
separate-PC player session is included in this documentation-and-checkpoint task.

## 9. References

- [Multiplayer behavior and recorded verification](Multiplayer.md)
- [Client authority audit and UI work packages](MultiplayerClientAuthorityAudit.md)
- [Networking research and performance targets](MultiplayerNetworkingResearch.md)
- [Historical live AWS benchmark findings](MultiplayerBenchmark.md)
- [Dedicated server build/deployment](../Services/DedicatedServer/README.md)

Next action: reproduce and explain the creator's scene-transfer stall, then fix
prediction coverage and repeat focused native acceptance before any rollout.
