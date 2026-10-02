param([string]$GameDirectory = '')
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$gameRoot = if ($GameDirectory) { (Resolve-Path -LiteralPath $GameDirectory).Path } else { $taskRoot }
Push-Location $taskRoot
try {
    New-Item -ItemType Directory -Path (Join-Path $gameRoot 'build-mp') -Force | Out-Null
    if (!$GameDirectory) {
        & 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe' RTEA.sln /m:4 /p:Configuration=Final /p:Platform=x64 /v:quiet '/flp:logfile=build-mp/cursor-build.log;verbosity=minimal'
        if ($LASTEXITCODE) { throw 'Game build failed.' }
        if (!(Test-Path -LiteralPath fmod.dll)) { Copy-Item -LiteralPath external/lib/win/fmod.dll -Destination fmod.dll }
    }
    $settings = Join-Path $gameRoot 'build-mp/cursor-settings.ini'
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
    $previousSettings = $env:CCCP_SETTINGSPATH
    $previousCursor = $env:CCCP_MPSMOKE_CURSOR
    $logPath = Join-Path $gameRoot 'build-mp/cursor-smoke.log'
    if (Test-Path -LiteralPath $logPath) { Remove-Item -LiteralPath $logPath }
    $instance = $null
    try {
        $env:CCCP_SETTINGSPATH = $settings
        $env:CCCP_MPSMOKE_CURSOR = '1'
        $instance = Start-Process -FilePath (Join-Path $gameRoot 'Cortex Command.exe') -WorkingDirectory $gameRoot -WindowStyle Hidden -PassThru
        $deadline = [DateTime]::UtcNow.AddSeconds(120)
        while (!$instance.HasExited -and [DateTime]::UtcNow -lt $deadline) {
            if ((Test-Path -LiteralPath $logPath) -and ((Get-Content -LiteralPath $logPath) -match '^(PASS|FAIL):')) {
                if (!$instance.WaitForExit(5000)) { Stop-Process -Id $instance.Id; $instance.WaitForExit() }
                break
            }
            Start-Sleep -Milliseconds 200
        }
        if (!$instance.HasExited) { throw 'Native cursor verification timed out.' }
        $log = Get-Content -LiteralPath $logPath
        $log
        if (!($log -match '^PASS:') -or ($log -match '^FAIL:')) { throw 'Multiplayer cursor verification failed.' }
        foreach ($line in $log) {
            if ($line -match '^([a-z-]+): expected=') {
                $screenshot = Join-Path $gameRoot "build-mp/cursor-$($Matches[1]).png"
                if (!(Test-Path -LiteralPath $screenshot) -or (Get-Item -LiteralPath $screenshot).Length -eq 0) { throw "Cursor screenshot missing: $screenshot" }
            }
        }
    } finally {
        if ($instance -and !$instance.HasExited) { Stop-Process -Id $instance.Id }
        $env:CCCP_SETTINGSPATH = $previousSettings
        $env:CCCP_MPSMOKE_CURSOR = $previousCursor
    }
} finally { Pop-Location }
