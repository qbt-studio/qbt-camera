@echo off
rem Double-click to remove QBT Camera and its virtual webcam.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install.ps1" -Uninstall
