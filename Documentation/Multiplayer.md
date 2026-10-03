# Playing multiplayer

Multiplayer is available from the main menu. A room supports one host and up to three guests. The host runs the match and plays as player one; each guest gets their own camera, controls, buy menu, and positional sounds.

## Host a room

1. Choose **Multiplayer**, open **Host game**, and enter your player name.
2. Set a room name and optional password. The **Connection** page selects **Online - invitation codes** or direct LAN play. The bundled service is **54.164.52.173:8001**. To use another service, enter its **Room server** address and choose **Save server**. **Use default** restores the bundled address.
3. Open **Connection settings** to adjust upload if needed. Each guest draws at their own game resolution. Upload is a budget for each guest, not the whole room; it defaults to 24 Mbps per guest.
4. Choose **Create lobby**. Wait for your room code, then choose **Copy invite** and share it with guests.
5. Select an activity and compatible battlefield, with the same fonts, controls, sounds and menu skin used by regular game setup. Choose teams in the roster and use **Match rules and factions** for difficulty, gold, fog, scene deployment and factions. Guests can change their own teams and mark themselves ready. Changing match options clears guest readiness. The lobby explains what is required before the host can start.
6. Choose **Start match** after everyone is ready. Activities must support the player count and have enough distinct teams. AI teams are reserved for the activity.

Room-code play uses outgoing connections to the central relay, so hosts do not forward ports. The service must stay online and your network must allow outgoing UDP. To run your own service locally or on AWS, see [the room service guide](../Services/RoomService/README.md).

For direct LAN play, select **Local network / direct address** on the **Connection** page and use UDP port **8000** by default. Direct internet play still needs the selected port forwarded to the host and a reachable public IPv4 address.

## Join a room

1. Choose **Multiplayer**, open **Join game**, and set your player name.
2. Paste the host's ten-character **Invitation code** and enter the password if needed. Both players must use the same room service. For direct play, select **Local network / direct address** on the **Connection** page, enter the host address such as `192.168.1.20:8000`, or use **Find LAN games** to discover rooms on port 8000.
3. Choose **Join lobby**, select your team, and press **Ready up**. Room chat is available in the lobby and session menu. At compact resolutions it opens on its own page.
4. Configure your normal player-one controls in the game's options. Guests send their keyboard, mouse, and gamepad mappings as their assigned network player. Shop search text and navigation keys also travel to that player's menus. The host's controls and typing stay local.

All players should use the same game build and game/mod assets. The handshake rejects a different game version or multiplayer protocol. It does not currently compare every installed mod or asset checksum; missing guest sound files cannot be played.

## During a match

- **Escape** opens the session menu. The host can also use their player-one Start control. Guests use **Escape** for the session menu.
- **Resume** returns control to the match. Opening the menu releases that player's controls while the other players continue.
- The host can **Return everyone to lobby** and launch another match with new options. The host's ordinary single-player pause, quick-load, and restart shortcuts do not bypass the room lifecycle.
- After game over, the results remain visible for five seconds, then everyone automatically returns to the same lobby. Activity exits also return the group to the lobby. The invitation code, player slots, teams, settings and chat stay in place; guests ready up again and the host chooses **Play again**. Input, synchronized scene state and looping sounds are cleared between rounds.
- The lobby and session menu show the guest's connection latency; the session menu also shows local rendering frame rate. Loading and reconnecting have dedicated screens.
- A dropped connection triggers automatic reconnect attempts for 20 seconds. The host reserves the player's slot for 60 seconds. Rejoining during that window restores the same player and ongoing looping sounds. Controls expire after 250 ms without fresh input, so a disconnected player does not keep firing or moving.
- A match accepts returning players; new players join in the lobby. Before another match, the host must wait for disconnected players or **Release slot**. Closing the room tells guests the host left.

Command-line equivalents are `"Cortex Command.exe" -mp-host 8000` and `"Cortex Command.exe" -mp-join 192.168.1.20:8000`. First-run Windows setup generates **Host Multiplayer.bat**. The old `-server` launcher belongs to the retired multiplayer implementation.

For code-based command-line play, set `CCCP_MP_SERVICE` to the service hostname/IP and port, then use `-mp-host 8000` to create a code or `-mp-join ABCDE-F2345` to join it. The host port argument is unused for outgoing code connections.

## Implementation

The host owns simulation, terrain, physics, AI, Lua, and activities. Guests render a retained native scene locally with the game's palette, sprite assets, and GPU renderer. The host no longer renders, reads back, compresses, or sends completed guest images.

Authoritative snapshots target 20 updates per second. Stable object identities preserve native drawing order, sprite frames, attachment parents, transforms, lifecycle, and camera state. Guests interpolate positions, rotations, parent-relative joints, and camera movement with a 75 ms presentation buffer. A bounded 100 ms continuation covers brief interruptions; longer interruptions freeze motion. Scene and angle wrapping follow the shortest path. Teleports and transient effects do not invent intermediate motion. A new match or reconnect rebuilds the baseline.

Palette and RGBA resources are content-addressed and cached. Backgrounds, foreground terrain, parallax, and per-team fog use 64-pixel tiles with a margin around the camera. Changed tiles receive new content identities. Resources travel reliably; pose snapshots use independent, replaceable datagrams with one XOR parity fragment per eight data fragments. The first baseline waits for its assets before enabling controls. Subsequent poses keep moving while an existing object retains its last complete visual until its new asset arrives. Downloaded assets survive reconnects and rounds within the same room. Assemblies, packet queues, dimensions, counts, timestamps, and the 128 MB cache are bounded; eviction protects the active timeline and retained visuals. Missing evicted resources can be requested again.

