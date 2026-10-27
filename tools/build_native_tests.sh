
#!/bin/sh
# build_native_tests.sh — compile and RUN all streamer unit tests on the
# Linux host (BOOTSTRAP §10.3: test the boring parts on the host first).
# The same sources cross-compile for Win7 (tools/cross_check.sh) and the
# test binaries can also be run there for real-environment verification.
set -e
cd "$(dirname "$0")/.."

CC="${CC:-gcc}"
CFLAGS="-std=c11 -O1 -g -Wall -Wextra -Wpedantic -Istreamer/include -Istreamer/src -Ithird_party/tiny-aes-c -Ithird_party/crypto-algorithms-sha1"
mkdir -p build

echo "== native compile (-Wall -Wextra -Wpedantic) =="
KNCORE="streamer/src/knlog.c streamer/src/knring.c streamer/src/knfec.c \
    streamer/src/kn_aes.c streamer/src/kn_sha1.c streamer/src/knsrtp.c \
    streamer/src/kn_diag.c streamer/src/kn_selftest.c \
    streamer/src/kn_decode.c streamer/src/kn_d3d.c streamer/src/kn_gs.c \
    streamer/src/kn_perf.c streamer/src/kn_audio.c streamer/src/kn_net.c \
    streamer/src/kn_rx.c streamer/src/kn_sock.c \
    streamer/src/kn_ws.c streamer/src/kn_rtsp.c \
    streamer/src/kn_json.c streamer/src/kn_http.c streamer/src/kn_cm.c \
    streamer/src/kn_cert.c streamer/src/kn_dtls.c \
    streamer/src/kn_sctp.c \
    third_party/tiny-aes-c/aes.c third_party/crypto-algorithms-sha1/sha1.c"
$CC $CFLAGS $KNCORE streamer/tests/test_knlog.c -o build/test_knlog -pthread -lm
$CC $CFLAGS $KNCORE streamer/tests/test_knring.c -o build/test_knring -pthread -lm
$CC $CFLAGS streamer/src/knfec.c streamer/tests/test_knfec.c -o build/test_knfec
$CC $CFLAGS streamer/src/kn_aes.c streamer/src/kn_sha1.c streamer/src/knsrtp.c \
    third_party/tiny-aes-c/aes.c third_party/crypto-algorithms-sha1/sha1.c \
    streamer/tests/test_knsrtp.c -o build/test_knsrtp
$CC $CFLAGS $KNCORE streamer/tests/test_kndiag.c -o build/test_kndiag -pthread -lm
$CC $CFLAGS $KNCORE streamer/tests/test_knselftest.c -o build/test_knselftest -pthread -lm
$CC $CFLAGS streamer/src/kn_gs.c streamer/tests/test_kngs.c -o build/test_kngs
$CC $CFLAGS streamer/src/kn_rx.c streamer/src/kn_gs.c streamer/src/knfec.c \
    streamer/src/knsrtp.c streamer/src/kn_aes.c streamer/src/kn_sha1.c \
    third_party/tiny-aes-c/aes.c third_party/crypto-algorithms-sha1/sha1.c \
    streamer/tests/test_knrx.c -o build/test_knrx
$CC $CFLAGS streamer/src/kn_audio.c streamer/tests/test_knaudio.c -o build/test_knaudio -lm
$CC $CFLAGS streamer/src/kn_sock.c streamer/src/kn_sha1.c \
    third_party/crypto-algorithms-sha1/sha1.c \
    streamer/tests/test_knsock.c -o build/test_knsock
$CC $CFLAGS streamer/src/kn_ws.c streamer/src/kn_sha1.c \
    third_party/crypto-algorithms-sha1/sha1.c \
    streamer/tests/test_knws.c -o build/test_knws
$CC $CFLAGS streamer/src/kn_rtsp.c streamer/tests/test_knrtsp.c -o build/test_knrtsp
$CC $CFLAGS streamer/src/kn_json.c streamer/src/kn_http.c \
    streamer/src/kn_cm.c streamer/tests/test_kncm.c -o build/test_kncm
$CC $CFLAGS streamer/src/kn_cert.c streamer/tests/test_kncert.c \
    -o build/test_kncert
$CC $CFLAGS streamer/src/kn_dtls.c streamer/src/kn_sock.c streamer/src/kn_sha1.c \
    third_party/crypto-algorithms-sha1/sha1.c streamer/tests/test_kndtls.c \
    -o build/test_kndtls
$CC $CFLAGS streamer/src/kn_sctp.c streamer/tests/test_knsctp.c \
    -o build/test_knsctp
$CC $CFLAGS $KNCORE streamer/src/knvstd_main.c -o build/knvstd -pthread -lm
# round XXVII permanent gate (the T10-r20 target catch): GCC 16.2 flags
# -Wunused-but-set-variable which sandbox GCC 12 does NOT put in -Wall;
# the paranoid knvstd leg pins both target-catch classes every round.
echo "== knvstd paranoid: -Wunused-but-set-variable + -Wformat-truncation=2 =="
$CC $CFLAGS -Werror -Wunused-but-set-variable -Wformat-truncation=2 \
    -c streamer/src/knvstd_main.c -o build/knvstd_paranoid.o

