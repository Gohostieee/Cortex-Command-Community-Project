# Cortex Command dedicated game workers

Hosted rooms run the real native game on AWS. The room service starts one separate game process per room and gives every player an assigned UDP endpoint. The room creator has the same client role as the other human players; their computer no longer runs authoritative physics, AI, Lua, damage, or terrain simulation. Existing host-computer rooms remain supported by the broker.

Workers run without a display, GPU, or audio device. Each process loads the matching game data and runs the same authoritative simulation as local play. A room has at most four human players, including its creator. The broker reserves worker capacity, waits for readiness, reaps stopped processes, and expires abandoned rooms. Per-room private configuration is held under the state directory and removed when the worker exits.

## Build a release

Use a Linux filesystem on Ubuntu 24.04 x86_64, with GCC 13 or later. Windows Git commonly stores FMOD links as short text files; the build repairs those links in the Linux source copy. Do not run this build directly in the Windows checkout.

```sh
sudo bash Services/DedicatedServer/InstallDependencies.sh
bash Services/DedicatedServer/Build.sh
```

The build runs the UDP protocol/transport suite, legacy room relay suite, and hosted broker/worker lifecycle suite. It emits `build-mp/dedicated-linux/cc-dedicated-ubuntu24-x86_64.tar.gz` and a SHA-256 sidecar. The release contains the native game, standalone broker, matching full `Data` directory, FMOD library, component licenses, and exact source archive. Build output and mutable user data are excluded.

Alternatively, build with a Linux Docker engine and export the final package:

```sh
docker build --platform linux/amd64 -f Services/DedicatedServer/Dockerfile --output type=local,dest=build-mp/dedicated-package .
```

## Run a broker and workers

Extract the release, install the runtime dependencies listed in `aws/dedicated-server.yaml`, and run from its root:

```sh
CC_HOSTED_ADDRESS=YOUR-PUBLIC-IP CC_STATE_DIRECTORY="$PWD/server-state" ./cc-start
```

`CC_SERVICE_PORT` defaults to 8001; `CC_GAME_PORT_BASE` to 8100; `CC_MAX_HOSTED` to 2. The game endpoints occupy `CC_MAX_HOSTED` consecutive UDP ports. Players need outgoing UDP access to the broker and their assigned game endpoint. Server firewall ingress must include exactly those ports. Worker capacity is an admission limit, not a measured simultaneous-match guarantee.

For production, use the unprivileged systemd service defined in the AWS template. Its writable state is separate from the immutable release. Run only one broker against a given state directory and worker port range.

Individual game workers accept `-mp-dedicated CONFIG_PATH`. The private UTF-8 configuration uses `key=value`: `port`, `owner_token`, `room_name`, optional `password`, `idle_seconds`, and `status_file`. The broker generates configurations and owner tokens; administrators should let it manage them. The status file exposes readiness and aggregate player/match state without credentials. Read broker metrics and per-room logs through the state directory.

## AWS deployment

The new CloudFormation stack is isolated from the existing relay. It creates its own VPC, public subnet, UDP security group, stable Elastic IP, encrypted volume, instance profile, launch template, and fixed-performance EC2 worker host. Administration uses SSM; inbound SSH is closed. IMDSv2 is required. The instance role can download only the selected immutable release object. The game and broker run unprivileged under systemd. Bootstrap verifies the runtime archive and every game data file, runs real UDP integration tests, and proves headless game startup before signaling readiness.

Bootstrap installs a checksum-pinned official AWS CLI v2 archive. Ubuntu 24.04's package repositories do not currently supply the `awscli` package; using the pinned installer keeps fresh-instance startup reproducible.

The initial instance choice is `c7i.xlarge` (4 vCPU/8 GiB), with two admitted hosted rooms. Burstable CPU-credit instances are excluded. This is a starting capacity that must be assessed using full-game timing measurements. Resizing and release upgrades replace the worker host and end its running rooms; the service address remains stable. Room state is ephemeral. Multiple fleet machines, cross-machine allocation, failover, and live match migration are not implemented by this initial stack.

Use an authenticated AWS CLI and an existing private encrypted artifact bucket. The existing `Services/RoomService/aws/artifacts.yaml` can create one. Prepare a reviewable change set:

```powershell
.\Services\DedicatedServer\aws\Deploy.ps1 -StackName cortex-hosted -Region us-east-1 `
  -ArtifactBucket YOUR-PRIVATE-BUCKET `
  -RuntimeArchive .\build-mp\dedicated-linux\cc-dedicated-ubuntu24-x86_64.tar.gz `
  -PlayersCIDR YOUR-PLAYER-IP-RANGE
```

Add `-Execute` to execute the reviewed change set. Explicitly select player ingress; a bounded CIDR is appropriate for private verification, and public play requires the intended public range. Deployment prints the hosted `ServiceAddress` and `InstanceId`. First verify real Windows clients against that endpoint, then change the bundled `MultiplayerService.txt`. The deployment script never changes client configuration automatically.

Compute, encrypted EBS, public IPv4, and outgoing gameplay traffic incur ongoing charges. EC2 detailed CPU and network metrics are enabled. Use SSM to inspect `systemctl status cc-dedicated`, `journalctl -u cc-dedicated`, and `/var/lib/cc-dedicated/metrics.json`. On process failure systemd kills the whole service group and restarts the broker; clients recreate rooms after a broker restart.

Reference: [EC2 CloudFormation resources](https://docs.aws.amazon.com/AWSCloudFormation/latest/TemplateReference/aws-resource-ec2-instance.html), [Ubuntu images on AWS](https://ubuntu.com/aws/docs/aws-how-to/instances/find-ubuntu-images/), and [Session Manager](https://docs.aws.amazon.com/systems-manager/latest/userguide/session-manager.html).
