param(
    [ValidateRange(1, 48)][int]$LinkMbps = 4,
    [ValidateRange(0, 48)][int]$HostUploadMbps = 0,
    [ValidatePattern('^[^\\/]+\.exe$')][string]$GameExecutable = 'Cortex Command.exe',
    [ValidatePattern('^[a-zA-Z0-9-]+$')][string]$Label = 'wan',
    [switch]$SkipTransportTests
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
Push-Location $taskRoot
$service = $proxy = $null
$previousUpload = $env:CCCP_MPSMOKE_UPLOAD
try {
    if (!$SkipTransportTests) {
        & ./Tests/RunMultiplayerTests.ps1
        & ./Services/RoomService/Build.ps1 -Test
    }
    $python = (Get-Command python -ErrorAction Stop).Source
    foreach ($role in @('host', 'client')) {
        $log = Join-Path $taskRoot "build-mp/$role-smoke.log"
        if (Test-Path -LiteralPath $log) { Remove-Item -LiteralPath $log }
    }
    $service = Start-Process -FilePath "$taskRoot/build-mp/cc-room-service.exe" -ArgumentList '--bind 127.0.0.1 --port 38995 --max-rooms 4' -WorkingDirectory $taskRoot -WindowStyle Hidden -RedirectStandardOutput "build-mp/$Label-service.log" -RedirectStandardError "build-mp/$Label-service-error.log" -PassThru
    $proxy = Start-Process -FilePath $python -ArgumentList "Tests/MultiplayerWanProxy.py --duration 300 --mbps $LinkMbps --warmup-log build-mp/client-smoke.log" -WorkingDirectory $taskRoot -WindowStyle Hidden -RedirectStandardOutput "build-mp/$Label-proxy.log" -RedirectStandardError "build-mp/$Label-proxy-error.log" -PassThru
    $env:CCCP_MPSMOKE_UPLOAD = if ($HostUploadMbps) { "$HostUploadMbps" } else { $null }
    & ./Tests/RunMultiplayerTests.ps1 -Smoke -Encounter -CombatStress -Relay -ServiceAddress '127.0.0.1:38996' -GameDirectory $taskRoot -GameExecutable $GameExecutable -SkipTransportTests
    if (!((Get-Content "build-mp/$Label-proxy.log") -match '^CONSTRAINED:')) { throw 'WAN fixture never applied the upload cap.' }
    if ((Get-Item "build-mp/$Label-proxy-error.log").Length) { throw 'WAN fixture reported an error.' }
    & $python Tests/CheckMultiplayerWanLog.py build-mp/client-smoke.log
    if ($LASTEXITCODE) { throw 'WAN guest updates stalled despite a healthy local rendering rate.' }
} finally {
    $env:CCCP_MPSMOKE_UPLOAD = $previousUpload
    if ($proxy -and !$proxy.HasExited) { Stop-Process -Id $proxy.Id }
    if ($service -and !$service.HasExited) { Stop-Process -Id $service.Id }
    foreach ($role in @('host', 'client')) {
        $log = Join-Path $taskRoot "build-mp/$role-smoke.log"
        if (Test-Path -LiteralPath $log) { Copy-Item -LiteralPath $log -Destination "build-mp/$Label-$role.log" }
    }
    Pop-Location
}
