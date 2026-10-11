# Multiplayer remaining-work specification

Baseline: 2026-10-11. Branch `codex/multiplayer-rebuild`, checkpoint `b80e3a83a`.
Gameplay protocol **v10**. This document supersedes the "next work" sections
(4–8) of [the continuation specification](MultiplayerContinuationSpec.md); its
section 10 records the latest validation run.

## 1. Where things stand

The initial fixes below were committed as `e3c325015` and published in
`mp-2026.10.11`. Its matching Linux worker was deployed to AWS and passed the
four-client native lifecycle check. Subsequent debris delivery, sprite indexing,
diagnostics, and Windows auto-updater changes are included in preparation for
`mp-2026.10.11.2`, including the deep-path fix found during the real updater
download test. The acceptance work in section 3 remains a specification;
publication alone does not complete those gates.

| Area | State | Evidence |
| --- | --- | --- |
| Dedicated worker crash in combat | Fixed. Host test fixtures now wait for Lua's asynchronous garbage collection before creating scripted objects. The unmodified v10 build crashed in 2 of 3 local runs. | `repro-wan-01`, `orig-stock-02` worker `AbortLog.txt` |
| Rejoin stall (map never arrives) | Fixed. World data that overtakes the Welcome/Lobby is buffered and replayed. A guest holding states without a manifest re-requests it (`WorldAck` type 5). Unanswered resource credits expire after 20 s. | Native regression in `RunMultiplayerPresentationTests.ps1`; unit test for credit expiry |
| Lost native Leave blocks rejoin for 5 s | Fixed. Transport shutdown waits up to 500 ms to flush the Leave. A returning player's token reclaims a slot whose old connection still looks open. | Native regression; `trace-wan-01` failure, `takeover-wan-01` pass |
| Four-client hosted lifecycle under WAN impairment | Passes: two matches, automatic reconnects, creator Leave/room-code Join during combat, owner transfer, room closure. | `takeover-wan-01`, `seam-wan-01`, `reason-wan-01`, `drawn-wan-finish-01` |
| Own-movement prediction | Works most of the time but **misses its gates** (§3.3). | `drawn-wan-finish-01` |
| Linux worker | The v10 release starts headless and passed four-client stock combat and lifecycle on AWS. Further stress qualification remains open. | `mp-2026.10.11` release verification |
| AWS deployment | v10 deployed to `cortex-hosted` in `us-east-1`, endpoint `34.231.191.247:8001`. | `mp-2026.10.11` release verification |
| Separate-PC playtest | **Not done.** | — |
| Craft and gib debris on guests | Priority tiers, stable selection, local sprite indexing and resource retention implemented. Acceptance measurements remain separate (§3.4). | `MultiplayerDebrisSpec.md`; debris benchmark logs |

The full list of fixes, with their causes, is in §2. Benchmark evidence lives
in `%LOCALAPPDATA%/CortexCommandBenchmarks/<label>/`, outside Git.

## 2. Changes awaiting commit

Commit these as reviewable units, in this order, each building and passing
`RunMultiplayerTests.ps1`:

1. **Tooling.** `Tests/VisualStudio.ps1` locates any Visual Studio 2022 edition,
   including Build Tools; the test, build and packaging scripts use it.
2. **Worker crash.** `LuaMan::WaitForGarbageCollection`, called before host
   fixture work in `SmokeTick`.
3. **Rejoin reliability.** Early world-packet buffer and replay, manifest
   re-request, receipt-credit expiry (`ResourceWindow::Expire`), shutdown
   flush, token takeover, and the two native regressions in `VerifyWorldFlow`.
   The dedicated smoke test now names which final condition failed.
4. **Prediction.**
   - Footing learned as a median while walking, per gait.
   - Walking speed measured from host progress.
   - Stopping where the host is blocked.
   - Unknown terrain makes the guest fall back to host presentation instead of inventing terrain.
   - Collision falls back to held and last-drawn terrain revisions.
   - Tile pixels addressed by tile index.
   - Corrected `InputAge` comment: it is real time, and that is consistent with the guest's frame clock.
5. **Diagnostics.**
   - Per-frame prediction status and unknown-terrain reasons in `<role>-benchmark.csv`.
   - Per-state correction trace `<role>-motion.csv`.
   - New analyzer metrics.

Protocol: the snapshot layout is unchanged. `WorldAck` type 5 is new, and v10
hosts ignore unknown acknowledgement types. v10 was never deployed, so no
version bump is required. All clients and workers must still match.

