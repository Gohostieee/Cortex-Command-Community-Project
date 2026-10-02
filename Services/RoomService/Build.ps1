param([switch]$Test)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Push-Location $taskRoot
try {
    New-Item -ItemType Directory -Path build-mp -Force | Out-Null
    $devShell = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat'
    $common = ' /nologo /std:c++20 /EHsc /O2 /MD /DNOMINMAX /I Source\System /I external\sources\RakNet\include '
    $link = ' /link external\sources\RakNet\_Bin\raknet-release.lib ws2_32.lib winmm.lib'
    & cmd /c ('call "' + $devShell + '" -arch=x64 -host_arch=x64 > nul && cl' + $common + 'Services\RoomService\RoomService.cpp /Fo:build-mp\ /Fe:build-mp\cc-room-service.exe' + $link)
    if ($LASTEXITCODE) { throw 'Room service build failed.' }
    if ($Test) {
        & cmd /c ('call "' + $devShell + '" -arch=x64 -host_arch=x64 > nul && cl' + $common + 'Tests\MultiplayerRelayTests.cpp Source\System\MultiplayerTransport.cpp /Fo:build-mp\ /Fe:build-mp\relay-tests.exe' + $link)
        if ($LASTEXITCODE) { throw 'Relay test build failed.' }
        $service = Start-Process -FilePath (Join-Path $taskRoot 'build-mp/cc-room-service.exe') -ArgumentList '--bind 127.0.0.1 --port 38997 --max-rooms 8 --metrics-file build-mp/relay-metrics.json' -WorkingDirectory $taskRoot -WindowStyle Hidden -RedirectStandardOutput 'build-mp/relay-service.log' -RedirectStandardError 'build-mp/relay-service-error.log' -PassThru
        try { & ./build-mp/relay-tests.exe; if ($LASTEXITCODE) { throw 'Relay integration tests failed.' } }
        finally { if (!$service.HasExited) { Stop-Process -Id $service.Id } }
    }
} finally { Pop-Location }
