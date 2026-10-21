# Builds the Windows app in Release and packs it into release\QBTCamera-Windows-<version>.zip
# (app, virtual camera module, installer, manual).
#   powershell -ExecutionPolicy Bypass -File scripts\make_release.ps1 [-Version 1.0]
param([string]$Version = '1.0')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

$cmake = Get-ChildItem "C:\Program Files*\Microsoft Visual Studio\2022\*\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" |
    Select-Object -First 1
if (-not $cmake) { $cmake = Get-Command cmake -ErrorAction Stop }
$build = Join-Path $root 'windows\build'
if (-not (Test-Path (Join-Path $build 'CMakeCache.txt'))) {
    & $cmake.FullName -S (Join-Path $root 'windows') -B $build -G 'Visual Studio 17 2022' -A x64
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
}
& $cmake.FullName --build $build --config Release
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }

$name = "QBTCamera-Windows-$Version"
$stage = Join-Path $root "release\$name"
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item (Join-Path $build 'Release\QBTCamera.exe'), (Join-Path $build 'Release\QBTVirtualCamera.dll') $stage
Copy-Item (Join-Path $root 'windows\package\*') $stage
Copy-Item (Join-Path $root 'docs\MANUAL.md') $stage

$zip = Join-Path $root "release\$name.zip"
Remove-Item $zip -Force -ErrorAction SilentlyContinue
Compress-Archive -Path "$stage\*" -DestinationPath $zip
Write-Host "Release: $zip"
