# Mining AI diagnosis — 2026-10-03

The reported symptoms are miners standing around, stopping after initial progress, and failing to seek more distant gold. The source contains both narrow prospecting rules and reproducible stalls. The stalls need correction alongside better exploration; increasing the search radius alone would leave permanent idle states intact.

The findings below describe the original behavior before the fix. Source line numbers refer to that version. The source-level reproductions use LuaJIT 2.1, with fixtures for engine objects, terrain queries, and timers. They do not simulate physics, pathfinding, multiplayer, or an entire match.

## Implemented correction

- `GoldMiningSurvey` indexes every terrain pixel at scene load, then refreshes the index incrementally. Targets are actual remaining gold pixels, including isolated scraps, edges and deeply buried veins. Removed gold is revalidated before assignment; newly settled gold is discovered by the rolling refresh.
- Miners keep one persistent mining coroutine. They reserve separate patches for their team, excavate directly through terrain their digger can break, and use pathfinding for open travel and harder obstacles. Close aiming uses the arm joint and miners reposition for scraps inside the barrel's reach. Empty or fully reserved terrain leaves the mining order alive for future work.
- Lack of progress releases a patch and retries it after a delay. Reservations expire when a miner disappears and are released on cancellation. When all remaining patches are assigned, idle team-mates can assist older jobs. Failed paths are not permanently blacklisted.
- The arrived-waypoint transition creates sentry behavior only for a go-to order. Digging orders retain their mining job.
- If combat or an alarm interrupts mining, the scheduler restores the mining coroutine even if an old movement routine is still present.
- Equipment searches count requests actually dispatched and have bounded waits for both callbacks and pickup routes. Digger-only miners are no longer interrupted by a periodic rifle search.

Regression coverage is in `Tests/GoldMiningTests.cpp` (exact terrain index), `Tests/GoldMiningTests.lua` (real Lua behaviors/scheduler with engine fixtures), and `Tests/GoldMining.rte` (native physics/pathfinding activity). Native acceptance requires the fixture to log `PASS: all terrain gold mined`, with no AI script errors; source-harness passes alone are insufficient.

Two native acceptance runs on 2026-10-03 each cleared all 1,371 terrain gold pixels in the 960 x 540 fixture, in 108.91 and 127.06 simulated seconds, with all three miners alive at 100 health and no AI script errors. The second run used the updated playable `Cortex Command.exe`. This includes a light digger, two heavy diggers, buried veins at different depths, single-pixel gold and newly settled scraps. The fixture has a solid bottom boundary to keep miners in the scene. Survey tests separately cover exact map boundaries, wrapping, released and expired reservations, cooperative finishing, retries, new gold and scene changes.

## 1. Prospecting is short ranged and favors shallow horizontal veins

`Data/Base.rte/AI/HumanBehaviors.lua`, `GoldDig`, lines 405–536:

- The sweep casts about 120 rays. Most sideways rays are 180 pixels long; the downward sector uses 60-pixel rays, starting at the digger muzzle.
- The scoring adds five times the vertical distance and subtracts another 40–80 points for predominantly horizontal targets. This strongly favors gold at a similar height.
- The scan stores only the best location for its current invocation. It keeps no map of veins, visited areas, exhausted areas, failed targets, or territory assigned to other miners.
- If the scan finds nothing, it picks a nearby downward point, normally 40 pixels sideways and 80 pixels down, based on two terrain-strength rays. Near the bottom it picks a nearby horizontal point instead. It hands that point to the general movement behavior.

The synthetic terrain fixtures detect a vein 75 pixels sideways from the actor, but miss a vein 300 pixels sideways and another 105 pixels directly below the muzzle. Both missed-vein cases choose the same blind fallback point. This verifies the search geometry; it does not establish whether an actor can physically reach those veins.

## 2. One arrival path switches a miner into sentry behavior

`Data/Base.rte/AI/SharedBehaviors.lua`, `GoToWpt`, lines 381–395:

When there are no queued waypoints and the last target is close enough, the function clears the route and unconditionally calls `AI:CreateSentryBehavior(Owner)`. It checks `AIMODE_GOTO` only afterward, when setting the sentry position and facing. The guard-behavior call itself is outside that check.

