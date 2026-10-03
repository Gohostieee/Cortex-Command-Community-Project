param(
    [string]$Executable = 'Cortex Command.mining.exe',
    [int]$TimeoutSeconds = 420
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path $PSScriptRoot -Parent
New-Item -ItemType Directory -Path (Join-Path $taskRoot 'build-mp') -Force | Out-Null
$taskModule = Join-Path $taskRoot 'Mods/build-GoldMiningTests.rte'
New-Item -ItemType Directory -Path $taskModule -Force | Out-Null
Copy-Item -Path "$PSScriptRoot/GoldMining.rte/*" -Destination $taskModule -Force
$taskLog = Join-Path $taskModule 'runtime.log'
if (Test-Path -LiteralPath $taskLog) { Remove-Item -LiteralPath $taskLog }
$taskSettings = Join-Path $taskRoot 'build-mp/mining-settings.ini'
@'
SettingsMan
	ResolutionX = 960
	ResolutionY = 540
	ResolutionMultiplier = 1
	Fullscreen = 0
	EnableVSync = 0
	SkipIntro = 1
	MuteMaster = 1
	DefaultSceneName = Gold Mining Regression Terrain
'@ | Set-Content -LiteralPath $taskSettings
$taskProcess = Start-Process -FilePath (Join-Path $taskRoot $Executable) -ArgumentList '-test-activity "Gold Mining Regression" -module build-GoldMiningTests.rte' -WorkingDirectory $taskRoot -WindowStyle Hidden -Environment @{CCCP_SETTINGSPATH=$taskSettings;CCCP_TEST_ACTIVITY='Gold Mining Regression'} -PassThru
try {
    if (-not $taskProcess.WaitForExit($TimeoutSeconds * 1000)) { throw 'Native mining test timed out; inspect the startup/error and runtime logs.' }
    if (-not (Test-Path -LiteralPath $taskLog)) { throw 'Native test did not start; inspect build-mp/native-test-error.log.' }
    $taskResult = Get-Content -LiteralPath $taskLog -Raw
    if ($taskProcess.ExitCode -ne 0 -or $taskResult -notmatch 'PASS: all terrain gold mined') { throw "Native mining test failed.`n$taskResult" }
    $taskConsole = Join-Path $taskRoot 'build-mp/mining-native-console.log'
    if (Test-Path -LiteralPath $taskConsole) {
        if ((Get-Content -LiteralPath $taskConsole -Raw) -match 'behavior .* error:|GoToWpt error:') { throw 'Native mining test produced AI script errors.' }
    }
    ($taskResult -split "`n" | Select-Object -First 1)
    ($taskResult -split "`n" | Where-Object { $_ -match 'remaining=0|PASS:' } | Select-Object -Last 2)
} finally {
    if (-not $taskProcess.HasExited) { Stop-Process -Id $taskProcess.Id }
    # Keep the evidence, and remove the temporary activity from normal gameplay.
    $taskResolvedModule = [IO.Path]::GetFullPath($taskModule)
    $taskExpectedModule = [IO.Path]::GetFullPath((Join-Path $taskRoot 'Mods/build-GoldMiningTests.rte'))
    $taskEvidence = [IO.Path]::GetFullPath((Join-Path $taskRoot ('build-mp/mining-native-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))))
    $taskEvidenceParent = [IO.Path]::GetFullPath((Join-Path $taskRoot 'build-mp'))
    if ($taskResolvedModule -ne $taskExpectedModule -or [IO.Path]::GetDirectoryName($taskEvidence) -ne $taskEvidenceParent) { throw 'Regression cleanup paths are outside the expected workspace.' }
    if (Test-Path -LiteralPath $taskResolvedModule) { Move-Item -LiteralPath $taskResolvedModule -Destination $taskEvidence }
}
