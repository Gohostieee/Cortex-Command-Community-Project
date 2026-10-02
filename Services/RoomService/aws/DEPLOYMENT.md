# Hosted multiplayer deployment

Provisioned with the AWS CLI on 2026-10-02. The game includes this service address; players do not need AWS accounts.

| Setting | Deployed value |
| --- | --- |
| Game server address | `54.164.52.173:8001` |
| AWS region | `us-east-1` |
| Service stack | `cortex-multiplayer` |
| Artifact stack | `cortex-multiplayer-artifacts` |
| Instance | `i-0c9e7cef696cb8c2b` |
| System service | `cc-room.service`, enabled at boot |
| Instance size | `t3.small`, standard CPU credits |
| Limits | 16 rooms, 200 Mbps aggregate relay ceiling |
| Public ingress | UDP 8001 only |

The Elastic IP stays allocated across instance restart/replacement. The service automatically restarts after process failure. Rooms are ephemeral and must be recreated after a service restart. This is one instance; capacity limits do not establish a tested concurrent-room guarantee. Compute, storage, public IPv4, and outgoing traffic incur ongoing AWS charges.

## Administration

Use your own authenticated AWS CLI. Open Session Manager for this instance, or use SSM Run Command (`AWS-RunShellScript`) if the local Session Manager plugin is unavailable:

```powershell
aws ssm start-session --target i-0c9e7cef696cb8c2b --region us-east-1
```

Inside the instance:

```sh
systemctl status cc-room
journalctl -u cc-room --no-pager -n 40
cat /var/lib/cc-room/metrics.json
```

Metrics contain aggregate rooms/connections, relayed bytes, and dropped packets. The host runs game simulation and rendering. The relay runs as the unprivileged `cc-room` user; administration uses SSM without inbound SSH.

## Upgrade

Run the deployment script from a source checkout. Obtain the existing artifact bucket name:

```powershell
aws cloudformation describe-stacks --stack-name cortex-multiplayer-artifacts `
  --region us-east-1 --query 'Stacks[0].Outputs[?OutputKey==`ArtifactBucket`].OutputValue' --output text
```

Pass that name as `-ArtifactBucket`:

```powershell
.\Services\RoomService\aws\Deploy.ps1 -StackName cortex-multiplayer `
  -Region us-east-1 -ArtifactBucket YOUR-EXISTING-BUCKET `
  -PlayersCIDR 0.0.0.0/0 -MaxRooms 16 -MaxMbps 200
```

Review the prepared change set. Execute it using CloudFormation, or rerun the script with `-Execute` after review. Source and configuration updates replace the instance and interrupt all active rooms. The address remains stable. After readiness:

```powershell
.\build-mp\relay-tests.exe 8001 54.164.52.173
.\Tests\RunMultiplayerTests.ps1 -Smoke -Guests 3 -Relay -ServiceAddress 54.164.52.173:8001
```

Both CloudFormation stacks reached `CREATE_COMPLETE`. Windows clients passed the public relay suite. The freshly extracted Windows game passed its four-instance native verification through AWS. With no rooms active, a controlled process failure confirmed automatic recovery with a new process; the public relay suite then passed again. See [the multiplayer guide](../../../Documentation/Multiplayer.md) for the exact test scope and performance measurements. The GitHub Actions relay workflow is present, but its first run was blocked by the GitHub account billing lock before executing any steps.
