param([switch]$Smoke, [ValidateRange(1, 3)][int]$Guests = 1, [switch]$Relay, [switch]$Deployment, [switch]$Loss, [ValidateSet('640x360', '960x540', '1280x720', '1920x1080')][string[]]$GuestResolutions = @(), [string]$ServiceAddress = '', [string]$GameDirectory = '', [ValidatePattern('^[^\\/]+\.exe$')][string]$GameExecutable = 'Cortex Command.exe')
$ErrorActionPreference = 'Stop'
if ($ServiceAddress -and !$Relay) { throw 'Use -Relay with -ServiceAddress for a live room-service test.' }
if ($GuestResolutions.Count -gt 1 -and $GuestResolutions.Count -ne $Guests) { throw 'GuestResolutions must specify one size for all guests or one size per guest.' }
$taskRoot = Split-Path -Parent $PSScriptRoot
$gameRoot = if ($GameDirectory) { (Resolve-Path -LiteralPath $GameDirectory).Path } else { $taskRoot }
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
        if ($Relay) { & ./Services/RoomService/Build.ps1 -Test }
        if (!$GameDirectory) {
            & 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe' RTEA.sln /m:4 /p:Configuration=Final /p:Platform=x64 "/p:TargetName=$([IO.Path]::GetFileNameWithoutExtension($GameExecutable))" "/p:ForceImportAfterCppTargets=$PSScriptRoot\MultiplayerBuild.props" /v:quiet '/flp:logfile=build-mp/restoration.log;verbosity=minimal'
            if ($LASTEXITCODE) { throw 'Game build failed.' }
            if (!(Test-Path -LiteralPath fmod.dll)) { Copy-Item -LiteralPath external/lib/win/fmod.dll -Destination fmod.dll }
        }
        if (!(Test-Path -LiteralPath (Join-Path $gameRoot $GameExecutable)) -or !(Test-Path -LiteralPath (Join-Path $gameRoot 'Data'))) { throw 'GameDirectory must contain the built game and Data folder.' }
        New-Item -ItemType Directory -Path (Join-Path $gameRoot 'build-mp') -Force | Out-Null
        $previousSettings = $env:CCCP_SETTINGSPATH
        $previousSmokeRole = $env:CCCP_MPSMOKE_ROLE
        $previousSmokeGuests = $env:CCCP_MPSMOKE_GUESTS
        $previousDeployment = $env:CCCP_MPSMOKE_DEPLOYMENT
        $previousLoss = $env:CCCP_MPSMOKE_WORLD_LOSS
        $previousService = $env:CCCP_MP_SERVICE
        $env:CCCP_MPSMOKE_GUESTS = "$Guests"
        $env:CCCP_MPSMOKE_DEPLOYMENT = if ($Deployment) { '1' } else { $null }
        $env:CCCP_MPSMOKE_WORLD_LOSS = if ($Loss) { '1' } else { $null }
        $roles = @('host', 'client')
        if ($Guests -ge 2) { $roles += 'client2' }
        if ($Guests -ge 3) { $roles += 'client3' }
        $instances = @()
        $roomService = $null
        try {
            if ($Relay) {
                $env:CCCP_MP_SERVICE = if ($ServiceAddress) { $ServiceAddress } else { '127.0.0.1:38997' }
                $codeFile = Join-Path $gameRoot 'build-mp/room-code.txt'
                if (Test-Path -LiteralPath $codeFile) { Remove-Item -LiteralPath $codeFile }
                if (!$ServiceAddress) { $roomService = Start-Process -FilePath (Join-Path $taskRoot 'build-mp/cc-room-service.exe') -ArgumentList '--bind 127.0.0.1 --port 38997 --max-rooms 8' -WorkingDirectory $taskRoot -WindowStyle Hidden -RedirectStandardOutput 'build-mp/native-relay-service.log' -RedirectStandardError 'build-mp/native-relay-service-error.log' -PassThru }
            } else { $env:CCCP_MP_SERVICE = $null }
            foreach ($role in $roles) {
                $settings = Join-Path $gameRoot "build-mp/$role-settings.ini"
                $guestIndex = [array]::IndexOf($roles, $role) - 1
                $resolution = if ($guestIndex -lt 0 -or !$GuestResolutions.Count) { '960x540' } elseif ($GuestResolutions.Count -eq 1) { $GuestResolutions[0] } else { $GuestResolutions[$guestIndex] }
                $dimensions = $resolution.Split('x')
                $disabledMods = (Get-ChildItem -LiteralPath (Join-Path $gameRoot 'Mods') -Directory -Filter '*.rte' | ForEach-Object { "    DisableMod = $($_.Name)" }) -join "`n"
                @"
SettingsMan
    ResolutionX = $($dimensions[0])
    ResolutionY = $($dimensions[1])
    ResolutionMultiplier = 1
    Fullscreen = 0
    EnableVSync = 1
    UseMultiDisplays = 0
    SkipIntro = 1
    LaunchIntoActivity = 0
$disabledMods
"@ | ForEach-Object { $_ -replace '(?m)^    ', "`t" } | Set-Content -LiteralPath $settings
                $env:CCCP_SETTINGSPATH = $settings
                $env:CCCP_MPSMOKE_ROLE = $role
                if ($Relay -and $role -ne 'host') {
                    $codeDeadline = [DateTime]::UtcNow.AddSeconds(60)
                    while (!(Test-Path -LiteralPath $codeFile) -and [DateTime]::UtcNow -lt $codeDeadline) { Start-Sleep -Milliseconds 200 }
                    if (!(Test-Path -LiteralPath $codeFile)) { throw 'Host did not receive a room code.' }
                    $roomCode = (Get-Content -LiteralPath $codeFile -Raw).Trim()
                }
                $argument = if ($role -eq 'host') { '-mp-smoke-host 38998' } elseif ($Relay) { "-mp-smoke-client $roomCode" } else { '-mp-smoke-client 127.0.0.1:38998' }
                $instances += Start-Process -FilePath (Join-Path $gameRoot $GameExecutable) -ArgumentList $argument -WorkingDirectory $gameRoot -WindowStyle Hidden -PassThru
            }
            $deadline = [DateTime]::UtcNow.AddSeconds(150)
            while (@($instances | Where-Object { !$_.HasExited }).Count -gt 0 -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Seconds 1 }
            foreach ($instance in $instances) { if (!$instance.HasExited) { throw 'Native multiplayer verification timed out.' }; if ($instance.ExitCode) { throw "Game instance exited with $($instance.ExitCode)." } }
            foreach ($role in $roles) {
                $log = Get-Content -LiteralPath (Join-Path $gameRoot "build-mp/$role-smoke.log")
                $log
                if (!($log -match '^PASS:') -or ($log -match '^FAIL:')) { throw "$role native verification failed." }
            }
        } finally {
            foreach ($instance in $instances) { if (!$instance.HasExited) { Stop-Process -Id $instance.Id } }
            if ($roomService -and !$roomService.HasExited) { Stop-Process -Id $roomService.Id }
            $env:CCCP_SETTINGSPATH = $previousSettings
            $env:CCCP_MPSMOKE_ROLE = $previousSmokeRole
            $env:CCCP_MPSMOKE_GUESTS = $previousSmokeGuests
            $env:CCCP_MPSMOKE_DEPLOYMENT = $previousDeployment
            $env:CCCP_MPSMOKE_WORLD_LOSS = $previousLoss
            $env:CCCP_MP_SERVICE = $previousService
        }
    }
} finally { Pop-Location }
