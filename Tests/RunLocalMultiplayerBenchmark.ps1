param(
    [ValidatePattern('^[a-zA-Z0-9-]+$')][string]$Label = 'local',
    [ValidatePattern('^[^\\/]+\.exe$')][string]$GameExecutable = 'Cortex Command.exe',
    [ValidateRange(1, 3)][int]$Guests = 3,
    [string[]]$GuestResolutions = @('640x360', '960x540', '1280x720'),
    [ValidateRange(0, 256)][int]$Actors = 0,
    [ValidateRange(0, 64)][int]$Burst = 0,
    [ValidateRange(1, 48)][int]$UploadMbps = 3,
    [switch]$Wan,
    [ValidateRange(0, 500)][double]$DelayMs = 60,
    [ValidateRange(0, 200)][double]$JitterMs = 20,
    [ValidateRange(0, 50)][double]$LossPercent = 3,
    [ValidateRange(0.1, 100)][double]$ProxyMbps = 3,
    # Independent per-datagram delays reorder traffic, which real queues rarely do.
    [switch]$Reorder,
    [switch]$Loss
)
# Local equivalent of RunMultiplayerBenchmark.ps1: a headless dedicated worker
# launched by a local room service, four native clients, optional AI load, and
# optional WAN impairment through the seeded UDP proxy. The worker listens on
# port 8100 so the existing benchmark proxy mapping applies unchanged.
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
# Runtimes hold Data junctions, so they live outside the game folder: the game
# scans its install tree at startup and would otherwise walk every copy.
$runtime = Join-Path $env:LOCALAPPDATA "CortexCommandBenchmarks/$Label"
if (Test-Path -LiteralPath $runtime) { throw "Use a fresh benchmark label: $runtime already exists." }
New-Item -ItemType Directory -Path $runtime, "$runtime/Mods", "$runtime/ScreenShots", "$runtime/build-mp", "$runtime/userdata", "$runtime/Data" -Force | Out-Null

# Overlay Data so only the isolated OneManArmy copy carries the load fixture.
foreach ($module in Get-ChildItem -LiteralPath "$taskRoot/Data" -Force) {
    if ($module.Name -ne 'Base.rte') { New-Item -ItemType Junction -Path "$runtime/Data/$($module.Name)" -Target $module.FullName | Out-Null; continue }
    New-Item -ItemType Directory -Path "$runtime/Data/Base.rte" | Out-Null
    foreach ($entry in Get-ChildItem -LiteralPath $module.FullName -Force) {
        if ($entry.Name -eq 'Activities') {
            New-Item -ItemType Directory -Path "$runtime/Data/Base.rte/Activities" | Out-Null
            foreach ($activity in Get-ChildItem -LiteralPath $entry.FullName -Force) {
                if ($activity.PSIsContainer) { New-Item -ItemType Junction -Path "$runtime/Data/Base.rte/Activities/$($activity.Name)" -Target $activity.FullName | Out-Null }
                else { Copy-Item -LiteralPath $activity.FullName -Destination "$runtime/Data/Base.rte/Activities/$($activity.Name)" }
            }
            Add-Content -LiteralPath "$runtime/Data/Base.rte/Activities/OneManArmy.lua" -Value ("`n" + (Get-Content -LiteralPath "$PSScriptRoot/MultiplayerBenchmarkLoad.lua" -Raw))
        } elseif ($entry.PSIsContainer) { New-Item -ItemType Junction -Path "$runtime/Data/Base.rte/$($entry.Name)" -Target $entry.FullName | Out-Null }
        else { Copy-Item -LiteralPath $entry.FullName -Destination "$runtime/Data/Base.rte/$($entry.Name)" }
    }
}
Copy-Item -LiteralPath (Join-Path $taskRoot $GameExecutable) -Destination $runtime
Get-ChildItem -LiteralPath $taskRoot -Filter '*.dll' -File | Copy-Item -Destination $runtime

