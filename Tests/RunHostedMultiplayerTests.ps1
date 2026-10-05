param(
    [ValidateRange(1, 3)][int]$Guests = 3,
    [switch]$SkipTransportTests,
    [switch]$Loss,
    [switch]$CombatStress,
    [ValidateSet('640x360', '960x540', '1280x720', '1920x1080')][string[]]$GuestResolutions = @('640x360', '960x540', '1280x720'),
    [string]$ServiceAddress = '',
    [string]$GameDirectory = '',
    [ValidatePattern('^[^\\/]+\.exe$')][string]$GameExecutable = 'Cortex Command.exe'
)
$ErrorActionPreference = 'Stop'
if ($Guests -ne 3 -and !$PSBoundParameters.ContainsKey('GuestResolutions')) { $GuestResolutions = @('960x540') }

# The same native match fixture exercises the dedicated process and every human
# as a remote player, including the room creator. The worker verifies all input
# slots, reconnection, room survival and return to the lobby for another match.
$arguments = @{
    Hosted = $true
    Guests = $Guests
    SkipTransportTests = $SkipTransportTests
    Loss = $Loss
    CombatStress = $CombatStress
    GuestResolutions = $GuestResolutions
    ServiceAddress = $ServiceAddress
    GameDirectory = $GameDirectory
    GameExecutable = $GameExecutable
}
& (Join-Path $PSScriptRoot 'RunMultiplayerTests.ps1') @arguments