`NativeHumanAI:Update`, lines 195–224, requests a fresh mining behavior only when the AI mode changes or both current behaviors are absent. Sentry remains an active behavior, while the actor's mode remains `AIMODE_GOLDDIG`. Therefore the miner can keep its gold-digging order and stop mining.

Reproduction: feed the real movement coroutine an arrived miner with no queued waypoints, then run the real scheduler. Result: `next behavior=Sentry`, followed by `behavior=Sentry; fire=false; movement=0`. Restricting that arrival transition to `AIMODE_GOTO` in memory makes the same fixture resume `GoldDig` without script errors.

This establishes a valid stall path. Its frequency in the user's matches has not been measured.

## 3. Item searches can wait for callbacks that will never happen

`Data/Base.rte/AI/HumanBehaviors.lua`, `WeaponSearch`, lines 613–651, and `ToolSearch`, lines 745–767:

Both routines initialize `searchesRemaining` to the number of collected candidate devices. Only asynchronous path callbacks decrement it. Candidates skipped before a path request therefore remain counted forever.

The deterministic weapon-search case is especially relevant: a nearby non-digger tool is collected as a candidate, then rejected with `pathMultiplier = -1`. No path request is made for it, but it remains in `searchesRemaining`. The coroutine waits forever. A device becoming invalid before dispatch creates the equivalent problem in either search.

This can affect a miner with only a digger: `NativeHumanAI:Update`, lines 537–546, periodically starts weapon acquisition when `EquipFirearm(false)` fails. `AHuman::EquipFirearm` accepts weapons, so a digging tool does not satisfy that check. Once the current movement/mining route finishes, the hung acquisition behavior prevents the scheduler from starting another gold search.

Reproduction: one rejected non-digger tool produces zero path requests, but the real weapon-search coroutine remains suspended after 300 resumes. Running that wait through the real scheduler leaves `behavior=WeaponSearch; mode=GOLDDIG; fire=false; move=0`. Counting actual dispatched requests instead of all candidates eliminates the wait in the fixture. The vanished-tool fixture behaves the same way.

## 4. Every scan creates several seconds of inactivity

`GoldDig` yields after every angle, plus an additional yield for valid gold hits. It scans the entire arc before selecting a route; it does not begin digging as soon as it finds a useful vein.

At the engine defaults of a roughly 1/60-second simulation step and an AI update interval of 2:

- No-gold fixture: 123 coroutine resumes, about 4.1 simulated seconds.
- Nearby-vein fixture: 130 resumes, about 4.3 simulated seconds.

These fixtures issue no firing commands during the scan. The no-gold scan alone accounts for a conspicuous pause before each fallback move. A diagnostic in-memory probe that batches eight scan angles per yield reduces that fixture to 18 resumes, about 0.6 seconds. This is a timing probe, not a production patch; abort handling and the per-update work budget require a proper implementation.

## Original diagnosis validation and limits

The retained local diagnostic harness is in `build-mp/ai-diagnosis/`. Run:

```powershell
python build-mp/ai-diagnosis/run.py
python build-mp/ai-diagnosis/run.py --differential --probe=arrival
python build-mp/ai-diagnosis/run.py --differential --probe=counter
python build-mp/ai-diagnosis/run.py --differential --probe=scan
python build-mp/ai-diagnosis/run.py --differential
```

The baseline intentionally exits with failing checks. Each isolated probe clears its corresponding failure while preserving unrelated failures. The combined diagnostic probes pass. Production files remain unchanged by these probes.

A temporary activity was also launched during the original diagnosis using the then-current local executable and the released executable. Those initial attempts did not yield an activity trace or usable gameplay evidence. The native acceptance fixture described above subsequently verified the implemented fix with actual physics and several miners. The original diagnostic module is removed from the active Mods folder and retained under the diagnostic folder.

## Original implementation recommendation

First correct the arrival transition and item-search accounting, and give asynchronous waits a bounded recovery path. Then make mining a persistent job: budget scans across updates, remember depleted or unreachable targets, expand the search when nearby veins are exhausted, and choose new reachable exploration points after sustained lack of movement or gold income. Coordinate target selection between miners to reduce repeated work on the same patch.

Verify the resulting behavior in an actual scene with several miners, nearby and distant veins, unsuitable rock, discarded tools, and depleted mining areas. Track gold income, movement, time spent scanning, and sustained idle periods rather than relying on a successful build.
