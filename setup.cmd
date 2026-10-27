@echo off
setlocal
cd /d "%~dp0"
where conda >nul 2>nul
if errorlevel 1 (
    echo Open Miniforge Prompt, then run setup.cmd again.
    exit /b 1
)
call conda env create --file environment.yml
if errorlevel 1 exit /b 1
echo.
echo Ready. Run run.cmd to start YouCam, or test.cmd to run the checks.
