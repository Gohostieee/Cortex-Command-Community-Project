# AWS multiplayer benchmark, 9 October 2026

Moderate diagnostic load completed its lifecycle checks, but this service is
not demonstrated ready for heavy combat or adverse networks. The default-rate
case delivered fresh combat states at 16–18 Hz; its creator reconnect/control
gate failed. Heavy actor loads slowed actual simulation substantially.

This benchmark uses the deployed `cortex-hosted` service in `us-east-1`, a
`c7i.xlarge` with four virtual CPUs, 8 GiB RAM and two admitted room slots.
Each match has four native Windows clients; a two-room run has eight clients.
All Windows clients share one PC with 16 logical processors, about 40 GiB RAM,
and an RTX 3080 Ti. Client FPS therefore includes contention on this PC.

The benchmark client adds opt-in timing traces to the normal client. The
native Linux gameplay executable is the deployed production binary. Increased
load comes from an isolated copy of the One-Man Army activity that replenishes
real AI soldiers, equips assault rifles, and optionally gibs grenades and
dropships. The original production activity and released Windows ZIP are
preserved. These controlled battles do not establish unrestricted gameplay
acceptance, single-client FPS, AI parity, or performance across different
geographic player networks.

The hosted verification fixture starts with a **12 Mbps per-player upload
allowance**, whereas ordinary hosting defaults to **3 Mbps**. Actor-load
results at 12 Mbps are diagnostic capacity measurements; they must not be
presented as proof of default production performance. The final default-rate
case explicitly sends the normal owner bandwidth control, waits for the
acknowledged lobby value, and records `upload_mbps` in every trace row.

## Measurements

`CCCP_MPBENCHMARK` records actual installed world snapshots, local render
intervals, sampled update age, world-update rate and transport RTT. Combat
statistics use the second match and exclude the initial two seconds after
readiness. Deliberate creator leave/rejoin is excluded while the world is
unready; no-update intervals while the world remains ready are retained.

The isolated server activity records simulation time versus real elapsed
time, actor counts, actual stress soldiers alive/spawned/firing/damaged,
particles, MOIDs and burst counts. Separate one-second server measurements
record process CPU, resident memory, threads, handles, available memory,
network traffic and broker occupancy. EC2 CloudWatch metrics provide an
additional host-level check.

The standard native fixture runs two matches, forces a creator disconnect,
exercises native menus and controls, returns to the persistent lobby, and
closes the room. Its second match lasts approximately 35 seconds after all
players warm up. Repeated cycles test recreation and cleanup; this is not a
single uninterrupted long-running match. Under heavy load a verification
assertion intentionally exits the worker; that must be distinguished from an
unplanned engine crash.

## Reproduction

Build a separate `Cortex Command.benchmark.exe` with the opt-in traces in
`Source/Managers/MultiplayerMan.cpp`. Enable the existing hosted verification
environment on an empty owned AWS broker. For actor and explosion load,
append `Tests/MultiplayerBenchmarkLoad.lua` to an isolated copy of
`Data/Base.rte/Activities/OneManArmy.lua` and configure
`CCCP_MPBENCH_ACTORS` / `CCCP_MPBENCH_BURST`. Point only the temporary
verification service override at that copy.

```powershell
./Tests/RunMultiplayerBenchmark.ps1 -Rooms 2 -Cycles 3 -Label ai24-eight-soak -GuestResolutions 640x360
python ./Tests/AnalyzeMultiplayerBenchmark.py build-mp/aws-benchmark/ai24-eight-soak
./Tests/RunMultiplayerBenchmark.ps1 -Rooms 1 -Label default3-four-loss -UploadMbps 3 -Loss -GuestResolutions 640x360
```

The WAN proxy forwards actual worker UDP traffic through loopback, with
seeded delay, jitter, random packet loss and per-peer bandwidth limits.
`-ProxyBase 38990` routes worker ports 8100–8101 through local proxy ports
38990–38991. The regular `-Loss` fixture instead discards one data chunk in
each eight-chunk parity group; these are different tests.

Always restore the production service after testing, verify zero test rooms,
and confirm no verification environment or overridden runtime remains.

## Evidence

| Case | Players / rooms | Upload allowance per player | Actual simulation speed | Fresh states per second | Lifecycle result |
| --- | --- | --- | --- | --- | --- |
| Stock activity | 4 / 1 | 12 Mbps diagnostic | Not instrumented | 16–18 | Passed two matches |
| 24 added AI soldiers | 4 / 1 | 12 Mbps diagnostic | 91.6% | 13–18 | Passed two matches |
| 24 added AI soldiers per room, three cycles | 8 / 2 | 12 Mbps diagnostic | 84.5–84.8% in final cycle | 10–17 | Passed twelve matches |
| 64 added AI soldiers per room | 8 / 2 | 12 Mbps diagnostic | 60.5–62.9% | 9–11 for persistent guests | Failed creator reconnect/camera gate |
| 128 added AI soldiers plus explosion bursts | 4 / 1 | 12 Mbps diagnostic | 37.8% | 5 for persistent guests | Failed lifecycle gate |
| Delay, jitter, 3% random UDP loss | 4 / 1 | 12 Mbps fixture, 3 Mbps proxy cap | No combat interval | No playable battlefield | Loading timed out |
| Stock activity with recoverable chunk loss | 4 / 1 | **3 Mbps default, acknowledged and traced** | 99.6% | 16–18 | Failed creator reconnect/control gate |

