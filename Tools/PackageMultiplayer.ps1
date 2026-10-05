param(
    [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9_.-]+$')][string]$Version,
    [string]$RuntimeDirectory = '',
    [ValidatePattern('^[^\\/]+\.exe$')][string]$GameExecutable = 'Cortex Command.exe'
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$endpointFile = Join-Path $taskRoot 'MultiplayerService.txt'
if (!(Test-Path -LiteralPath $endpointFile)) { throw 'Configure the bundled MultiplayerService.txt before packaging.' }
$endpoint = (Get-Content -LiteralPath $endpointFile -Raw).Trim()
if ($endpoint -notmatch '^[A-Za-z0-9.-]+:[0-9]+$') { throw 'The bundled server address must be hostname:port or IPv4:port.' }
$exe = Join-Path $taskRoot $GameExecutable
if (!(Test-Path -LiteralPath $exe)) { throw 'Build the Final x64 game before packaging.' }
if (!$RuntimeDirectory) {
    $redist = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Redist\MSVC'
    $runtimeVersion = Get-ChildItem -LiteralPath $redist -Directory | Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
    if (!$runtimeVersion) { throw 'Provide the Visual C++ x64 redistributable RuntimeDirectory.' }
    $RuntimeDirectory = Join-Path $runtimeVersion.FullName 'x64/Microsoft.VC143.CRT'
}
foreach ($runtime in @('msvcp140.dll','msvcp140_atomic_wait.dll','vcruntime140.dll','vcruntime140_1.dll')) {
    if (!(Test-Path -LiteralPath (Join-Path $RuntimeDirectory $runtime))) { throw "Required runtime missing: $runtime" }
}
$releaseRoot = Join-Path $taskRoot 'build-mp/releases'
New-Item -ItemType Directory -Path $releaseRoot -Force | Out-Null
$stage = Join-Path $releaseRoot ("$Version-" + [guid]::NewGuid().ToString('N'))
$game = Join-Path $stage 'CortexCommand-Multiplayer'
New-Item -ItemType Directory -Path $game -Force | Out-Null
foreach ($path in @('Data','Licences','LICENSE','MultiplayerService.txt')) {
    Copy-Item -LiteralPath (Join-Path $taskRoot $path) -Destination $game -Recurse
}
Copy-Item -LiteralPath $exe -Destination (Join-Path $game 'Cortex Command.exe')
Copy-Item -LiteralPath (Join-Path $taskRoot 'external/lib/win/fmod.dll') -Destination $game
Get-ChildItem -LiteralPath $RuntimeDirectory -Filter '*.dll' | Copy-Item -Destination $game
$commit = & git -C $taskRoot rev-parse HEAD
if ($LASTEXITCODE) { throw 'Cannot determine the source revision.' }
@"
Cortex Command Multiplayer - $Version (Windows x64)

Extract this entire folder, then open Cortex Command.exe.
Choose Multiplayer, set your name, and choose Host or Join.
Online - AWS hosted runs the match on the dedicated server.
Host: Create room, copy the code, and share it with your friends.
Join: Paste that code, select your team, and choose Ready.
The room owner starts the match when everyone is ready. Up to four players.
Every player renders locally; AWS handles physics, AI, damage and terrain.
Leaving an AWS-hosted room keeps the match running for the other players.
Close room is a separate owner action that ends the match for everyone.

The bundled room server is $endpoint. Players do not forward router ports.
To use another server: Multiplayer > Connection settings > Server IP or hostname
and Save server address. Use default server restores the bundled address.
Everyone in a room must use the same server and game build.
Guests render locally at their own resolution, set in the normal game options.

Windows 10/11 x64 and an OpenGL-capable GPU are required. Game data and the
Visual C++ runtime are included. Traffic uses UDP and is not encrypted.
Create a new room after a server restart. Custom mods are not included.

Source and license:
https://github.com/Gohostieee/Cortex-Command-Community-Project/tree/$commit
GNU AGPL v3; see LICENSE and Licences for component notices.
Build source revision: $commit
"@ | Set-Content -LiteralPath (Join-Path $game 'START-HERE.txt') -Encoding UTF8
$archive = Join-Path $releaseRoot "CortexCommand-Multiplayer-$Version-Windows-x64.zip"
if (Test-Path -LiteralPath $archive) { throw 'This version is already packaged; select a new version or move the existing archive.' }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $archive, [System.IO.Compression.CompressionLevel]::Optimal, $false)
$sha = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
"$sha  $([System.IO.Path]::GetFileName($archive))" | Set-Content -LiteralPath "$archive.sha256" -Encoding ASCII
[PSCustomObject]@{ Archive = $archive; Sha256 = $sha; GameDirectory = $game; SourceCommit = $commit }
