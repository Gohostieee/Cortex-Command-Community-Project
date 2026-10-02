# Playing multiplayer

Multiplayer is available from the main menu. A room supports one host and up to three guests. The host runs the match and plays as player one; each guest gets their own camera, controls, buy menu, and positional sounds.

## Host a room

1. Choose **Multiplayer**, enter your player name, and open **Host**.
2. Leave **Join with room codes** enabled. Configure the room service once under **Connection settings**, or bundle its endpoint in `MultiplayerService.txt`. Set a room name and optional password.
3. Start with **640 × 360** stream quality. **960 × 540** uses more upload bandwidth and rendering time. The upload slider is a budget for each guest, not the whole room; it defaults to 24 Mbps per guest.
4. Choose **Create room**. Wait for your room code, then choose **Copy code** and share it with guests.
5. Select an activity and compatible scene. Choose teams, difficulty, gold, fog, scene deployment, and team factions. Guests can change their own teams and mark themselves ready. Changing match options clears guest readiness.
6. Choose **Start match** after everyone is ready. Activities must support the player count and have enough distinct teams. AI teams are reserved for the activity.

Room-code play uses outgoing connections to the central relay, so hosts do not forward ports. The service must stay online and your network must allow outgoing UDP. To run the included service locally or on AWS, see [the room service guide](../Services/RoomService/README.md). No public service is preconfigured.

For direct LAN play, disable **Join with room codes** and use UDP port **8000** by default. Direct internet play still needs the selected port forwarded to the host and a reachable public IPv4 address.

## Join a room

1. Choose **Multiplayer**, set your player name, and open **Join**.
2. Paste the host's ten-character **Room code** and enter the password if needed. Both players must use the same room service. For direct play, disable **Join with room codes**, enter the host address such as `192.168.1.20:8000`, or use **Find LAN rooms** to discover rooms on port 8000.
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

The replacement uses the current GPU renderer. The host owns simulation, terrain, physics, Lua, and activities. Each guest receives complete independent RGB565 frames compressed with LZ4, aiming for 30 frames per second when the host and link can sustain it. Every eight fragments include XOR parity to repair one lost fragment. A later complete frame recovers from heavier loss without depending on an earlier frame. Incomplete frames never replace the displayed view.

Packets use explicit fixed-width, big-endian serialization with version, session, and match identifiers. Only registered peers can submit gameplay messages. Strings, dimensions, datagrams, frame assemblies, and sound channels have limits. Relative mouse movement and button transitions use cumulative counters to survive lost, duplicated, or reordered input snapshots. New matches clear previous input, frames, and sounds.

Encoding workers own copied pixels; engine access stays on the main thread. Frame acknowledgements, upload budgets, stale-frame expiry, and transport backpressure prevent unlimited frame queues. Audio uses reliable ordered messages, coalesces repeated property changes, preserves stop/fade events, and rebuilds active loops after reconnect. Severely overloaded peers reconnect rather than accumulating an unbounded audio queue. Shutdown joins outstanding encoding work before releasing transport or engine resources.

This is host-rendered multiplayer and includes network input latency. The standalone service adds invitation codes and relayed connections; it does not run game simulation. There is no client prediction, replicated local simulation, automated public matchmaking, or Steam invites. Service restarts expire all codes. Transport traffic is not encrypted.

## Verification

On Windows with Visual Studio 2022 C++ tools and the repository dependencies installed, run from the checkout:

```powershell
.\Tests\RunMultiplayerTests.ps1
.\Tests\RunMultiplayerTests.ps1 -Smoke
.\Tests\RunMultiplayerTests.ps1 -Smoke -Guests 3
.\Tests\RunMultiplayerTests.ps1 -Smoke -Guests 3 -Relay
```

The standalone suite checks serialization bounds, every mapped action, movement and button recovery, duplicate/reordered/wrapped inputs, control expiry, frame repair and heavier-loss recovery, 20,000 malformed packet bodies, real UDP frame delivery, passwords, LAN discovery replies, four-player transport capacity, rejection of a fifth player, disconnects, and repeated shutdown/startup. Meson also registers `multiplayer-tests` for builds on other platforms.

The native smoke test builds the complete Windows x64 game and runs a real host and one to three guests with isolated settings. It exercises two different activities and both stream sizes, returns to the lobby, reconnects every original guest slot, checks each remote actor's movement and firing, verifies GUI navigation and text routing, chat, and looping-sound replay/stop, and captures the actual OpenGL output. Logs and screenshots are written to ignored `build-mp/` files. Smoke flags are opt-in test hooks and are not used by normal rooms.

The local two-instance direct combat run received approximately 25–30 FPS at 640 × 360. The four-instance direct run measured approximately 15–23 FPS at 960 × 540. The four-instance relay run passed for every guest; all three received at least twenty fresh second-match frames and measured about 20 FPS at their verification checkpoints. Both stream sizes, three simultaneous reconnects, remote movement/firing, GUI input, chat, and looping-sound replay/stop passed. Windows clients also passed the relay suite against the native Linux service. These are local results, not internet performance promises. Full Windows builds passed with existing LuaJIT, luabind, and build-property warnings. Cross-machine internet acceptance, deployed AWS behavior, listening quality, physical controllers, and Linux/macOS game builds remain unverified.
