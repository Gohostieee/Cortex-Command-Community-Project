# Windows automatic updates

The full multiplayer release includes `CortexUpdater.exe`, `updater.json`, and
`update-files.json`. Open **Cortex Command.exe** normally. Before game data loads,
it hands startup to the updater; after checking or installing the release the
updater launches the game with the original arguments. Updates never run in an
active match. Source checkouts without the bundled updater launch directly.

The updater selects the highest versioned published `mp-YYYY.MM.DD[.N]` release
from this fork, including multiplayer prereleases. Drafts, older versions,
unrelated tags, and packages without GitHub's SHA-256 digest are skipped. The
required asset name is `CortexCommand-Multiplayer-TAG-Windows-x64.zip`.

Downloads use HTTPS and are checked against the published size and SHA-256
before extraction. Extraction rejects paths that escape the package, duplicate
paths, linked entries and invalid Windows paths. Only release-managed files are
installed; settings, saves, `Userdata`, `Mods`, and `ScreenShots` are preserved.
Deep paths in bundled game data are supported without a Windows settings change.
Files removed from a newer release are deleted only if the previous release
listed them as managed. Unmanaged files are kept.

The running updater copies a helper outside the game folder so that it can
replace both itself and the game. It backs up affected files before writing,
records a recovery journal, and restores the previous installation on an
installation failure. A later startup recovers an interrupted journal before
checking for another update. The helper starts the updated game after successful
installation. Installation requires write access to the game folder.

If the release check is unavailable, the installed game starts. A failed
download or validation offers **Play installed version** and **Retry update**.
Use `Cortex Command.exe --no-update` to bypass the startup check explicitly.
Logs and transaction files are under `%LOCALAPPDATA%/CortexCommand/Updater/`,
separated by installation folder. Completed transactions are cleaned at a later
updater startup.

## Existing installations without an updater

Install the new full ZIP once, or extract the released updater installer ZIP
and run:

```powershell
.\InstallUpdater.ps1 -GameDirectory 'C:\Games\CortexCommand-Multiplayer'
```

This installs and opens the updater, which downloads the current full release.
After that, the updated game executable checks automatically on normal startup.
An old executable cannot acquire this behavior without that initial installation.
The standalone updater can be opened again if the first check was offline.

## Building, testing, and releasing

Windows 10/11 with .NET Framework 4.8 is supported. The build uses the installed
Windows C# compiler and does not require a separate .NET SDK or NuGet packages.

```powershell
.\Tools\BuildUpdater.ps1
.\Tests\RunUpdaterTests.ps1
.\Tools\PackageMultiplayer.ps1 -Version mp-2026.10.11.2
```

Commit all intended source and build the Final x64 game before packaging. The
packager refuses a dirty checkout, includes the updater and matching source,
and writes the installed tag and managed-file list. Upload the resulting ZIP
and checksum to a GitHub release with that exact tag. GitHub Actions separately
checks the updater when its source changes; local checks remain available when
Actions cannot run.

For read-only diagnostics, run `CortexUpdater.exe --root GAME_FOLDER --check-only`.
`--update-only` uses the same verified installation path without starting the
game, for release qualification. The helper may finish after this command exits;
inspect the installed tag and update log for its final result.

Publishing a client release does not deploy the AWS worker. Keep hosted workers
and clients matched using `Services/DedicatedServer/aws/Deploy.ps1` and verify
the public endpoint after rollout.
