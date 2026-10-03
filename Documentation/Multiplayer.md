# Playing multiplayer

Multiplayer is available from the main menu. A room supports one host and up to three guests. The host runs the match and plays as player one; each guest gets their own camera, controls, buy menu, and positional sounds.

## Host a room

1. Choose **Multiplayer**, enter your player name, and open **Host**.
2. Leave **Use room codes** enabled. This fork includes the AWS service at **54.164.52.173:8001**. Set a room name and optional password. To use another service, expand **Connection settings**, enter its **Server IP or hostname**, and choose **Save server address**. **Use default server** restores the bundled AWS address.
3. Start with **640 × 360** stream quality. **960 × 540** uses more upload bandwidth and rendering time. The upload slider is a budget for each guest, not the whole room; it defaults to 24 Mbps per guest.
4. Choose **Create room**. Wait for your room code, then choose **Copy code** and share it with guests.
5. Select an activity and compatible scene. Choose teams, difficulty, gold, fog, scene deployment, and team factions. Guests can change their own teams and mark themselves ready. Changing match options clears guest readiness.
6. Choose **Start match** after everyone is ready. Activities must support the player count and have enough distinct teams. AI teams are reserved for the activity.

Room-code play uses outgoing connections to the central relay, so hosts do not forward ports. The service must stay online and your network must allow outgoing UDP. To run your own service locally or on AWS, see [the room service guide](../Services/RoomService/README.md).

For direct LAN play, disable **Use room codes** and use UDP port **8000** by default. Direct internet play still needs the selected port forwarded to the host and a reachable public IPv4 address.

## Join a room

1. Choose **Multiplayer**, set your player name, and open **Join**.
2. Paste the host's ten-character **Room code** and enter the password if needed. Both players must use the same room service. For direct play, disable **Use room codes**, enter the host address such as `192.168.1.20:8000`, or use **Find LAN rooms** to discover rooms on port 8000.
3. Choose **Join room**, select your team, and press **Ready**. Room chat is available before and during a match.
4. Configure your normal player-one controls in the game's options. Guests send their keyboard, mouse, and gamepad mappings as their assigned network player. Shop search text and navigation keys also travel to that player's menus. The host's controls and typing stay local.

All players should use the same game build and game/mod assets. The handshake rejects a different game version or multiplayer protocol. It does not currently compare every installed mod or asset checksum; missing guest sound files cannot be played.

## During a match

- **Escape** opens the session menu. The host can also use their player-one Start control. Guests use **Escape** for the session menu.
- **Resume** returns control to the match. Opening the menu releases that player's controls while the other players continue.
- The host can **Return everyone to lobby** and launch another match with new options. The host's ordinary single-player pause, quick-load, and restart shortcuts do not bypass the room lifecycle.
- The guest overlay shows ping, received frame rate, and when frames stop arriving.
- A dropped connection triggers automatic reconnect attempts for 20 seconds. The host reserves the player's slot for 60 seconds. Rejoining during that window restores the same player and ongoing looping sounds. Controls expire after 250 ms without fresh input, so a disconnected player does not keep firing or moving.
- A match accepts returning players; new players join in the lobby. Before another match, the host must wait for disconnected players or **Release slot**. Closing the room tells guests the host left.

Command-line equivalents are `"Cortex Command.exe" -mp-host 8000` and `"Cortex Command.exe" -mp-join 192.168.1.20:8000`. First-run Windows setup generates **Host Multiplayer.bat**. The old `-server` launcher belongs to the retired multiplayer implementation.

For code-based command-line play, set `CCCP_MP_SERVICE` to the service hostname/IP and port, then use `-mp-host 8000` to create a code or `-mp-join ABCDE-F2345` to join it. The host port argument is unused for outgoing code connections.

## Implementation

The replacement uses the current GPU renderer. The host owns simulation, terrain, physics, Lua, and activities. Each guest receives complete independent RGB565 frames compressed with LZ4 HC, aiming for 30 frames per second when the host and link can sustain it. Moderate compression levels reduce terrain upload cost, with a faster level for the larger stream. Guests upload RGB565 directly to the GPU. Every eight fragments include XOR parity to repair one lost fragment. A later complete frame recovers from heavier loss without depending on an earlier frame. Incomplete frames never replace the displayed view.

Packets use explicit fixed-width, big-endian serialization with version, session, and match identifiers. Only registered peers can submit gameplay messages. Strings, dimensions, datagrams, frame assemblies, and sound channels have limits. Relative mouse movement and button transitions use cumulative counters to survive lost, duplicated, or reordered input snapshots. New matches clear previous input, frames, and sounds.

Encoding workers own copied pixels; engine access stays on the main thread. The frame window follows measured capture-to-acknowledgement time, including both relay legs, and stays bounded at eight frames. Upload budgets, stale-frame expiry, and transport backpressure also prevent unlimited frame queues. Audio uses reliable ordered messages, coalesces repeated property changes, preserves stop/fade events, and rebuilds active loops after reconnect. Severely overloaded peers reconnect rather than accumulating an unbounded audio queue. Shutdown joins outstanding encoding work before releasing transport or engine resources.

