param([Parameter(Mandatory)][string]$GameDirectory, [ValidatePattern('^mp-\d+\.\d+\.\d+(\.\d+)?$')][string]$InstalledVersion = 'mp-0.0.0')
$ErrorActionPreference = 'Stop'
$game = (Resolve-Path -LiteralPath $GameDirectory).Path
if (!(Test-Path -LiteralPath (Join-Path $game 'Cortex Command.exe'))) { throw 'Select the folder containing Cortex Command.exe.' }
$updater = Join-Path $PSScriptRoot 'CortexUpdater.exe'
if (!(Test-Path -LiteralPath $updater)) { $updater = Join-Path (Split-Path -Parent $PSScriptRoot) 'build-mp/updater/CortexUpdater.exe' }
if (!(Test-Path -LiteralPath $updater)) { throw 'Use the released updater installer, or run Tools/BuildUpdater.ps1.' }
Copy-Item -LiteralPath $updater -Destination (Join-Path $game 'CortexUpdater.exe') -Force
@{schema=1;repository='Gohostieee/Cortex-Command-Community-Project';tag=$InstalledVersion} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $game 'updater.json') -Encoding UTF8
Start-Process -FilePath (Join-Path $game 'CortexUpdater.exe') -ArgumentList @('--root', ('"' + $game + '"')) -WorkingDirectory $game
