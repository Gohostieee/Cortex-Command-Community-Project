param([switch]$Test)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
function Wait-RoomServiceReady($Process, [string]$LogPath) {
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($Process.HasExited) { throw 'Room service exited before its test listener was ready.' }
        if ((Test-Path -LiteralPath $LogPath) -and ((Get-Content -LiteralPath $LogPath) -match '^Room service ready on UDP ')) { return }
        Start-Sleep -Milliseconds 50
    }
    throw 'Room service did not become ready for integration tests.'
}
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
        try { Wait-RoomServiceReady $service 'build-mp/relay-service.log'; & ./build-mp/relay-tests.exe; if ($LASTEXITCODE) { throw 'Relay integration tests failed.' } }
        finally { if (!$service.HasExited) { Stop-Process -Id $service.Id } }
        $workerState = Join-Path $taskRoot ('build-mp/hosted-broker-fixture-' + [Guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $workerState -Force | Out-Null
        $hostedArguments = '--bind 127.0.0.1 --port 38996 --max-rooms 8 --game-executable "' + (Join-Path $taskRoot 'build-mp/relay-tests.exe') + '" --game-directory "' + $taskRoot + '" --hosted-address 127.0.0.1 --game-port-base 39010 --max-hosted 1 --state-directory "' + $workerState + '"'
        $service = Start-Process -FilePath (Join-Path $taskRoot 'build-mp/cc-room-service.exe') -ArgumentList $hostedArguments -WorkingDirectory $taskRoot -WindowStyle Hidden -RedirectStandardOutput 'build-mp/hosted-broker.log' -RedirectStandardError 'build-mp/hosted-broker-error.log' -PassThru
        try { Wait-RoomServiceReady $service 'build-mp/hosted-broker.log'; & ./build-mp/relay-tests.exe --hosted 38996 127.0.0.1 $workerState; if ($LASTEXITCODE) { throw 'Hosted broker integration tests failed.' } }
        finally { if (!$service.HasExited) { Stop-Process -Id $service.Id } }
        $unavailableState = Join-Path $taskRoot ('build-mp/unavailable-broker-fixture-' + [Guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $unavailableState -Force | Out-Null
        $unavailableExecutable = Join-Path $unavailableState 'removed-worker.exe'
        Copy-Item -LiteralPath (Join-Path $taskRoot 'build-mp/relay-tests.exe') -Destination $unavailableExecutable
        $unavailableArguments = '--bind 127.0.0.1 --port 38995 --max-rooms 8 --game-executable "' + $unavailableExecutable + '" --game-directory "' + $taskRoot + '" --hosted-address 127.0.0.1 --game-port-base 39011 --max-hosted 1 --state-directory "' + $unavailableState + '"'
        $service = Start-Process -FilePath (Join-Path $taskRoot 'build-mp/cc-room-service.exe') -ArgumentList $unavailableArguments -WorkingDirectory $taskRoot -WindowStyle Hidden -RedirectStandardOutput 'build-mp/unavailable-broker.log' -RedirectStandardError 'build-mp/unavailable-broker-error.log' -PassThru
        try {
            Wait-RoomServiceReady $service 'build-mp/unavailable-broker.log'
            # Disappearance after validation must exercise failed child launch.
            Remove-Item -LiteralPath $unavailableExecutable
            & ./build-mp/relay-tests.exe --hosted-unavailable 38995 127.0.0.1 $unavailableState
            if ($LASTEXITCODE) { throw 'Unavailable hosted executable verification failed.' }
        }
        finally { if (!$service.HasExited) { Stop-Process -Id $service.Id } }
        $lifetimeState = Join-Path $taskRoot ('build-mp/lifetime-broker-fixture-' + [Guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $lifetimeState -Force | Out-Null
        $lifetimeArguments = '--bind 127.0.0.1 --port 38994 --max-rooms 8 --game-executable "' + (Join-Path $taskRoot 'build-mp/relay-tests.exe') + '" --game-directory "' + $taskRoot + '" --hosted-address 127.0.0.1 --game-port-base 39012 --max-hosted 1 --state-directory "' + $lifetimeState + '"'
        $service = Start-Process -FilePath (Join-Path $taskRoot 'build-mp/cc-room-service.exe') -ArgumentList $lifetimeArguments -WorkingDirectory $taskRoot -WindowStyle Hidden -RedirectStandardOutput 'build-mp/lifetime-broker.log' -RedirectStandardError 'build-mp/lifetime-broker-error.log' -PassThru
        $workerProcess = $null
        try {
            Wait-RoomServiceReady $service 'build-mp/lifetime-broker.log'
            & ./build-mp/relay-tests.exe --hosted-lifetime 38994 127.0.0.1 $lifetimeState
            if ($LASTEXITCODE) { throw 'Hosted worker lifetime fixture failed.' }
            $workerId = [int](Get-Content -LiteralPath (Join-Path $lifetimeState 'test-process.txt') -Raw).Trim()
            $workerProcess = Get-Process -Id $workerId
            if ($workerProcess.Path -ne (Join-Path $taskRoot 'build-mp/relay-tests.exe')) { throw 'Lifetime fixture did not identify its owned test worker.' }
            Stop-Process -Id $service.Id
            $deadline = [DateTime]::UtcNow.AddSeconds(5)
            while (!$workerProcess.HasExited -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 50; $workerProcess.Refresh() }
            if (!$workerProcess.HasExited) { throw 'Abrupt broker shutdown leaked its dedicated worker process.' }
            Write-Output 'PASS: abrupt Windows broker shutdown terminates its owned dedicated child process.'
        } finally {
            if ($workerProcess -and !$workerProcess.HasExited) { Stop-Process -Id $workerProcess.Id }
            if (!$service.HasExited) { Stop-Process -Id $service.Id }
        }
        $startupState = Join-Path $taskRoot ('build-mp/startup-broker-fixture-' + [Guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $startupState -Force | Out-Null
        $startupArguments = '--bind 127.0.0.1 --port 38993 --max-rooms 8 --game-executable "' + (Join-Path $taskRoot 'build-mp/relay-tests.exe') + '" --game-directory "' + $taskRoot + '" --hosted-address 127.0.0.1 --game-port-base 39013 --max-hosted 1 --startup-seconds 2 --state-directory "' + $startupState + '"'
        $service = Start-Process -FilePath (Join-Path $taskRoot 'build-mp/cc-room-service.exe') -ArgumentList $startupArguments -WorkingDirectory $taskRoot -WindowStyle Hidden -RedirectStandardOutput 'build-mp/startup-broker.log' -RedirectStandardError 'build-mp/startup-broker-error.log' -PassThru
        try { Wait-RoomServiceReady $service 'build-mp/startup-broker.log'; & ./build-mp/relay-tests.exe --hosted-startup-timeout 38993 127.0.0.1 $startupState; if ($LASTEXITCODE) { throw 'Stalled hosted startup verification failed.' } }
        finally { if (!$service.HasExited) { Stop-Process -Id $service.Id } }
    }
} finally { Pop-Location }