## 3. Remaining work

### 3.1 Commit and repository hygiene

- Delete `Tests/__pycache__/` and add `__pycache__/` to `.gitignore`.
- `RunMultiplayerTests.ps1` (with `-Smoke`), `RunMultiplayerPresentationTests.ps1`
  and `RunMultiplayerCursorTests.ps1` pass `/p:TargetName` to the whole
  solution. On a fresh checkout this renames the LuaJIT library and the build
  fails. Build with the default name (`Cortex Command.exe`), then copy the
  result when another executable name is requested.
- `RunMultiplayerPresentationTests.ps1` fails when `Mods/` does not exist.
  Create it or skip the disabled-mods list.

Acceptance: a fresh clone builds and runs every script listed in §5 without
manual steps.

### 3.2 Documentation

- `Multiplayer.md` still states there is no gameplay prediction and that the
  protocol is v9, and its status banner describes the rejoin failure as open.
  Rewrite:
  - the banner;
  - the Implementation paragraphs on own-actor presentation, prediction and its fallbacks, rejoin buffering, manifest re-request and slot takeover;
  - a 2026-10-11 Verification entry.
- Mark sections 4–8 of `MultiplayerContinuationSpec.md` as superseded by
  this document.

Acceptance: no document describes the fixed rejoin failure as open, or
claims prediction is absent or accepted.

### 3.3 Prediction acceptance

Measured in `drawn-wan-finish-01` (WAN fixture, rematch combat):

| Client | Walking coverage | Actor frames drawn predicted | Correction p95 / max (px) |
| --- | ---: | ---: | ---: |
| Creator (960×540) | 87.3% | 60.6% | 4.46 / 12.09 |
| Guest 1 (640×360) | 100% | 78.8% | 3.29 / 11.01 |
| Guest 2 (960×540) | 100% | 55.2% | 10.51 / 68.24 |
| Guest 3 (1280×720) | 100% | 74.9% | 6.81 / 19.82 |

Gates, unchanged from the continuation specification:
- ≥95% coverage of eligible walking.
- Correction p95 < 4 px at about 120 ms RTT.
- No walking through solid terrain, attachment separation, correction oscillation or rejoin regression.

Work:

1. **Split the `NoFooting` status.** It currently also counts unlearned
   footing and failed replay steps. Report "footing unknown", "terrain
   unknown" and "airborne replay" separately, so coverage losses are
   attributable.
2. **Explain the remaining large corrections.** Guest 2 has a p95 of 10.5 px
   and a maximum of 68 px. Use `<role>-motion.csv` (the `below`,
   `below_gap_px` and `replay_known` columns) to classify them by cause:
   knockback from explosions, landing, gait change, or host overload. Fix the
   model or exclude the cause with a stated reason.
3. **Improve the creator's walking coverage (87%).** Find which frames fall back
   after its mid-combat rejoin, when footing is learned again.
4. **Add a movement fixture.** The current smoke input holds right and fire
   for the whole match, which tests almost nothing but walking into
   obstacles. Add scripted segments: start/stop, reverse, crouch-walk, jump,
   jetpack, slope, and walking into another actor. Measure each separately.
5. **Jetpack and air movement.** The model learns jet thrust and air control
   from host states, but nothing has validated this. Either meet the gates in
   the new fixture, or keep the explicit fallback for air movement and
   document the supported scope.
6. **Confirm resets.** Check prediction resets on pause, actor switch, death,
   loss of control, reconnect and new match in the native traces; status
   should go to `NoActor` or `Unsupported` and corrections should not spike.

Acceptance: the gates pass in the stock and WAN fixtures with the new
movement segments, or the supported scope is narrowed in the documentation
and every excluded interval has a recorded reason.

### 3.4 Combat debris visibility

Guests see little of a destroyed craft's hull pieces and other gibs. Two causes:
- The snapshot budget thins debris in proportion to explosion particles, and changes which pieces survive from one snapshot to the next.
- Guests download every object sprite the first time they see it, and the server cancels the queued download of a piece that was thinned out.

The full design, measurement plan and acceptance are in
[the combat debris specification](MultiplayerDebrisSpec.md):
- build object sprites locally from the guest's own game data;
- give rigid debris priority over particles, with stable selection between snapshots;
- keep sprite downloads alive across thinning.

No protocol change is required.

Acceptance: the debris specification's §5 passes in the stock and WAN
benchmarks.

### 3.5 Matching release from a committed revision

