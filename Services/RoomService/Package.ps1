$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Push-Location $taskRoot
try {
    New-Item -ItemType Directory -Path build-mp -Force | Out-Null
    $archive = Join-Path $taskRoot 'build-mp/room-service-source.tar.gz'
    & tar -czf $archive Services/RoomService/CMakeLists.txt Services/RoomService/RoomService.cpp Source/System/MultiplayerProtocol.h Source/System/MultiplayerRelay.h Source/System/MultiplayerTransport.h Source/System/MultiplayerTransport.cpp Tests/MultiplayerRelayTests.cpp external/sources/RakNet/include external/sources/RakNet/src
    if ($LASTEXITCODE) { throw 'Source packaging failed.' }
    $sha = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    [PSCustomObject]@{ Archive = $archive; Sha256 = $sha; Key = "room-service/releases/$sha.tar.gz" }
} finally { Pop-Location }
