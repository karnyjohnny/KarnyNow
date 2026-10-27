
#!/bin/sh
# cross_check.sh — the R21 gate: every C file must cross-compile for the
# Windows 7 target with ZERO warnings before it leaves a session.
# Uses the canonical F1 flag set (HANDOFF §6/F1, BOOTSTRAP §9 F1).
set -e
cd "$(dirname "$0")/.."

DEFS="-D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -DNTDDI_VERSION=0x06010000"
CFLAGS="-std=c11 -O2 -Wall -Wextra -Werror -march=core2 -msse3 $DEFS -Istreamer/include -Istreamer/src -Ithird_party/tiny-aes-c -Ithird_party/crypto-algorithms-sha1"
LDFLAGS="-static -static-libgcc \
 -Wl,--major-subsystem-version,6.01 -Wl,--minor-subsystem-version,01 \
 -Wl,--major-os-version,6.01 -Wl,--minor-os-version,01"
mkdir -p build/cross

for CC in x86_64-w64-mingw32-gcc i686-w64-mingw32-gcc; do
    arch=$(echo "$CC" | cut -d- -f1)
    echo "== $CC =="

    # R14/F1 verification: the macros and SIMD ISAs must actually be in effect
    echo | $CC $DEFS -march=core2 -msse3 -dM -E - \
        | grep -E '^#define (_WIN32_WINNT|WINVER|NTDDI_VERSION|__SSE3__|__SSSE3__) ' \
        | sort

    $CC $CFLAGS -c streamer/src/knlog.c  -o build/cross/knlog_$arch.o
    $CC $CFLAGS -c streamer/src/knring.c -o build/cross/knring_$arch.o
    $CC $CFLAGS -c streamer/src/knfec.c  -o build/cross/knfec_$arch.o
    $CC $CFLAGS -c streamer/src/kn_aes.c -o build/cross/kn_aes_$arch.o
    $CC $CFLAGS -c streamer/src/kn_sha1.c -o build/cross/kn_sha1_$arch.o
    $CC $CFLAGS -c streamer/src/knsrtp.c -o build/cross/knsrtp_$arch.o
    $CC $CFLAGS -c streamer/src/kn_diag.c -o build/cross/kn_diag_$arch.o
    $CC $CFLAGS -c streamer/src/kn_selftest.c -o build/cross/kn_selftest_$arch.o
    $CC $CFLAGS -c streamer/src/kn_decode.c -o build/cross/kn_decode_$arch.o
    $CC $CFLAGS -c streamer/src/kn_d3d.c   -o build/cross/kn_d3d_$arch.o
    $CC $CFLAGS -c streamer/src/kn_gs.c    -o build/cross/kn_gs_$arch.o
    $CC $CFLAGS -c streamer/src/kn_rx.c    -o build/cross/kn_rx_$arch.o
    $CC $CFLAGS -c streamer/src/kn_perf.c  -o build/cross/kn_perf_$arch.o
    $CC $CFLAGS -c streamer/src/kn_audio.c -o build/cross/kn_audio_$arch.o
    $CC $CFLAGS -c streamer/src/kn_net.c   -o build/cross/kn_net_$arch.o
    # Round XXII: the Win32+mbedTLS body of kn_net_mbedtls.c MUST compile
    # here whenever the pinned tree is present. Sandbox cmake (POSIX stubs)
    # and this script (no mbedTLS before) never covered it — a bad edit
    # once shipped to the target broken (T10 r16: 'port' undeclared in the
    # DNS path + missing <stdlib.h> that only i686 exposed). The tree is
    # gitignored; T10/prepare scripts fetch it, CI-less sandboxes skip.
    if [ -d third_party/mbedtls/include/mbedtls ]; then
        $CC $CFLAGS -DKN_TLS_BACKEND_MBEDTLS=1 -Ithird_party/mbedtls/include \
            '-DMBEDTLS_USER_CONFIG_FILE="kn_mbedtls_user_config.h"' \
            -c streamer/src/kn_net_mbedtls.c -o build/cross/kn_net_mbedtls_$arch.o
    else
        echo "SKIP kn_net_mbedtls cross-compile (third_party/mbedtls absent)"
    fi
    $CC $CFLAGS -c streamer/src/kn_sock.c  -o build/cross/kn_sock_$arch.o
    $CC $CFLAGS -c streamer/src/kn_ws.c    -o build/cross/kn_ws_$arch.o
    $CC $CFLAGS -c streamer/src/kn_rtsp.c  -o build/cross/kn_rtsp_$arch.o
    $CC $CFLAGS -c streamer/src/kn_json.c  -o build/cross/kn_json_$arch.o
    $CC $CFLAGS -c streamer/src/kn_http.c  -o build/cross/kn_http_$arch.o
    $CC $CFLAGS -c streamer/src/kn_cm.c    -o build/cross/kn_cm_$arch.o
    $CC $CFLAGS -c streamer/src/kn_cert.c  -o build/cross/kn_cert_$arch.o
    # round XXX (R3-A): the kn_cert REAL body must survive the exact target
    # flag set whenever the mbedTLS tree exists (the round-XXII rule)
    if [ -d third_party/mbedtls/include/mbedtls ]; then
        $CC $CFLAGS -DKN_TLS_BACKEND_MBEDTLS=1 -Ithird_party/mbedtls/include \
            '-DMBEDTLS_USER_CONFIG_FILE="kn_mbedtls_user_config.h"' \
            -c streamer/src/kn_cert.c -o build/cross/kn_cert_real_$arch.o
    else
        echo "SKIP kn_cert real-body cross-compile (third_party/mbedtls absent)"
    fi
    $CC $CFLAGS -c streamer/src/kn_dtls.c  -o build/cross/kn_dtls_$arch.o
    $CC $CFLAGS -c streamer/src/kn_sctp.c  -o build/cross/kn_sctp_$arch.o
    # round XXXIII (R3-C): the kn_sctp REAL body + the test's real section
    # must survive the exact target flags whenever the usrsctp tree exists
    if [ -d third_party/usrsctp/usrsctplib ]; then
        $CC $CFLAGS -DKN_SCTP_BACKEND_USRSCTP=1 -Ithird_party/usrsctp/usrsctplib \
            -c streamer/src/kn_sctp.c -o build/cross/kn_sctp_real_$arch.o
        $CC $CFLAGS -DKN_SCTP_BACKEND_USRSCTP=1 -Ithird_party/usrsctp/usrsctplib \
            -c streamer/tests/test_knsctp.c -o build/cross/test_knsctp_real_$arch.o
        # round XXXIII hotfix leg (the SIXTH target catch): the T10
        # configure failed on a WIN32-only CMake branch the POSIX sandbox
        # never executes (plain-vs-keyword target_link_libraries on the
        # usrsctp target). Compile legs cannot see LINK closure — so for
        # x86_64 (the target arch) also CROSS-BUILD usrsctp and LINK the
        # real suite with the exact F1 flags: proves the static closure
        # (ws2_32/iphlpapi, no winpthread DLL) before the target ever
        # configures. i686 keeps the compile-only legs.
        if [ "$arch" = "x86_64" ]; then
            printf 'set(CMAKE_SYSTEM_NAME Windows)\nset(CMAKE_SYSTEM_PROCESSOR x86_64)\nset(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)\n' > build/cross/mingw64-toolchain.cmake
            cmake -S third_party/usrsctp -B build/cross/usrsctp-x64 -G Ninja \
                -DCMAKE_TOOLCHAIN_FILE="$PWD/build/cross/mingw64-toolchain.cmake" \
                -DCMAKE_BUILD_TYPE=Release -Dsctp_debug=0 -Dsctp_werror=0 \
                -Dsctp_build_programs=0 -Dsctp_build_shared_lib=0 > /dev/null
            cmake --build build/cross/usrsctp-x64 > /dev/null
            $CC $CFLAGS -DKN_SCTP_BACKEND_USRSCTP=1 -Ithird_party/usrsctp/usrsctplib \
                streamer/tests/test_knsctp.c streamer/src/kn_sctp.c \
                streamer/src/kn_sock.c streamer/src/kn_sha1.c \
                third_party/crypto-algorithms-sha1/sha1.c \
                build/cross/usrsctp-x64/usrsctplib/libusrsctp.a \
                $LDFLAGS -o build/cross/test_knsctp_real_x86_64.exe \
                -lws2_32 -liphlpapi
        fi
    else
        echo "SKIP kn_sctp real-body cross-compile (third_party/usrsctp absent)"
    fi
    # round XXXI (R3-B): the kn_dtls REAL body AND the test's real battery
    # section must survive the exact target flag set (the round-XXII rule;
    # the sandbox shell gates never define the backend)
    if [ -d third_party/mbedtls/include/mbedtls ]; then
        $CC $CFLAGS -DKN_TLS_BACKEND_MBEDTLS=1 -Ithird_party/mbedtls/include \
            '-DMBEDTLS_USER_CONFIG_FILE="kn_mbedtls_user_config.h"' \
            -c streamer/src/kn_dtls.c -o build/cross/kn_dtls_real_$arch.o
        $CC $CFLAGS -DKN_TLS_BACKEND_MBEDTLS=1 -Ithird_party/mbedtls/include \
            '-DMBEDTLS_USER_CONFIG_FILE="kn_mbedtls_user_config.h"' \
            -c streamer/tests/test_kndtls.c -o build/cross/test_kndtls_real_$arch.o
    else
        echo "SKIP kn_dtls real-body cross-compile (third_party/mbedtls absent)"
    fi
    $CC $CFLAGS -c third_party/tiny-aes-c/aes.c -o build/cross/tinyaes_$arch.o
    $CC $CFLAGS -c third_party/crypto-algorithms-sha1/sha1.c -o build/cross/sha1_$arch.o
    $CC $CFLAGS streamer/src/knlog.c streamer/src/knring.c streamer/tests/test_knlog.c \
        $LDFLAGS -o build/cross/test_knlog_$arch.exe
    $CC $CFLAGS streamer/src/knlog.c streamer/src/knring.c streamer/tests/test_knring.c \
        $LDFLAGS -o build/cross/test_knring_$arch.exe
    $CC $CFLAGS streamer/src/knfec.c streamer/tests/test_knfec.c \
        $LDFLAGS -o build/cross/test_knfec_$arch.exe
    $CC $CFLAGS streamer/src/kn_gs.c streamer/tests/test_kngs.c \
        $LDFLAGS -o build/cross/test_kngs_$arch.exe
    $CC $CFLAGS streamer/src/kn_rx.c streamer/src/kn_gs.c streamer/src/knfec.c \
        streamer/src/knsrtp.c streamer/src/kn_aes.c streamer/src/kn_sha1.c \
        third_party/tiny-aes-c/aes.c third_party/crypto-algorithms-sha1/sha1.c \
        streamer/tests/test_knrx.c \
        $LDFLAGS -o build/cross/test_knrx_$arch.exe
    $CC $CFLAGS streamer/src/kn_audio.c streamer/src/knlog.c \
        streamer/src/knring.c streamer/tests/test_knaudio.c \
        $LDFLAGS -o build/cross/test_knaudio_$arch.exe -lole32
    $CC $CFLAGS streamer/src/kn_sock.c streamer/src/kn_sha1.c \
        third_party/crypto-algorithms-sha1/sha1.c \
        streamer/tests/test_knsock.c \
        $LDFLAGS -o build/cross/test_knsock_$arch.exe -lws2_32
    $CC $CFLAGS streamer/src/kn_ws.c streamer/src/kn_sha1.c \
        third_party/crypto-algorithms-sha1/sha1.c \
        streamer/tests/test_knws.c \
        $LDFLAGS -o build/cross/test_knws_$arch.exe
    $CC $CFLAGS streamer/src/kn_rtsp.c streamer/tests/test_knrtsp.c \
        $LDFLAGS -o build/cross/test_knrtsp_$arch.exe
    $CC $CFLAGS streamer/src/kn_json.c streamer/src/kn_http.c \
        streamer/src/kn_cm.c streamer/tests/test_kncm.c \
        $LDFLAGS -o build/cross/test_kncm_$arch.exe
    $CC $CFLAGS streamer/src/kn_cert.c streamer/tests/test_kncert.c \
        $LDFLAGS -o build/cross/test_kncert_$arch.exe
    $CC $CFLAGS streamer/src/kn_dtls.c streamer/src/kn_sock.c streamer/src/kn_sha1.c \
        third_party/crypto-algorithms-sha1/sha1.c streamer/tests/test_kndtls.c \
        $LDFLAGS -o build/cross/test_kndtls_$arch.exe -lws2_32
    $CC $CFLAGS streamer/src/kn_sctp.c streamer/tests/test_knsctp.c \
        $LDFLAGS -o build/cross/test_knsctp_$arch.exe
    $CC $CFLAGS streamer/src/kn_aes.c streamer/src/kn_sha1.c streamer/src/knsrtp.c \
    third_party/tiny-aes-c/aes.c third_party/crypto-algorithms-sha1/sha1.c \
        streamer/tests/test_knsrtp.c $LDFLAGS -o build/cross/test_knsrtp_$arch.exe
    # kndiag/knselftest/knvstd pull the full core; winmm for timeBeginPeriod,
    # d3d9 for kn_d3d (both are import archives — F1 -static accepts them)
    KCORE="streamer/src/knlog.c streamer/src/knring.c streamer/src/knfec.c \
        streamer/src/kn_aes.c streamer/src/kn_sha1.c streamer/src/knsrtp.c \
        streamer/src/kn_diag.c streamer/src/kn_selftest.c \
        streamer/src/kn_decode.c streamer/src/kn_d3d.c streamer/src/kn_gs.c \
        streamer/src/kn_perf.c streamer/src/kn_audio.c \
        streamer/src/kn_net.c streamer/src/kn_rx.c streamer/src/kn_sock.c \
        streamer/src/kn_ws.c streamer/src/kn_rtsp.c \
        streamer/src/kn_json.c streamer/src/kn_http.c streamer/src/kn_cm.c \
        streamer/src/kn_cert.c streamer/src/kn_dtls.c \
        streamer/src/kn_sctp.c \
        third_party/tiny-aes-c/aes.c third_party/crypto-algorithms-sha1/sha1.c"
    $CC $CFLAGS $KCORE streamer/tests/test_kndiag.c \
        $LDFLAGS -o build/cross/test_kndiag_$arch.exe -lwinmm -ld3d9 -lole32 \
        -lws2_32 -lsecur32 -lcrypt32
    $CC $CFLAGS $KCORE streamer/tests/test_knselftest.c \
        $LDFLAGS -o build/cross/test_knselftest_$arch.exe -lwinmm -ld3d9 -lole32 \
        -lws2_32 -lsecur32 -lcrypt32
    $CC $CFLAGS $KCORE streamer/src/knvstd_main.c \
        $LDFLAGS -o build/cross/knvstd_$arch.exe -lwinmm -ld3d9 -lole32 \
        -lws2_32 -lsecur32 -lcrypt32
    # round XXVII: the paranoid knvstd leg (target-GCC catch classes —
    # T10 r20 flagged set-but-unused which sandbox GCC 12 keeps out of -Wall)
    $CC $CFLAGS -Werror -Wunused-but-set-variable -Wformat-truncation=2 \
        -c streamer/src/knvstd_main.c -o build/cross/knvstd_paranoid_$arch.o
    ls -la build/cross/*_$arch.exe | awk '{print $5, $9}'

    # kn_decode/kn_selftest against the REAL FFmpeg headers (the user's exact
    # commit bf0e831606, libavcodec 63) — compile-only: no import libs here,
    # full link happens on the target via CMake KN_FFMPEG_PREFIX.
    if [ -d third_party/ffmpeg-headers/libavcodec ]; then
        $CC $CFLAGS -DKN_HAVE_FFMPEG=1 -isystem third_party/ffmpeg-headers \
            -c streamer/src/kn_decode.c -o build/cross/kn_decode_ff_$arch.o
        $CC $CFLAGS -DKN_HAVE_FFMPEG=1 \
            -c streamer/src/kn_selftest.c -o build/cross/kn_selftest_ff_$arch.o
    fi
done

echo "CROSS-CHECK PASS (zero warnings under -Wall -Wextra -Werror)"


