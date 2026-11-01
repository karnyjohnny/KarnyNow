@echo off
REM wiretap/capture_ref_edge.bat — same as capture_ref.bat but for Edge
REM (msedge honors SSLKEYLOGFILE identically). Close ALL Edge windows first.
setlocal
set KEYS=%USERPROFILE%\gfn_keys.log
if exist "%KEYS%" del "%KEYS%"
set SSLKEYLOGFILE=%KEYS%
echo SSLKEYLOGFILE=%KEYS%
set EDGE=
if exist "%ProgramFiles(x86)%\Microsoft\Edge\Application\msedge.exe" set EDGE=%ProgramFiles(x86)%\Microsoft\Edge\Application\msedge.exe
if exist "%ProgramFiles%\Microsoft\Edge\Application\msedge.exe" set EDGE=%ProgramFiles%\Microsoft\Edge\Application\msedge.exe
if "%EDGE%"=="" (
    echo ERROR: msedge.exe not found — use capture_ref.bat for Chrome.
    exit /b 1
)
start "" "%EDGE%" --user-data-dir="%TEMP%\gfn_capture_profile" https://play.geforcenow.com
echo.
echo Edge started with a FRESH capture profile.
echo   keys log : %KEYS%   (NEVER push this file)
endlocal
