# Cortex Command room service

Runnable invitation-code broker and UDP relay. A host creates a ten-character code, displayed as `ABCDE-F2345`; guests enter it. Every game socket connects **outward** to this service, so hosts do not forward router ports. The host still runs physics, Lua, and rendering. The service needs no game assets, display, FMOD, or GPU.

## Run locally

From the repository root, with the existing Visual Studio C++ dependencies:

```powershell
.\Services\RoomService\Build.ps1 -Test
.\build-mp\cc-room-service.exe --port 8001
```

In **Multiplayer → Connection settings**, enter `127.0.0.1:8001` for same-computer testing, or the service computer's LAN IPv4 address for LAN players. Leave **Join with room codes** enabled. The host chooses **Create room → Copy code**; friends paste it under **Join → Room code**, choose teams, and ready up. Passwords are optional. Lowercase codes and the displayed hyphen are accepted.

The game remembers the endpoint in `Userdata/MultiplayerService.txt`. For distributing a build, include a root `MultiplayerService.txt` containing your deployed endpoint on the first line. Players then only need a room code. `CCCP_MP_SERVICE` overrides both files for launchers/testing. No public AWS endpoint is preconfigured.

On Linux, install a C++20 compiler, CMake 3.20+, and Make:

```sh
cmake -S Services/RoomService -B build-room -DCMAKE_BUILD_TYPE=Release
cmake --build build-room -j 2
./build-room/cc-room-service --port 8001 --max-rooms 100 --max-mbps 800
```

Container build, from the repository root:

```sh
docker build -f Services/RoomService/Dockerfile -t cc-room-service .
docker run --rm --name cc-rooms -p 8001:8001/udp cc-room-service
```

The image runs unprivileged. The native Linux service and tests have been built and executed under Ubuntu/GCC 11; Docker execution needs verification on a running Docker engine.

## AWS deployment

`aws/room-service.yaml` creates an isolated VPC, subnet, one EC2 instance, Elastic IP, and one UDP ingress port. Administration uses SSM; no inbound SSH or HTTP is opened. IMDSv2 is required, storage is encrypted, and systemd runs the service unprivileged. Bootstrap downloads a private immutable source archive, verifies SHA-256, builds, runs real UDP integration tests, and signals readiness. New source/configuration creates a launch-template version and replaces the instance so bootstrap reruns. The Elastic IP stays stable. **Updates interrupt existing rooms.**

You need an authenticated AWS CLI, an existing private S3 artifact bucket in the chosen region, and EC2/VPC/IAM deployment permissions. Keep the bucket's public access blocked and encryption enabled. The instance role reads only the selected release object; uploads use SSE-S3. Archives contain service/transport source, without credentials or game assets.

Prepare a reviewable change set using your values:

```powershell
.\Services\RoomService\aws\Deploy.ps1 `
  -StackName cortex-rooms -Region us-east-1 `
  -ArtifactBucket YOUR-PRIVATE-BUCKET -PlayersCIDR YOUR-PLAYER-IP-RANGE
```

`PlayersCIDR` has no default. Use a bounded IPv4 range for initial testing; public internet access requires selecting the intended public range. Review the change set, then rerun with `-Execute` to provision. After readiness the script prints `ServiceAddress` and `InstanceId`. Put that endpoint in the distributed `MultiplayerService.txt`; alternatively point your DNS A record at the Elastic IP.

AWS syntax validation runs before change-set creation. Local `cfn-lint`/`cfn-guard` were unavailable, and account-side validation was blocked by an expired AWS login. YAML and script syntax have local checks. **This stack has not been deployed or tested on EC2 yet.** Reauthenticate and complete AWS validation before rollout. Relevant behavior: [EC2 CloudFormation properties](https://docs.aws.amazon.com/AWSCloudFormation/latest/TemplateReference/aws-resource-ec2-instance.html), [user-data execution](https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/user-data.html), and [readiness signals](https://docs.aws.amazon.com/AWSCloudFormation/latest/TemplateReference/cfn-signal.html).

## Operation

Aggregate JSON metrics print every ten seconds: rooms, connections, relayed bytes, dropped packets. Codes, passwords, resume secrets, player names, and chat are never logged. `--metrics-file` writes the latest metrics locally. Through SSM use `systemctl status cc-room`, `journalctl -u cc-room`, and `/var/lib/cc-room/metrics.json`. Use EC2 CPU/network metrics for load assessment; this initial stack has no load balancer or external monitoring platform.

Invitation codes have 50 bits of random entropy; resume secrets have 128 bits. Membership and routing are room-scoped. Guests only send to their host; hosts only send to their own guests. The service cannot forward to arbitrary internet destinations. It checks passwords before registration and bounds room creation/join requests, per-IP connections, packet sizes, relay throughput, and outgoing queues. Unregistered sockets expire after ten seconds. Capacity parameters are admission limits, not a measured concurrent-match guarantee.

Unexpected disconnects reserve membership for sixty seconds. Valid resume secrets replace stale sockets immediately. The game retries brief outages and restores its player slot and looping sounds. Closing the host room invalidates its code. Rooms live in memory: service restarts discard all codes, requiring hosts to create new rooms. This is one service instance, without persistent rooms or cross-instance failover.

Code connections use the relay for all traffic. Direct LAN/address play remains available by disabling **Join with room codes**. NAT hole punching and selecting an optimized direct internet path are not implemented. Networks must permit outgoing UDP; UDP-blocking networks cannot use this transport.

The bundled RakNet transport does not encrypt gameplay, room passwords, or resume messages. Codes restrict membership but do not provide traffic confidentiality or account authentication. Use unique room passwords rather than account credentials. Public operation needs patching, monitoring, abuse response, and load testing.

AWS charges for compute, storage, public IPv4, and outgoing traffic. A fully used 24 Mbps stream budget is 10.8 GB per guest-hour, or 32.4 GB/hour for three guests before overhead. Actual use depends on activity. Choose bandwidth/admission limits and instance size for your audience. Overload drops frame packets rather than growing queues indefinitely. T3 instances use standard CPU credits to avoid automatic surplus-credit billing; sustained CPU overload can throttle them.

## Verification

```powershell
.\Services\RoomService\Build.ps1 -Test
.\Tests\RunMultiplayerTests.ps1 -Smoke -Guests 3 -Relay
```

The relay suite covers code normalization/malformed headers, passwords, invalid codes, four-player capacity, simultaneous sequenced inputs, exact reliable payloads, three independent frame streams, guest reconnect, host resume, room isolation, and host closure. All four game transports have incoming connections disabled. To check a deployed endpoint:

```powershell
.\build-mp\relay-tests.exe 8001 YOUR-SERVICE-IP
```

This creates temporary test rooms, sends actual UDP data, then closes them. The game smoke test runs a real Windows host and three guest processes through the service, checking two matches, both stream sizes, remote controls/GUI text, chat, sound replay/stop, reconnect, and actual OpenGL screenshots. Evidence is in ignored `build-mp/` logs. Cross-machine internet acceptance and live AWS verification remain separate checks.
