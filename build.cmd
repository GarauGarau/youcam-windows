@echo off
setlocal
cd /d "%~dp0"
where cmake >nul 2>nul
if errorlevel 1 (
  echo Install Microsoft C++ Build Tools with Desktop development with C++, Windows SDK and CMake tools.
  echo Then open Developer Command Prompt and run build.cmd again.
  exit /b 1
)
cmake -S . -B build -A x64
if errorlevel 1 exit /b 1
cmake --build build --config Release --parallel
if errorlevel 1 exit /b 1
echo Ready: build\Release\YouCam.exe
