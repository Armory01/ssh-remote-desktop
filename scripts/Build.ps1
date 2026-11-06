[CmdletBinding()]
param([ValidateSet('x64','Win32')][string]$Architecture = 'x64', [ValidateSet('Release','Debug')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$cmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
if ($cmakeCommand) { $cmake = $cmakeCommand.Source }
else {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (!(Test-Path -LiteralPath $vswhere)) { throw 'CMake or Visual Studio Build Tools is required on the development machine.' }
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $cmake = Join-Path $installation 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
}
$build = Join-Path $root "build/$Architecture"
& $cmake -S $root -B $build -G 'Visual Studio 16 2019' -A $Architecture '-DCMAKE_SYSTEM_VERSION=10.0.19041.0'
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& $cmake --build $build --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
& (Join-Path (Split-Path $cmake -Parent) 'ctest.exe') --test-dir $build -C $Configuration --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
Write-Output (Join-Path $build "$Configuration/ra.exe")