State rates are medians per client in measured second-match combat intervals.
Simulation speeds compare the server activity's simulation clock with its
real elapsed time over the final combat match. Even the passing two-room
case ran below real-time speed; passing lifecycle checks does not establish
performance parity with local gameplay. The creator's short pre-disconnect
interval is omitted from the persistent-guest rates in the heavier cases.

The 3 Mbps case deliberately discarded the first data chunk in each
eight-chunk parity group. All four clients reached an interactive battlefield
38.18–38.27 seconds after the first match's playing announcement. The three
persistent guests then rendered 83.9–87.0 FPS, with median fresh-state rates
of 16–17 Hz and 95th-percentile update gaps of 102–119 ms. No sampled combat
update age reached 500 ms. The creator's pre-leave interval was only 1.99
measured seconds, at 18 Hz. The native gate reported missing creator mouse
radial, Q-selection and camera-movement evidence and intentionally exited
the worker. The creator lifecycle consequently could not complete. The
exact cause of this failed gate has not been isolated; it is not evidence
of an unplanned production crash.

The moderate two-room repeat completed three cycles: six room lifecycles,
twelve native matches, and twenty-four passing client lifecycle traces. Each
room maintained 24 added AI soldiers. All cycles passed creator reconnect,
native controls, rematch and acknowledged closure. A fresh capacity probe
observed both workers connected to four players and correctly received
`All AWS match servers are busy. Try again shortly.`

Two rooms with 64 added soldiers each failed the native creator rejoin/camera
verification. Simulation advanced only about 60–63% as fast as real time.
World updates continued at roughly 9–11 Hz during the measured combat, with
95th-percentile gaps of about 171–196 ms for the six persistent guest clients.
The verification assertion exited a worker; this run is a failed lifecycle
test and is not a successful soak or proof of an unplanned engine crash.

The 128-soldier case added twelve grenade detonations and one dropship wreck
every three seconds. It reached 143 additional detonations/wrecks and over
8,000 active particles. Simulation advanced about 38% as fast as real time,
while persistent clients rendered about 62–67 FPS and received only about
5 fresh states per second. This illustrates why rendering FPS alone is not
a measure of multiplayer responsiveness. The lifecycle check failed.

The real-worker WAN test added 60 ms each way, uniform jitter of up to 20 ms
each way, 3% seeded random UDP packet loss, and a 3 Mbps per-peer limit. It
forwarded 210,618 datagrams and dropped 6,324 for simulated loss, with no
queue-overflow drops. All four clients connected, but none installed a
playable battlefield before the approximately four-minute verification
timeout. This is a battlefield-loading failure under the tested network
conditions; no combat latency or FPS result is claimed for that run.

Whole-case server process samples, including loading and lobby time, found
the following resource usage. CPU is the sum of measured broker/native
process use divided by four vCPUs. It is not a profiling result for the
simulation thread.

| Case | CPU, 95th percentile | Native resident memory peak, all rooms | Minimum host available memory |
| --- | --- | --- | --- |
| 24 AI, one room | 36.9% | 1.41 GiB | 5.63 GiB |
| 24 AI per room, two rooms | 66.0% | 2.88 GiB | 4.15 GiB |
| 64 AI per room, two rooms | 69.1% | 3.02 GiB | 4.03 GiB |
| 128 AI plus bursts | 50.7% | 1.69 GiB | 5.35 GiB |
| Default 3 Mbps stock activity | 26.8% | 1.37 GiB | 5.67 GiB |

CloudWatch's highest one-minute EC2 CPU sample over the two-hour test window
was 46.0%; process samples are finer-grained and can capture shorter peaks.
These measurements do not establish a memory leak, out-of-memory failure,
or a precise CPU bottleneck. The first baseline predates the corrected
native-process sampler, so its process CPU is unavailable.

Production restoration succeeded at 06:36:50 UTC. The service again uses
`/opt/cc-dedicated/current`, all verification/benchmark flags and the
temporary service override are removed, broker occupancy is zero, and the
original production Data hashes pass verification. All six sampler commands
finished successfully. The deployed Linux executable retains SHA-256
`6dca58e99f805dcf3593667c0b60383d461f4040bf7e97d347907cb016590517`.

Priorities from these results are battlefield loading under limited/lossy
connections, creator reconnect/control readiness, and simulation cost under
large actor/particle loads. A subsequent acceptance run should use the
ordinary 3 Mbps allowance, clients on separate PCs, and a continuous match
longer than this fixture before claiming production performance parity.

Raw traces, native lifecycle logs, process samples and the final machine
readable results are in `build-mp/aws-benchmark/`. The initial `ai24-four`
run is excluded: its fixture failed to track living actors and overspawned.
The corrected fixture tags actors directly
and verifies its actual living count. The first capacity probe is also
inconclusive because its occupancy observation was stale; the repeat probe
uses a fresh observation of both active four-player rooms.
The initial WAN forwarding attempt is excluded because its upstream socket
was incorrectly bound to loopback; the measured WAN run used a routed
upstream socket and verified actual forwarding/loss counters.

The local server evidence archive is `build-mp/aws-benchmark/server-evidence.tar.gz`
(365,402 bytes), SHA-256
`18146eafde6fdd017a5523c5466dd86dd3a95c42a7f7694e59411fc8b647227d`.
`result.json`, `cloudwatch-final.json`, `production-restored.json`, and
`samplers-finished.json` sit beside the raw client and extracted server traces.
