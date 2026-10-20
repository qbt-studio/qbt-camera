@echo off
chcp 65001 >nul
title QBT Camera Viewer
python "%~dp0qbt_viewer.py" %*
pause
