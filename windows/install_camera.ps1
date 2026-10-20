# Installs (or updates / removes) the "QBT Camera" virtual camera module, QBTVirtualCamera.dll.
# Needs administrator rights: relaunches itself elevated, so Windows asks for confirmation.
#   .\install_camera.ps1              install or update from build\Release
#   .\install_camera.ps1 -Uninstall   remove
param(
    [string]$Source = (Join-Path $PSScriptRoot 'build\Release\QBTVirtualCamera.dll'),
    [string]$App = (Join-Path $PSScriptRoot 'build\Release\QBTCamera.exe'),
    [switch]$Uninstall
)
$ErrorActionPreference = 'Stop'

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-Source', "`"$Source`"",
                   '-App', "`"$App`"")
    if ($Uninstall) { $arguments += '-Uninstall' }
    $process = Start-Process powershell.exe -Verb RunAs -ArgumentList $arguments -Wait -PassThru
    exit $process.ExitCode
}

# Program Files: readable by the Frame Server service account (LocalService).
$targetDir = Join-Path $env:ProgramFiles 'QBT Camera Studio'
$target = Join-Path $targetDir 'QBTVirtualCamera.dll'

# Frame Server keeps the DLL loaded while any app uses a camera; stop it to replace the file.
foreach ($service in 'FrameServerMonitor', 'FrameServer') {
    Stop-Service $service -Force -ErrorAction SilentlyContinue
}

if ($Uninstall) {
    if (Test-Path $App) { Start-Process $App -ArgumentList '--uninstall-camera' -Wait }
    if (Test-Path $target) {
        Start-Process regsvr32.exe -ArgumentList '/s', '/u', "`"$target`"" -Wait
        Remove-Item $target -Force
    }
    exit 0
}

New-Item -ItemType Directory -Force $targetDir | Out-Null
Copy-Item $Source $target -Force
$registration = Start-Process regsvr32.exe -ArgumentList '/s', "`"$target`"" -Wait -PassThru
if ($registration.ExitCode -ne 0) { exit $registration.ExitCode }

# System camera: stays in every app's camera list for all users, even while QBTCamera.exe is closed.
$camera = Start-Process $App -ArgumentList '--install-camera' -Wait -PassThru
exit $camera.ExitCode