$saved = @{}
foreach ($name in 'CCCP_MPBENCHMARK', 'CCCP_MPBENCH_UPLOAD', 'CCCP_MPBENCH_ACTORS', 'CCCP_MPBENCH_BURST', 'CCCP_MPBENCH_PROXY_BASE', 'CCCP_MPSMOKE_ROLE', 'CCCP_MPSMOKE_HOSTED', 'CCCP_MPSMOKE_COMBAT', 'CCCP_MPSMOKE_GUESTS', 'CCCP_MPSMOKE_WORLD_LOSS') { $saved[$name] = [Environment]::GetEnvironmentVariable($name) }
$service = $proxy = $null
try {
    $env:CCCP_MPBENCHMARK = '1'
    $env:CCCP_MPBENCH_UPLOAD = "$UploadMbps"
    $env:CCCP_MPBENCH_ACTORS = "$Actors"
    $env:CCCP_MPBENCH_BURST = "$Burst"
    $env:CCCP_MPBENCH_PROXY_BASE = if ($Wan) { '38990' } else { $null }
    # The worker inherits the smoke role from the service environment.
    $env:CCCP_MPSMOKE_ROLE = 'dedicated'
    $env:CCCP_MPSMOKE_HOSTED = '1'
    $env:CCCP_MPSMOKE_COMBAT = '1'
    $env:CCCP_MPSMOKE_GUESTS = "$Guests"
    $env:CCCP_MPSMOKE_WORLD_LOSS = if ($Loss) { '1' } else { $null }
    $workers = Join-Path $runtime 'workers'
    New-Item -ItemType Directory -Path $workers | Out-Null
    $serviceArguments = '--bind 127.0.0.1 --port 38997 --max-rooms 8 --game-executable "' + (Join-Path $runtime $GameExecutable) + '" --game-directory "' + $runtime + '" --hosted-address 127.0.0.1 --game-port-base 8100 --max-hosted 1 --state-directory "' + $workers + '"'
    $service = Start-Process -FilePath (Join-Path $taskRoot 'build-mp/cc-room-service.exe') -ArgumentList $serviceArguments -WorkingDirectory $taskRoot -WindowStyle Hidden -RedirectStandardOutput "$runtime/service.log" -RedirectStandardError "$runtime/service-error.log" -PassThru
    if ($Wan) {
        $python = (Get-Command python -ErrorAction Stop).Source
        $proxyArguments = "Tests/MultiplayerAwsWanProxy.py --server 127.0.0.1 --server-port 8100 --port 38990 --delay-ms $DelayMs --jitter-ms $JitterMs --loss-percent $LossPercent --mbps $ProxyMbps --duration 900 --stop-file `"$runtime/stop-proxy`"" + $(if ($Reorder) { ' --reorder' } else { '' })
        $proxy = Start-Process -FilePath $python -ArgumentList $proxyArguments -WorkingDirectory $taskRoot -WindowStyle Hidden -RedirectStandardOutput "$runtime/proxy.log" -RedirectStandardError "$runtime/proxy-error.log" -PassThru
    }
    Start-Sleep -Seconds 1
    $passed = $true
    try {
        & "$PSScriptRoot/RunMultiplayerTests.ps1" -Hosted -Guests $Guests -CombatStress -Loss:$Loss -ServiceAddress '127.0.0.1:38997' -GameDirectory $runtime -GameExecutable $GameExecutable -SkipTransportTests -GuestResolutions $GuestResolutions *> "$runtime/run.log"
    } catch { $passed = $false; "BENCHMARK RUN FAILED: $_" | Add-Content -LiteralPath "$runtime/run.log" }
    Copy-Item -LiteralPath "$workers/worker-8100/userdata/verification/dedicated-smoke.log" -Destination "$runtime/dedicated-smoke.log" -ErrorAction SilentlyContinue
    Copy-Item -LiteralPath "$workers/worker-8100/userdata/load-benchmark.csv" -Destination "$runtime/load-benchmark.csv" -ErrorAction SilentlyContinue
    Copy-Item -LiteralPath "$workers/worker-8100/userdata/verification/server-benchmark.csv" -Destination "$runtime/server-benchmark.csv" -ErrorAction SilentlyContinue
    "PASSED=$passed"
} finally {
    New-Item -ItemType File -Path "$runtime/stop-proxy" -Force | Out-Null
    if ($proxy -and !$proxy.HasExited) { Start-Sleep -Seconds 1; if (!$proxy.HasExited) { Stop-Process -Id $proxy.Id } }
    if ($service -and !$service.HasExited) { Stop-Process -Id $service.Id }
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name]) }
}
python "$PSScriptRoot/AnalyzeMultiplayerBenchmark.py" (Join-Path $runtime 'build-mp')
if (Test-Path -LiteralPath "$runtime/load-benchmark.csv") { python "$PSScriptRoot/AnalyzeLocalServerBenchmark.py" $runtime }
