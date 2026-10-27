@echo off
setlocal
cd /d "%~dp0"
ctest --test-dir build -C Release --output-on-failure
exit /b %errorlevel%
