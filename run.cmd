@echo off
setlocal
cd /d "%~dp0"
if not exist build\Release\YouCam.exe (
  echo Run build.cmd first.
  exit /b 1
)
start "" "%~dp0build\Release\YouCam.exe" %*
