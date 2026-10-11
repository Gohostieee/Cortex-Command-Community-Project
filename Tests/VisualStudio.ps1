# Locates the newest Visual Studio 2022 install with C++ tools (any edition,
# including Build Tools). Dot-source this file, then use $VisualStudioRoot,
# $VsDevCmd and $MSBuild.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$VisualStudioRoot = if (Test-Path -LiteralPath $vswhere) { & $vswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1 }
if (!$VisualStudioRoot) { throw 'Install Visual Studio 2022 (or its Build Tools) with the C++ x64 tools.' }
# VsDevCmd.bat looks for vswhere on PATH and writes an error to stderr without it.
if (!(Get-Command vswhere.exe -ErrorAction SilentlyContinue)) { $env:PATH = (Split-Path -Parent $vswhere) + ';' + $env:PATH }
$VsDevCmd = Join-Path $VisualStudioRoot 'Common7/Tools/VsDevCmd.bat'
$MSBuild = Join-Path $VisualStudioRoot 'MSBuild/Current/Bin/MSBuild.exe'
