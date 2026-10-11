param([string]$OutputDirectory = '')
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
if (!$OutputDirectory) { $OutputDirectory = Join-Path $taskRoot 'build-mp/updater' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$compiler = Join-Path $env:windir 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
if (!(Test-Path -LiteralPath $compiler)) { throw 'Install the Windows .NET Framework 4.8 developer tools.' }
$exe = Join-Path $OutputDirectory 'CortexUpdater.exe'
$core = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'Updater/UpdateCore.cs'))
$program = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'Updater/Program.cs'))
& $compiler /nologo /target:winexe /platform:x64 /optimize+ "/out:$exe" /r:System.IO.Compression.dll /r:System.IO.Compression.FileSystem.dll /r:System.Web.Extensions.dll /r:System.Windows.Forms.dll /r:System.Drawing.dll $core $program
if ($LASTEXITCODE) { throw 'Updater compilation failed.' }
Get-Item -LiteralPath $exe | Select-Object FullName,Length
