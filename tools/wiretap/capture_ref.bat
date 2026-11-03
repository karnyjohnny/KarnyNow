@echo off
REM wiretap/capture_ref.bat — start Chrome with TLS key logging for the
REM official GFN web client capture (see README.md in this directory).
REM ALL Chrome windows must be CLOSED first, or SSLKEYLOGFILE is ignored.
setlocal
set KEYS=%USERPROFILE%\gfn_keys.log
if exist "%KEYS%" del "%KEYS%"
set SSLKEYLOGFILE=%KEYS%
echo SSLKEYLOGFILE=%KEYS%
set CHROME=
if exist "%ProgramFiles%\Google\Chrome\Application\chrome.exe" set CHROME=%ProgramFiles%\Google\Chrome\Application\chrome.exe
if exist "%ProgramFiles(x86)%\Google\Chrome\Application\chrome.exe" set CHROME=%ProgramFiles(x86)%\Google\Chrome\Application\chrome.exe
if exist "C:\Programy\Chrome\App\Chrome-bin\chrome.exe" set CHROME=C:\Programy\Chrome\App\Chrome-bin\chrome.exe
if "%CHROME%"=="" (
    echo ERROR: chrome.exe not found — use capture_ref_edge.bat instead.
    exit /b 1
)
start "" "%CHROME%" --user-data-dir="%TEMP%\gfn_capture_profile" https://play.geforcenow.com
echo.
echo Chrome started with a FRESH capture profile.
echo   keys log : %KEYS%   (NEVER push this file)
echo Now: start Wireshark if you have not, log in, start a game,
echo wait ~60 s, stop the capture, save %%USERPROFILE%%\gfn_ref.pcapng,
echo then run extract_ref.bat.
endlocal
