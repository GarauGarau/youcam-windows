@echo off
setlocal
cd /d "%~dp0"
where conda >nul 2>nul
if errorlevel 1 (
    echo Open Miniforge Prompt, then run test.cmd again.
    exit /b 1
)
set QT_QPA_PLATFORM=offscreen
set YOUCAM_ENCODER=software
call conda run --no-capture-output -n youcam-windows python -m unittest discover -s tests -v
if errorlevel 1 exit /b 1
call conda run --no-capture-output -n youcam-windows python -m youcam --smoke-test
exit /b %errorlevel%
