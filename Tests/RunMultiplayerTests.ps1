param([switch]$Smoke, [ValidateRange(1, 3)][int]$Guests = 1)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
Push-Location $taskRoot
try {
    $devShell = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat'
    if (!(Test-Path -LiteralPath $devShell)) { throw 'Install Visual Studio 2022 C++ tools, or adjust devShell in this script.' }
    New-Item -ItemType Directory -Path build-mp -Force | Out-Null
    $compile = 'call "' + $devShell + '" -arch=x64 -host_arch=x64 > nul && cl /nologo /std:c++20 /EHsc /O2 /MD /DNOMINMAX /I Source\System /I external\sources\RakNet\include Tests\MultiplayerTests.cpp Source\System\MultiplayerTransport.cpp /Fo:build-mp\ /Fe:build-mp\MultiplayerTests.exe /link external\sources\RakNet\_Bin\raknet-release.lib ws2_32.lib winmm.lib'
    & cmd /c $compile
    if ($LASTEXITCODE) { throw 'Multiplayer test compilation failed.' }
    & ./build-mp/MultiplayerTests.exe
    if ($LASTEXITCODE) { throw 'Multiplayer protocol/transport tests failed.' }
    if ($Smoke) {
        & 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe' RTEA.sln /m:4 /p:Configuration=Final /p:Platform=x64 /v:quiet '/flp:logfile=build-mp/restoration.log;verbosity=minimal'
        if ($LASTEXITCODE) { throw 'Game build failed.' }
        if (!(Test-Path -LiteralPath fmod.dll)) { Copy-Item -LiteralPath external/lib/win/fmod.dll -Destination fmod.dll }
        $previousSettings = $env:CCCP_SETTINGSPATH
        $previousSmokeRole = $env:CCCP_MPSMOKE_ROLE
        $previousSmokeGuests = $env:CCCP_MPSMOKE_GUESTS
        $env:CCCP_MPSMOKE_GUESTS = "$Guests"
        $roles = @('host', 'client')
        if ($Guests -ge 2) { $roles += 'client2' }
        if ($Guests -ge 3) { $roles += 'client3' }
        $instances = @()
        try {
            foreach ($role in $roles) {
                $settings = Join-Path $taskRoot "build-mp/$role-settings.ini"
                @"
SettingsMan
    ResolutionX = 960
    ResolutionY = 540
    ResolutionMultiplier = 1
    Fullscreen = 0
    EnableVSync = 1
    UseMultiDisplays = 0
    SkipIntro = 1
    LaunchIntoActivity = 0
"@ | Set-Content -LiteralPath $settings
                $env:CCCP_SETTINGSPATH = $settings
                $env:CCCP_MPSMOKE_ROLE = $role
                $argument = if ($role -eq 'host') { '-mp-smoke-host 38998' } else { '-mp-smoke-client 127.0.0.1:38998' }
                $instances += Start-Process -FilePath (Join-Path $taskRoot 'Cortex Command.exe') -ArgumentList $argument -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
            }
            $deadline = [DateTime]::UtcNow.AddSeconds(150)
            while (@($instances | Where-Object { !$_.HasExited }).Count -gt 0 -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Seconds 1 }
            foreach ($instance in $instances) { if (!$instance.HasExited) { throw 'Native multiplayer verification timed out.' }; if ($instance.ExitCode) { throw "Game instance exited with $($instance.ExitCode)." } }
            foreach ($role in $roles) {
                $log = Get-Content -LiteralPath "build-mp/$role-smoke.log"
                $log
                if (!($log -match '^PASS:') -or ($log -match '^FAIL:')) { throw "$role native verification failed." }
            }
        } finally {
            foreach ($instance in $instances) { if (!$instance.HasExited) { Stop-Process -Id $instance.Id } }
            $env:CCCP_SETTINGSPATH = $previousSettings
            $env:CCCP_MPSMOKE_ROLE = $previousSmokeRole
            $env:CCCP_MPSMOKE_GUESTS = $previousSmokeGuests
        }
    }
} finally { Pop-Location }
