param(
    [string]$Executable = 'Cortex Command.exe',
    [ValidatePattern('^[a-zA-Z0-9-]+$')][string]$Label = 'verification',
    [switch]$Mirrored,
    [int]$TimeoutSeconds = 150
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path $PSScriptRoot -Parent
$taskModule = Join-Path $taskRoot 'Mods/build-AILocomotionTests.rte'
if (Test-Path -LiteralPath $taskModule) { throw 'The temporary locomotion module is already in use.' }
New-Item -ItemType Directory -Path $taskModule | Out-Null
Copy-Item -Path "$PSScriptRoot/AILocomotion.rte/*" -Destination $taskModule
if ($Mirrored) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'AILocomotion.rte/MirroredTerrain.png') -Destination (Join-Path $taskModule 'Terrain.png') -Force
    'return {mirrored=true};' | Set-Content -LiteralPath (Join-Path $taskModule 'Config.lua')
}
$taskSettings = Join-Path $taskRoot 'build-mp/locomotion-settings.ini'
@'
SettingsMan
	ResolutionX = 960
	ResolutionY = 540
	ResolutionMultiplier = 1
	Fullscreen = 0
	EnableVSync = 0
	SkipIntro = 1
	MuteMaster = 1
	DefaultSceneName = AI Locomotion Regression Terrain
'@ | Set-Content -LiteralPath $taskSettings
$taskProcess = $null
try {
    $taskProcess = Start-Process -FilePath (Join-Path $taskRoot $Executable) -ArgumentList '-test-activity "AI Locomotion Regression" -module build-AILocomotionTests.rte' -WorkingDirectory $taskRoot -WindowStyle Hidden -Environment @{CCCP_SETTINGSPATH=$taskSettings;CCCP_TEST_ACTIVITY='AI Locomotion Regression'} -PassThru
    if (-not $taskProcess.WaitForExit($TimeoutSeconds * 1000)) { throw 'Native locomotion test timed out.' }
    $taskLog = Join-Path $taskModule 'runtime.log'
    if (-not (Test-Path -LiteralPath $taskLog)) { throw 'Native locomotion activity did not start; inspect build-mp/native-test-error.log.' }
    $taskResult = Get-Content -LiteralPath $taskLog -Raw
    ($taskResult -split "`n" | Where-Object { $_ -match 'START|ARRIVED|PASS:|FAIL:' })
    if ($taskProcess.ExitCode -ne 0 -or $taskResult -notmatch 'PASS: all movement orders completed') { throw 'Native movement checks failed; the trace is retained under build-mp.' }
    if ((Get-Content -LiteralPath (Join-Path $taskRoot 'build-mp/locomotion-native-console.log') -Raw) -match 'behavior .* error:|GoToWpt error:') { throw 'Native locomotion produced AI script errors.' }
} finally {
    if ($taskProcess -and -not $taskProcess.HasExited) { Stop-Process -Id $taskProcess.Id }
    $taskResolvedModule = [IO.Path]::GetFullPath($taskModule)
    $taskExpectedModule = [IO.Path]::GetFullPath((Join-Path $taskRoot 'Mods/build-AILocomotionTests.rte'))
    $taskEvidence = [IO.Path]::GetFullPath((Join-Path $taskRoot ('build-mp/locomotion-native-' + $Label + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))))
    if ($taskResolvedModule -ne $taskExpectedModule -or [IO.Path]::GetDirectoryName($taskEvidence) -ne [IO.Path]::GetFullPath((Join-Path $taskRoot 'build-mp'))) { throw 'Locomotion cleanup path is outside the workspace.' }
    if (Test-Path -LiteralPath $taskResolvedModule) { Move-Item -LiteralPath $taskResolvedModule -Destination $taskEvidence }
    Write-Output "Evidence: $taskEvidence"
}