Native drawing adapters retain the host's HUD, fonts, buy menus, script primitives, damage flashes, and post effects as scene commands. Actor labels follow interpolated actor positions. Timestamped bullet trails preserve effects that begin and end between snapshots. Guests have passive presentation replicas: constructing gameplay Actors would execute scripts, so guests do not clone or independently simulate those objects.

Packets use explicit fixed-width, big-endian serialization with game version, session, and match identifiers. Only registered peers can submit gameplay messages. Relative mouse movement and button transitions use cumulative counters to survive lost, duplicated, or reordered inputs. Controls expire after 250 ms without new input. New matches clear previous input, presentation state, and sounds.

The room-code relay forwards opaque game packets inside a stable transport envelope. It works with the existing public v1 broker without restarting its rooms. Direct connections have a separate reliable resource lane; the v1 relay carries resources on its reliable audio lane. A 128 KB resource window requires receipts from the actual guest, preventing the relay's first-leg acknowledgements from admitting an unbounded download backlog. Control, inputs, and pose delivery remain independent. The service needs no game assets or GPU and runs no game simulation.

Audio uses reliable ordered messages, coalesces repeated property changes, preserves stop/fade events, and rebuilds active loops after reconnect. During initial troop placement, guests render the world without the team's fog layer. The exploration map stays intact, and combat restores the activity's fog.

Local rendering removes the network-update ceiling on visual frame rate. Input still travels to the authoritative host, and there is no client-side gameplay prediction or replicated collision simulation. Performance depends on the guest GPU, scene complexity, and host/network capacity. Standard game rendering paths have been exercised; arbitrary mods, physical controllers, and Linux/macOS game builds still need player acceptance. All players must use this new game build; framebuffer-streaming builds use an incompatible gameplay protocol. There is no automated public matchmaking or Steam invite integration. Relay restarts expire all codes, and transport traffic is not encrypted.

## Verification

On Windows with Visual Studio 2022 C++ tools and the repository dependencies installed, run from the checkout:

```powershell
.\Tests\RunMultiplayerTests.ps1
.\Tests\RunMultiplayerTests.ps1 -Smoke
.\Tests\RunMultiplayerTests.ps1 -Smoke -Guests 3 -Relay -Loss
.\Tests\RunMultiplayerTests.ps1 -Smoke -Deployment -Guests 3 -Relay
.\Tests\RunMultiplayerTests.ps1 -Smoke -Guests 3 -Relay -Loss -ServiceAddress 54.164.52.173:8001
.\Tests\RunMultiplayerTests.ps1 -Smoke -Guests 3 -Relay -Loss -GuestResolutions @('640x360','1280x720','1920x1080') -ServiceAddress 54.164.52.173:8001
.\Tests\RunMultiplayerCursorTests.ps1
```

The standalone suite checks serialization and resource integrity, interpolation between 20 Hz states, scene and angle wrapping, attachment rotation, lifecycle timing, short-lived trails, bounded continuation, malformed packets, resource assembly and parity repair, every mapped action, input recovery/expiry, real UDP resources, passwords, LAN discovery, room capacity, and reconnects. Meson registers `multiplayer-tests` for other platforms. The relay suite exercises outgoing-only rooms, three independent inputs and state streams, exact payloads near MTU, retained resources, passwords, capacity, isolation, guest reconnects, host resume, and closure. It has passed against the deployed public relay with the new game protocol.

The native smoke test builds and runs a real host and one to three guests using isolated settings with local mods disabled. It operates native Start/Ready/Resume menu buttons, exercises two activities, moves and fires every remote actor, requires changed foreground terrain resources to reach every guest, verifies GUI keys/text, chat, looping audio replay/stop, reconnects every slot, and returns everyone to the same lobby after activity exit and game over. Match phases wait for fresh state acknowledgements from every guest and retain a hard 90-second timeout. Actual OpenGL captures and logs are saved in ignored `build-mp/` files. It also requires changing intermediate guest frames between authoritative updates. `-Loss` deliberately discards one data fragment in each snapshot parity group while leaving reliable resources intact. `-GuestResolutions` sets each guest's own logical game resolution.

On 2026-10-03, native two-instance combat rendered approximately 146–171 guest frames per second while receiving approximately 19 state updates per second. A host and three guests passed two matches through the local relay with deliberate snapshot loss; steady combat guest rendering was approximately 141–179 FPS. All instances shared one Windows computer. These measurements demonstrate independent local rendering, and do not guarantee those rates on other hardware or networks.

The four-instance test also passed through the deployed public relay with guests at 640 × 360, 1280 × 720, and 1920 × 1080 and deliberate snapshot loss. Steady second-match intervals measured approximately 215–261, 110–114, and 70–78 local FPS respectively, with roughly 16–18 received states per second on the 1080p guest. The test required controls, GUI input, audio replay/stop, chat, every guest reconnecting, two activities, intermediate movement, and return to the same lobby. Initial scene downloads remain a loading phase.

The deployment variant initializes opaque fog, places brains and troops, lets guests wait while the host continues deploying, and enters combat. It checks rendered host and guest views for a black map and verifies preserved combat fog. The menu check captures 24 native menu/gameplay states, checks contents, bounds, overlapping controls, cursor pixels, and clean shutdown at 640 × 360, 960 × 540, or 1280 × 720. Smoke hooks are opt-in and inactive in normal rooms.

Use `-GameDirectory PATH -GameExecutable 'Cortex Command.replica.exe'` to verify a previously built executable without rebuilding it. `Tools/PackageMultiplayer.ps1 -Version YOUR-VERSION` packages the Windows x64 game, data, runtime dependencies, default endpoint, licenses, and source-revision link. It excludes personal settings and mods and emits a SHA-256 file beside the ZIP.
