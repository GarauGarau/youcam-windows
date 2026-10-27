@echo off
setlocal
cd /d "%~dp0"
where conda >nul 2>nul
if errorlevel 1 (
    echo Open Miniforge Prompt, then run run.cmd again.
    exit /b 1
)
call conda run --no-capture-output -n youcam-windows python -m youcam %*
exit /b %errorlevel%
