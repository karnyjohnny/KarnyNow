@echo off
REM wiretap/extract_ref.bat — turn the raw capture into the three TEXT
REM files that get pushed. Needs: gfn_ref.pcapng + gfn_keys.log (paths
REM below) and Wireshark's tshark. See README.md.
setlocal
set PCAP=%USERPROFILE%\gfn_ref.pcapng
set KEYS=%USERPROFILE%\gfn_keys.log
set OUT=%USERPROFILE%\gfn_ref_out
set TSHARK="C:\Program Files\Wireshark\tshark.exe"
if not exist %TSHARK% set TSHARK=tshark
if not exist "%PCAP%" (
    echo ERROR: %PCAP% not found — save the Wireshark capture there first.
    exit /b 1
)
mkdir "%OUT%" 2>nul

echo [1/3] HTTP/2 API traffic (decrypted; contains the v2/session body)...
%TSHARK% -r "%PCAP%" -o "tls.keylog_file:%KEYS%" -Y "http2" -T json > "%OUT%\gfn_ref_h2.json"

echo [2/3] WebSocket frame payloads (hex)...
%TSHARK% -r "%PCAP%" -o "tls.keylog_file:%KEYS%" -Y "websocket" -T fields -e frame.number -e frame.time_relative -e ip.src -e ip.dst -e websocket.opcode -e websocket.payload > "%OUT%\gfn_ref_ws.txt"

echo [3/3] Server names + request URLs (version params live here)...
%TSHARK% -r "%PCAP%" -o "tls.keylog_file:%KEYS%" -Y "tls.handshake.extensions_server_name || http.request || http2.headers" -T fields -e frame.number -e tls.handshake.extensions_server_name -e http.host -e http.request.uri -e http2.headers.path > "%OUT%\gfn_ref_urls.txt"

echo.
echo Done: %OUT%
dir "%OUT%"
echo.
echo SANITY: gfn_ref_h2.json must contain readable JSON (search for
echo   sessionRequestData). Empty/opaque = the browser was already
echo   running when capture_ref.bat started; redo with ALL windows closed.
echo SAFETY: eyeball the three files for Authorization/JWT values and
echo   strike them before pushing. Then DELETE %KEYS% and %PCAP% —
echo   the key log decrypts your whole capture and must never leave
echo   the machine.
endlocal
