param(
    [ValidateRange(1, 2)][int]$Rooms = 1,
    [ValidateRange(1, 20)][int]$Cycles = 1,
    [string]$Label = 'baseline',
    [string]$ServiceAddress = '34.231.191.247:8001',
    [string]$GameExecutable = 'Cortex Command.benchmark.exe',
    [string[]]$GuestResolutions = @('640x360', '960x540', '1280x720'),
    [ValidateRange(0, 65534)][int]$ProxyBase = 0,
    [ValidateRange(1, 48)][int]$UploadMbps = 12,
    [switch]$Loss
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$outputRoot = Join-Path $taskRoot "build-mp/aws-benchmark/$Label"
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
$jobs = @()
try {
    for ($cycle = 1; $cycle -le $Cycles; $cycle++) {
        $started = [DateTime]::UtcNow.ToString('o')
        $jobs = @()
        for ($room = 1; $room -le $Rooms; $room++) {
            $gameRoot = Join-Path $outputRoot "cycle-$cycle-room-$room"
            if (Test-Path -LiteralPath $gameRoot) { throw "Use a fresh benchmark label: $gameRoot already exists." }
            New-Item -ItemType Directory -Path $gameRoot, "$gameRoot/Mods", "$gameRoot/ScreenShots", "$gameRoot/build-mp", "$gameRoot/userdata" -Force | Out-Null
            New-Item -ItemType Junction -Path "$gameRoot/Data" -Target "$taskRoot/Data" | Out-Null
            Copy-Item -LiteralPath (Join-Path $taskRoot $GameExecutable) -Destination $gameRoot
            Get-ChildItem -LiteralPath $taskRoot -Filter '*.dll' -File | Copy-Item -Destination $gameRoot
            $jobs += Start-Job -ScriptBlock {
                param($repository, $runtime, $executable, $service, $resolutions, $loss, $proxyBase, $uploadMbps)
                $ErrorActionPreference = 'Stop'
                $env:CCCP_MPBENCHMARK = '1'
                $env:CCCP_MPBENCH_UPLOAD = "$uploadMbps"
                $env:CCCP_MPBENCH_PROXY_BASE = if ($proxyBase) { "$proxyBase" } else { $null }
                $env:CCCP_USERDATA_PATH = Join-Path $runtime 'userdata'
                & "$repository/Tests/RunHostedMultiplayerTests.ps1" -Guests 3 -CombatStress -Loss:$loss -ServiceAddress $service -GameDirectory $runtime -GameExecutable $executable -SkipTransportTests -GuestResolutions $resolutions *> "$runtime/run.log"
                [pscustomobject]@{Runtime=$runtime; Passed=$true}
            } -ArgumentList $taskRoot, $gameRoot, $GameExecutable, $ServiceAddress, $GuestResolutions, ([bool]$Loss), $ProxyBase, $UploadMbps
        }
        $deadline = [DateTime]::UtcNow.AddSeconds(340)
        while (@($jobs | Where-Object State -eq 'Running').Count -and [DateTime]::UtcNow -lt $deadline) {
            foreach ($log in Get-ChildItem -LiteralPath $outputRoot -Filter '*-smoke.log' -File -Recurse) {
                $text = Get-Content -LiteralPath $log.FullName -Raw
                if ($text -match '(?m)^FAIL:' -or ([regex]::Matches($text, '(?m)^JOIN: creator used room-code join').Count -ge 10)) {
                    throw "Native lifecycle failed or creator is stuck rejoining; inspect $($log.FullName)."
                }
            }
            $sample = foreach ($process in Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($GameExecutable)) -ErrorAction SilentlyContinue) {
                [pscustomobject]@{time=[DateTime]::UtcNow.ToString('o'); cycle=$cycle; pid=$process.Id; cpu_seconds=$process.CPU; working_set=$process.WorkingSet64; private_bytes=$process.PrivateMemorySize64; threads=$process.Threads.Count}
            }
            if ($sample) { $sample | Export-Csv -LiteralPath "$outputRoot/windows-processes.csv" -NoTypeInformation -Append }
            Start-Sleep -Seconds 1
        }
        foreach ($job in $jobs) {
            $result = Receive-Job -Job $job -ErrorAction Continue
            if ($job.State -ne 'Completed' -or !$result.Passed) { throw "Benchmark cycle $cycle failed; inspect room run.log and client traces." }
        }
        $jobs | Remove-Job
        $jobs = @()
        [pscustomobject]@{cycle=$cycle; started=$started; ended=[DateTime]::UtcNow.ToString('o'); rooms=$Rooms; players=$Rooms*4; loss=[bool]$Loss; passed=$true} | ConvertTo-Json -Compress | Add-Content -LiteralPath "$outputRoot/cycles.jsonl"
        Write-Output "Completed cycle $cycle with $Rooms room(s), $($Rooms * 4) native players."
    }
} catch {
    [pscustomobject]@{cycle=$cycle; started=$started; ended=[DateTime]::UtcNow.ToString('o'); rooms=$Rooms; players=$Rooms*4; loss=[bool]$Loss; passed=$false; error=$_.Exception.Message} | ConvertTo-Json -Compress | Add-Content -LiteralPath "$outputRoot/cycles.jsonl"
    throw
} finally {
    foreach ($job in $jobs) { Stop-Job -Job $job; Remove-Job -Job $job -Force }
    Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($GameExecutable)) -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path.StartsWith($outputRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) } | Stop-Process
}
