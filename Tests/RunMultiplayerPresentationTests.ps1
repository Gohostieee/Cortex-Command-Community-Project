param([string]$GameExecutable = 'Cortex Command.exe', [switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
Push-Location $taskRoot
try {
    New-Item -ItemType Directory -Path build-mp -Force | Out-Null
    if (!$SkipBuild) {
        & 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe' RTEA.sln /m:4 /p:Configuration=Final /p:Platform=x64 "/p:TargetName=$([IO.Path]::GetFileNameWithoutExtension($GameExecutable))" "/p:ForceImportAfterCppTargets=$PSScriptRoot\MultiplayerBuild.props" /v:quiet '/flp:logfile=build-mp/presentation-build.log;verbosity=minimal'
        if ($LASTEXITCODE) { throw 'Game build failed.' }
    }
    $previousSettings = $env:CCCP_SETTINGSPATH
    $previousPresentation = $env:CCCP_MPSMOKE_PRESENTATION
    $settings = Join-Path $taskRoot 'build-mp/presentation-settings.ini'
    $disabledMods = (Get-ChildItem -LiteralPath Mods -Directory -Filter '*.rte' | ForEach-Object { "`tDisableMod = $($_.Name)" }) -join "`n"
    "SettingsMan`n`tResolutionX = 640`n`tResolutionY = 360`n`tResolutionMultiplier = 1`n`tFullscreen = 0`n`tEnableVSync = 1`n`tSkipIntro = 1`n`tLaunchIntoActivity = 0`n$disabledMods" | Set-Content -LiteralPath $settings
    $instance = $null
    try {
        $env:CCCP_SETTINGSPATH = $settings
        $env:CCCP_MPSMOKE_PRESENTATION = '1'
        $logPath = Join-Path $taskRoot 'build-mp/presentation-smoke.log'
        if (Test-Path -LiteralPath $logPath) { Remove-Item -LiteralPath $logPath }
        $instance = Start-Process -FilePath (Join-Path $taskRoot $GameExecutable) -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
        if (!$instance.WaitForExit(60000)) { throw 'Native presentation verification timed out.' }
        $log = Get-Content -LiteralPath $logPath
        $log
        if ($instance.ExitCode -or !($log -match '^PASS:') -or ($log -match '^FAIL:')) { throw 'Native multiplayer presentation verification failed.' }
    } finally {
        if ($instance -and !$instance.HasExited) { Stop-Process -Id $instance.Id }
        $env:CCCP_SETTINGSPATH = $previousSettings
        $env:CCCP_MPSMOKE_PRESENTATION = $previousPresentation
    }
} finally { Pop-Location }