echo "== kn_decode.c/kn_selftest.c against REAL FFmpeg headers (compile-only) =="
rcff=0
if [ -d third_party/ffmpeg-headers/libavcodec ]; then
    $CC $CFLAGS -DKN_HAVE_FFMPEG=1 -isystem third_party/ffmpeg-headers \
        -c streamer/src/kn_decode.c -o build/kn_decode_ff.o || rcff=1
    $CC $CFLAGS -DKN_HAVE_FFMPEG=1 \
        -c streamer/src/kn_selftest.c -o build/kn_selftest_ff.o || rcff=1
else
    echo "SKIP: third_party/ffmpeg-headers not present"
fi

rc1=0; rc2=0; rc3=0; rc4=0

echo "== run test_knlog =="
KN_TEST_KEEP=1 ./build/test_knlog || rc1=$?

echo "== run test_knring =="
./build/test_knring || rc2=$?

echo "== run test_knfec =="
./build/test_knfec || rc4=$?

echo "== run test_knsrtp =="
rc5=0
./build/test_knsrtp || rc5=$?

echo "== run test_kndiag =="
rc6=0
KN_TEST_KEEP=1 ./build/test_kndiag || rc6=$?

echo "== run test_knselftest =="
rc8=0
KN_TEST_KEEP=1 ./build/test_knselftest || rc8=$?

echo "== run test_kngs =="
rc11=0
./build/test_kngs || rc11=$?

echo "== run test_knrx =="
rc13=0
./build/test_knrx || rc13=$?

echo "== run test_knaudio =="
rc12=0
./build/test_knaudio || rc12=$?

echo "== run test_knsock =="
rc14=0
./build/test_knsock || rc14=$?

echo "== run test_knws =="
rc15=0
./build/test_knws || rc15=$?

echo "== run test_knrtsp =="
rc16=0
./build/test_knrtsp || rc16=$?

echo "== run test_kncm =="
rc17=0
./build/test_kncm || rc17=$?

echo "== run test_kncert (stub mode in this script; the REAL battery runs via ctest/T10 with the mbedTLS backend) =="
rc18=0
./build/test_kncert || rc18=$?

echo "== run test_kndtls (stub mode here; the REAL DTLS handshake battery runs via ctest/T10 with the mbedTLS backend) =="
rc19=0
./build/test_kndtls || rc19=$?

echo "== run test_knsctp (stub mode here; the REAL usrsctp battery runs via ctest/T10 when third_party/usrsctp exists) =="
rc20=0
./build/test_knsctp || rc20=$?

echo "== knvstd smoke: --list-selftests + --selftest=fec.vectors =="
rc9=0
./build/knvstd --list-selftests > knvstd_list.json || rc9=$?
./build/knvstd --selftest=fec.vectors --log-dir=knvstd_testlogs > knvstd_fec.json || rc9=$?
./build/knvstd --diagnostics --log-dir=knvstd_testlogs > knvstd_diag.json || rc9=$?

echo "== validate jsonl output (strict JSON parse + schema) =="
python3 tools/check_jsonl.py testlogs testlogs2 || rc3=$?

echo "== validate diag bundle (python zipfile = independent zip reader) =="
rc7=0
python3 tools/check_zip.py testdiag/diag-*.zip || rc7=$?
BUNDLE=$(python3 -c "import json,sys; print(json.load(open('knvstd_diag.json'))['bundle'])" 2>/dev/null) || BUNDLE=""
if [ -n "$BUNDLE" ]; then
    python3 tools/check_zip.py "$BUNDLE" || rc7=1
else
    echo "FAIL: knvstd --diagnostics did not report a bundle path"; rc7=1
fi

echo "== validate knvstd stdout is parseable JSON (json-lines contract) =="
rc10=0
python3 -c "
import json
for f in ('knvstd_list.json', 'knvstd_fec.json', 'knvstd_diag.json',
          'selftest_render_check.json'):
    json.load(open(f))
    print('json ok:', f)
" || rc10=$?

rm -rf testlogs testlogs2 testring_dump.txt testdiag testst knvstd_testlogs \
    knvstd_list.json knvstd_fec.json knvstd_diag.json selftest_render_check.json
if [ $rc1 -eq 0 ] && [ $rc2 -eq 0 ] && [ $rc3 -eq 0 ] && [ $rc4 -eq 0 ] && \
   [ $rc5 -eq 0 ] && [ $rc6 -eq 0 ] && [ $rc7 -eq 0 ] && [ $rc8 -eq 0 ] && \
   [ $rc9 -eq 0 ] && [ $rc10 -eq 0 ] && [ $rcff -eq 0 ] && [ $rc11 -eq 0 ] && \
   [ $rc12 -eq 0 ] && [ $rc13 -eq 0 ] && [ $rc14 -eq 0 ] && \
   [ $rc15 -eq 0 ] && [ $rc16 -eq 0 ] && [ $rc17 -eq 0 ] && [ $rc18 -eq 0 ] && \
   [ $rc19 -eq 0 ] && [ $rc20 -eq 0 ]; then
    echo "NATIVE TESTS PASS"
else
    echo "NATIVE TESTS FAIL (rc knlog=$rc1 knring=$rc2 jsonl=$rc3 knfec=$rc4 knsrtp=$rc5 kndiag=$rc6 zip=$rc7 knselftest=$rc8 knvstd=$rc9 json=$rc10 ffmpeghdr=$rcff kngs=$rc11 knaudio=$rc12 knrx=$rc13 knsock=$rc14 knws=$rc15 knrtsp=$rc16 kncm=$rc17 kncert=$rc18 kndtls=$rc19 knsctp=$rc20)"
    exit 1
fi


