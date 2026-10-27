@echo off
setlocal
cd /d "%~dp0"
where conda >nul 2>nul
if errorlevel 1 (
    echo Open Miniforge Prompt, then run test-native.cmd again.
    exit /b 1
)
set QT_QPA_PLATFORM=windows
set YOUCAM_NATIVE_TESTS=1
set YOUCAM_ENCODER=software
call conda run --no-capture-output -n youcam-windows python -m unittest tests.test_windows_native tests.test_recorder -v
exit /b %errorlevel%
