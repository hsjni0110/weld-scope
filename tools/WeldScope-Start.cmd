@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Start-WeldScope-Practice.ps1"
if errorlevel 1 pause
