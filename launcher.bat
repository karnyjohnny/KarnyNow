@echo off
rem launcher.bat - one-click KarnyNow launcher (slice 5c).
rem Does exactly:  python -m tools.kn_launcher   (from the repo root).
rem Needs Python 3.8 + PyQt5 + httpx on PATH (SETUP.md, rig section).
setlocal
cd /d "%~dp0"
where python >nul 2>nul
if %errorlevel%==0 (
    python -m tools.kn_launcher %*
    goto :eof
)
where py >nul 2>nul
if %errorlevel%==0 (
    py -3 -m tools.kn_launcher %*
    goto :eof
)
echo [launcher] Python not found on PATH.
echo [launcher] Install Python 3.8 (or fix PATH), then double-click again.
pause
exit /b 1