1. Build the Windows Final x64 client and the Linux package (Docker:
   `docker build --platform linux/amd64 -f Services/DedicatedServer/Dockerfile
   --output type=local,dest=build-mp/dedicated-package .`) from the same commit.
   Record the commit, archive hashes, and `release.json` with a real
   `sourceRevision`.
2. Package Windows with `Tools/PackageMultiplayer.ps1 -Version <version>`.
   The bundled endpoint is `34.231.191.247:8001`.
3. Verify that a v9 client, and a different v10 build, are rejected with the
   version message rather than a hang.
4. Run the four-client hosted benchmark (stock and `-Wan`) against the Linux
   worker, not only the Windows worker.

Acceptance: both packages come from one commit, every suite in §5 passes on
that commit, and the Linux worker passes the four-client lifecycle.

### 3.6 AWS deployment — needs credentials and explicit go-ahead

The development PC has no AWS credentials. Deploying replaces the worker host
and ends every running room.

1. With an authenticated CLI:
   - Inspect the `cortex-hosted` stack: its revision (expected v7), instance size, active rooms and metrics.
   - Prepare the change set with `Services/DedicatedServer/aws/Deploy.ps1` and the §3.5 Linux archive.
   - Keep the current v7 archive as the rollback package.
2. After explicit authorization, execute the change set, confirm the live
   worker reports the new revision, and run
   `Tests/RunHostedMultiplayerTests.ps1 -Guests 3 -ServiceAddress 34.231.191.247:8001`
   with test hooks temporarily enabled. Then remove the hooks and confirm the
   worker cleans up.
3. Ship clients only together with the deployed worker. v10 clients cannot
   join the current v7 service.

Acceptance: four clients pass the hosted lifecycle against the live endpoint,
and the deployed revision and hashes are recorded in `MultiplayerBenchmark.md`.

### 3.7 Separate-PC playtest — needs players

Players on separate PCs, including one realistic higher-latency connection,
play an ordinary continuous match at the 3 Mbps per-player budget:
- create, join, ready;
- placement;
- shop purchase and delivery;
- movement and firing;
- pie menu and inventory use;
- creator Leave/Join;
- rematch and room closure.

Collect `LogMultiplayer-*.txt` from every client and from the worker, and
record:
- fresh-state cadence and age;
- input response;
- simulation speed;
- FPS and ping;
- player judgement of combat usability.

Release gate: no loading or rejoin stalls, no sustained world freezes, and
players judge normal combat usable. Local tests do not replace this.

### 3.8 Later work, outside this release

- Local pie-menu hover and inventory interaction (see `MultiplayerClientAuthorityAudit.md`).
- Heavy-fight performance at 24 and 64 AI, measured on the real AWS host. The long-term target is ≥95% of real time.
- Local HUD facts and predicted weapon feedback.
- Arbitrary mods and physical controllers.
- Updater work.

## 4. Test environment notes

- **Memory.** The development PC has 16 GB. The five-instance benchmark drives
  free memory to about 20 MB, and the worker then stalls for 3–28 s in its
  network stage. The unmodified v10 build stalls the same way. Close other
  applications, or compare against a re-run of the baseline executable before
  calling a stall a regression. Cadence of 13–15 Hz on this PC reflects the
  server running at 68–76% of real time, not a protocol limit.
- **Ports.** Do not run `RunMultiplayerTests.ps1` while a benchmark's room
  service holds UDP 38997. Set `CCCP_MPTEST_PORT` instead.
- **Line endings.** Sources use CRLF. Git Bash `sed -i` converts them to LF.

## 5. Validation commands

```powershell
.\Tests\RunMultiplayerTests.ps1
.\Services\RoomService\Build.ps1 -Test
. .\Tests\VisualStudio.ps1; & $MSBuild RTEA.sln /m:6 /p:Configuration=Final /p:Platform=x64 "/p:ForceImportAfterCppTargets=$PWD\Tests\MultiplayerBuild.props"
.\Tests\RunMultiplayerPresentationTests.ps1 -SkipBuild
.\Tests\RunLocalMultiplayerBenchmark.ps1 -Label <fresh-label>
.\Tests\RunLocalMultiplayerBenchmark.ps1 -Label <fresh-label> -Wan
```

Read `PASSED=`, the `PASS:`/`FAIL:` lines in each `*-smoke.log` and in the
worker's `dedicated-smoke.log`, and the `motion_*` fields in
`client-summary.json`.

Next action: commit §2, then §3.1–3.4. Hold §3.6 until credentials and
authorization are available.
