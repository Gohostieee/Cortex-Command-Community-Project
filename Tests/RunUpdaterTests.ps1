param()
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
& "$taskRoot/Tools/BuildUpdater.ps1"
$compiler = Join-Path $env:windir 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
$output = Join-Path $taskRoot 'build-mp/updater-tests'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$core = [IO.Path]::GetFullPath((Join-Path $taskRoot 'Tools/Updater/UpdateCore.cs'))
$tests = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'UpdaterTests.cs'))
$mock = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'UpdaterMockGame.cs'))
& $compiler /nologo /target:exe /platform:x64 "/out:$output/UpdaterTests.exe" /r:System.IO.Compression.dll /r:System.IO.Compression.FileSystem.dll /r:System.Web.Extensions.dll $core $tests
if ($LASTEXITCODE) { throw 'Updater tests did not compile.' }
& $compiler /nologo /target:exe /platform:x64 "/out:$output/MockGame.exe" /r:System.Web.Extensions.dll $mock
if ($LASTEXITCODE) { throw 'Mock game did not compile.' }
$fixture = Join-Path $output ([guid]::NewGuid().ToString('N'))
& "$output/UpdaterTests.exe" $fixture "$output/MockGame.exe" "$taskRoot/build-mp/updater/CortexUpdater.exe"
if ($LASTEXITCODE) { throw 'Updater checks failed.' }