During initial troop placement, the renderer shows the world without drawing the team's fog layer. It leaves the exploration map intact, so combat restores the activity's existing fog. This avoids a black placement screen when an activity initializes fog before players finish deploying.

This is host-rendered multiplayer and includes network input latency. The standalone service adds invitation codes and relayed connections; it does not run game simulation. There is no client prediction, replicated local simulation, automated public matchmaking, or Steam invites. Service restarts expire all codes. Transport traffic is not encrypted.

## Verification

On Windows with Visual Studio 2022 C++ tools and the repository dependencies installed, run from the checkout:

```powershell
.\Tests\RunMultiplayerTests.ps1
.\Tests\RunMultiplayerTests.ps1 -Smoke
.\Tests\RunMultiplayerTests.ps1 -Smoke -Guests 3
.\Tests\RunMultiplayerTests.ps1 -Smoke -Guests 3 -Relay
.\Tests\RunMultiplayerTests.ps1 -Smoke -Deployment -Guests 3 -Relay
.\Tests\RunMultiplayerTests.ps1 -Smoke -Guests 3 -Relay -ServiceAddress 54.164.52.173:8001
.\Tests\RunMultiplayerCursorTests.ps1
```

The standalone suite checks serialization bounds, every mapped action, movement and button recovery, duplicate/reordered/wrapped inputs, control expiry, frame repair and heavier-loss recovery, 20,000 malformed packet bodies, real UDP frame delivery, passwords, LAN discovery replies, four-player transport capacity, rejection of a fifth player, disconnects, and repeated shutdown/startup. Meson also registers `multiplayer-tests` for builds on other platforms.

The native smoke test builds the complete Windows x64 game and runs a real host and one to three guests with isolated settings. It exercises two different activities and both stream sizes, returns to the lobby, reconnects every original guest slot, checks each remote actor's movement and firing, verifies GUI navigation and text routing, chat, and looping-sound replay/stop, and captures the actual OpenGL output. Logs and screenshots are written to ignored `build-mp/` files. Smoke flags are opt-in test hooks and are not used by normal rooms.

The deployment variant initializes an opaque fog map, places brains and troops, marks guests ready while the host continues deploying, and then enters combat. It checks the host's rendered view and every guest's decoded view for a black map, verifies that combat preserves fog, reports delivered frame rates, and captures all four stages. The original fog fixture left only about 1% of the guest image visible. A separate deterministic check drives the production frame gate over a healthy 120 ms round trip; the old two-frame window produced 16.8 FPS, while the adaptive window produces 30.2 FPS. That gate measurement isolates acknowledgement pacing; actual frame rate also depends on rendering, encoding, upload capacity and packet loss.

After these fixes on 2026-10-02, a host and three guests passed the deployment variant through a local relay, including preserved combat fog. All four instances also passed the normal two-match smoke test through the deployed AWS relay, including movement/firing, GUI keys/text, audio replay/stop and reconnects. Guest checkpoints measured about 19–21 FPS at 640 × 360 and 12–17 FPS at 960 × 540 on one shared Windows computer. Separate computers and networks still need player acceptance; these measurements do not guarantee a particular frame rate.

The cursor check renders twelve menu, lobby, connection, session, and gameplay states through the actual game renderer. It asserts that foreground cursor geometry exists when menus are visible and disappears during gameplay and after closing the menu, and captures each OpenGL view. This check reproduced the original missing pointer (`software=0 rendered=0`) and passed after enabling the menu's software cursor. Opening the multiplayer menu also releases captured mouse input. The opt-in `CCCP_MPSMOKE_CURSOR=1` fixture does not start or join network rooms; it is separate from the live multiplayer smoke test.

The local two-instance direct combat run received approximately 25–30 FPS at 640 × 360. The four-instance direct run measured approximately 15–23 FPS at 960 × 540. The four-instance local relay run passed for every guest at about 20 FPS at its second-match verification checkpoints. Windows clients also passed the relay suite against the native Linux service.

On 2026-10-02 the Windows release ZIP was extracted into a fresh directory, and all four actual game instances passed through the deployed **54.164.52.173:8001** AWS relay. Both stream sizes, three simultaneous reconnects, remote movement/firing, GUI input, chat, and looping-sound replay/stop passed. The guests measured about 13 FPS at the 640 × 360 checkpoints and 10–11 FPS at 960 × 540; each received 32–34 fresh second-match frames before its pass. All four game instances shared one Windows computer while traffic crossed the public internet to AWS. These are verification measurements, not performance guarantees. Full Windows builds passed with existing LuaJIT, luabind, and build-property warnings. Separate players/computers/networks, listening quality, physical controllers, and Linux/macOS game builds remain unverified.

To check the portable release itself, extract it and run:

```powershell
.\Tests\RunMultiplayerTests.ps1 -Smoke -Guests 3 -Relay `
  -ServiceAddress 54.164.52.173:8001 -GameDirectory 'PATH-TO-EXTRACTED-GAME'
```

`Tools/PackageMultiplayer.ps1 -Version YOUR-VERSION` packages a built Windows x64 executable, game data, FMOD, Visual C++ runtime, default endpoint, licenses, and source-revision link. It excludes personal settings and mods and emits a SHA-256 file alongside the ZIP.
