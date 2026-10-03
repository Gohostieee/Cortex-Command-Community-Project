# AI movement and jetpack control

The movement changes remove unnecessary pauses and correct navigation decisions that did not match jetpack physics. They are implemented in the Lua AI, so the local game loads them on its next launch without rebuilding the executable.

## Behavior

- Unobstructed travel runs immediately, without random 2–5-second walking intervals. Combat, alarms, and crawling still slow movement. Advancing miners can run while carrying and using a digging tool.
- Ground contact is checked every 75 ms, with two empty checks required before entering flight. The debounce counts checks rather than individual feet. A short downward terrain probe prevents running strides from being mistaken for sustained flight.
- Flight prediction uses the pack's actual nozzle range, actor facing, strafing, rotation, and fixed-nozzle behavior. Velocity and acceleration share one fuel-limited time horizon. Burst impulse is applied as a mass-dependent velocity change.
- Air steering for standard packs resumes ordinary thrust instead of spending fuel on another takeoff burst every time steering pulses. Excavation retains its lift bursts, and jump packs retain their separate launch behavior and minimum fuel requirement.
- Recharge does not block walking, route updates, or obstacle recovery. Standard packs can resume after a useful partial recharge, respecting minimum fuel settings; jump packs retain the fuller recharge threshold. A falling actor can leave recharge mode with usable fuel.
- Projected horizontal speed is scored alongside position, so actors brake before destinations. Candidate flight paths check head clearance. Slightly elevated ground destinations do not trigger takeoff, and actors ascending above a lower destination release lift.
- Miners directly excavate breakable ceiling remnants up to two actor heights above them. Faster movement exposed a recovery loop where a remnant above a newly opened tunnel repeatedly sent the miner around a long route. Higher or stronger obstacles still use normal navigation.

## Verification on 2026-10-03

`Tests/AILocomotionTests.lua` runs the real movement coroutine and human/crab schedulers with deterministic engine fixtures. It covers recharge travel, descent recovery, one-foot actors, ground gaps and strides, immediate running and combat transitions, nozzle configurations, frozen nozzles, mass-dependent bursts, consistent lookahead, braking, takeoff suppression, standard air thrust, excavation bursts, and jump-pack fuel requirements. The existing gold-mining Lua suite also covers the ceiling-remnant recovery.

The actual playable `Cortex Command.exe` ran `Tests/AILocomotion.rte`, using native terrain, pathfinding, emissions, and physics. Three actors follow separate 1,350-pixel routes, including two 180-pixel ledges. The final acceptance requires completed paths, low arrival velocity, remaining near the destination, no scene escape or movement damage, and no AI script errors. It checks both directions of travel. All actors retained 100 health.

| Route | Original diagnostic arrival | Updated normal runs | Final mirrored run |
| --- | ---: | ---: | ---: |
| Light soldier, flat | 22.15 s | 17.35–20.02 s | 18.15 s |
| Light soldier, ledge | 23.22 s | 20.82–28.02 s | 17.88 s |
| Heavy soldier, ledge | 37.88 s | 21.08–22.15 s | 22.42 s |

These are unseeded native runs, not a controlled statistical benchmark. The original diagnostic counted entering the destination area; the updated tests require settling and completing the path. The light ledge result varies, so the data does not establish a speed improvement on every route. The heavy route consistently completed much sooner in the sampled runs.

The final native mining regression cleared all 1,371 terrain gold pixels in 96.81 simulated seconds, with three healthy miners and no AI script errors. An intermediate movement version left a ceiling scrap after 600 seconds; the ceiling-remnant regression and direct-excavation adjustment resolve that failure. Historical mining acceptance at the prior revision took 108.91 and 127.06 seconds, also in unseeded runs.

Evidence is retained under `build-mp/locomotion-native-*` and `build-mp/mining-native-*`. Native startup emits an empty-scene lookup diagnostic and sound/DSP readiness messages; successful fixture startup and AI-script checks are separate from these engine messages. The acceptance scenes do not cover an entire combat match, crowded squads, or every third-party actor and jetpack.

## Run the checks

```powershell
python Tests/RunGoldMiningLuaTests.py --lupa-dir build-mp/ai-diagnosis/python --test AILocomotionTests.lua
python Tests/RunGoldMiningLuaTests.py --lupa-dir build-mp/ai-diagnosis/python
pwsh -NoProfile -File Tests/RunAILocomotionNativeTests.ps1 -Label verification
pwsh -NoProfile -File Tests/RunAILocomotionNativeTests.ps1 -Label mirrored -Mirrored
pwsh -NoProfile -File Tests/RunGoldMiningNativeTests.ps1 -Executable 'Cortex Command.exe'
```

The source runner requires Python with `lupa.luajit21`; `--lupa-dir` may be omitted when that package is installed normally. Native tests create temporary Mods modules and move them into the evidence directory when finished. Run separate native fixture types only when their temporary module names are not already in use.
