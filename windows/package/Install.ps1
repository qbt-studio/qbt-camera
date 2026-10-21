# QBT Camera for Windows — installer (run "Install QBT Camera.cmd").
# Copies the app to Program Files, registers the "QBT Camera" virtual webcam for all apps,
# adds a Start menu shortcut and starts the app. Asks for administrator rights once.
#   Install.ps1              install or update
#   Install.ps1 -Uninstall   remove everything
param([switch]$Uninstall)
$ErrorActionPreference = 'Stop'

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    if ($Uninstall) { $arguments += '-Uninstall' }
    $process = Start-Process powershell.exe -Verb RunAs -ArgumentList $arguments -Wait -PassThru
    exit $process.ExitCode
}

$targetDir = Join-Path $env:ProgramFiles 'QBT Camera Studio'
$app = Join-Path $targetDir 'QBTCamera.exe'
$dll = Join-Path $targetDir 'QBTVirtualCamera.dll'
$shortcut = Join-Path ([Environment]::GetFolderPath('CommonPrograms')) 'QBT Camera.lnk'

# A running QBT Camera and the Windows camera service hold the files open.
Get-Process QBTCamera -ErrorAction SilentlyContinue | Stop-Process -Force
foreach ($service in 'FrameServerMonitor', 'FrameServer') {
    Stop-Service $service -Force -ErrorAction SilentlyContinue
}

if ($Uninstall) {
    if (Test-Path $app) { Start-Process $app -ArgumentList '--uninstall-camera' -Wait }
    if (Test-Path $dll) { Start-Process regsvr32.exe -ArgumentList '/s', '/u', "`"$dll`"" -Wait }
    Remove-Item $shortcut -Force -ErrorAction SilentlyContinue
    Remove-Item 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'QBT Camera' -ErrorAction SilentlyContinue
    Remove-Item $targetDir -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host 'QBT Camera removed.'
    exit 0
}

New-Item -ItemType Directory -Force $targetDir | Out-Null
Copy-Item (Join-Path $PSScriptRoot 'QBTCamera.exe') $app -Force
Copy-Item (Join-Path $PSScriptRoot 'QBTVirtualCamera.dll') $dll -Force

$registration = Start-Process regsvr32.exe -ArgumentList '/s', "`"$dll`"" -Wait -PassThru
if ($registration.ExitCode -ne 0) { throw "Camera module registration failed ($($registration.ExitCode))" }
# System camera: listed in every app for all users, even while QBT Camera is closed.
$camera = Start-Process $app -ArgumentList '--install-camera' -Wait -PassThru
if ($camera.ExitCode -ne 0) { throw "Virtual camera setup failed ($($camera.ExitCode))" }

$shell = New-Object -ComObject WScript.Shell
$link = $shell.CreateShortcut($shortcut)
$link.TargetPath = $app
$link.WorkingDirectory = $targetDir
$link.Description = 'QBT Camera — iPhone as a webcam'
$link.Save()

Write-Host 'QBT Camera installed. Starting it…'
# Start as the signed-in user, not as administrator.
Start-Process explorer.exe -ArgumentList "`"$app`""
exit 0
