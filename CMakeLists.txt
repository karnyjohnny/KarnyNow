# KarnyNow — knvstd (C11) build for the Windows 7 target (w64devkit: cmake + ninja).
#
#   cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
#   ninja -C build
#   ctest --test-dir build --output-on-failure     # unit tests run ON the target
#
# Canonical F1 flag set is applied for MINGW builds; _WIN32_WINNT=0x0601 is
# defined for OUR code and must be defined for every vendored library too
# (BOOTSTRAP §6.3 trap #1), as must -march=core2 -msse3 (trap #2, risk R14).
cmake_minimum_required(VERSION 3.16)
project(karnynow C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(MINGW)
  # Kept in variables so build.json can record the EFFECTIVE flag set (§7.8)
  set(KN_TARGET_DEFS _WIN32_WINNT=0x0601 WINVER=0x0601 NTDDI_VERSION=0x06010000)
  set(KN_TARGET_OPTS -march=core2 -msse3 -Wall -Wextra)
  add_compile_definitions(${KN_TARGET_DEFS})
  add_compile_options(${KN_TARGET_OPTS})
  add_link_options(-static -static-libgcc
    -Wl,--major-subsystem-version,6.01 -Wl,--minor-subsystem-version,01
    -Wl,--major-os-version,6.01 -Wl,--minor-os-version,01)
endif()

add_library(kncore STATIC
  streamer/src/knlog.c
  streamer/src/knring.c
  streamer/src/knfec.c
  streamer/src/kn_aes.c
  streamer/src/kn_sha1.c
  streamer/src/knsrtp.c
  streamer/src/kn_diag.c
  streamer/src/kn_selftest.c
  streamer/src/kn_decode.c
  streamer/src/kn_d3d.c
  streamer/src/kn_gs.c
  streamer/src/kn_perf.c
  streamer/src/kn_audio.c
  streamer/src/kn_net.c
  streamer/src/kn_rx.c
  streamer/src/kn_sock.c
  streamer/src/kn_ws.c
  streamer/src/kn_rtsp.c
  streamer/src/kn_json.c
  streamer/src/kn_http.c
  streamer/src/kn_cm.c
  streamer/src/kn_cert.c
  streamer/src/kn_dtls.c
  streamer/src/kn_sctp.c
  streamer/src/kn_input.c
  third_party/tiny-aes-c/aes.c
  third_party/crypto-algorithms-sha1/sha1.c
)
set(KN_TLS_BACKEND "mbedtls" CACHE STRING "TLS backend: mbedtls or schannel")
set_property(CACHE KN_TLS_BACKEND PROPERTY STRINGS mbedtls schannel)
if(NOT KN_TLS_BACKEND STREQUAL "schannel" AND
   NOT KN_TLS_BACKEND STREQUAL "mbedtls")
  message(FATAL_ERROR "KN_TLS_BACKEND must be schannel or mbedtls")
endif()
if(KN_TLS_BACKEND STREQUAL "mbedtls")
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls/CMakeLists.txt")
    message(FATAL_ERROR
      "KN_TLS_BACKEND=mbedtls requires mbedTLS 3.6.0 source in third_party/mbedtls")
  endif()
  set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
  set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
  set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
  add_subdirectory(third_party/mbedtls EXCLUDE_FROM_ALL)
  if(MINGW AND CMAKE_C_COMPILER_ID STREQUAL "GNU" AND
     CMAKE_C_COMPILER_VERSION VERSION_GREATER_EQUAL 16)
    target_compile_options(mbedtls PRIVATE
      -Wno-unterminated-string-initialization)
  endif()
  # Round XXXV (D-t9): the additive user config enabling
  # MBEDTLS_SSL_DTLS_SRTP (the use_srtp extension the reference's
  # ClientHello carries — str0m KeyingMaterial, crypto/srtp.rs ALL).
  # PUBLIC on the mbedtls target: the define CHANGES THE STRUCT
  # LAYOUT, so every consumer (kncore -> knvstd/tests) inherits it —
  # an asymmetric build would be silent memory corruption.
  target_compile_definitions(mbedtls PUBLIC
    "MBEDTLS_USER_CONFIG_FILE=\"${CMAKE_CURRENT_SOURCE_DIR}/streamer/src/kn_mbedtls_user_config.h\"")
  target_compile_definitions(kncore PRIVATE KN_TLS_BACKEND_MBEDTLS=1)
  target_sources(kncore PRIVATE streamer/src/kn_net_mbedtls.c)
  target_link_libraries(kncore PUBLIC mbedtls mbedx509 mbedcrypto)
endif()

target_include_directories(kncore PUBLIC streamer/include)
# System libraries every consumer of the self-test core needs on Windows:
#   d3d9  — kn_d3d (D7 probes, R25 milestone 0); d3d9.dll is OS-built-in on
#           Win7, and libd3d9.a is an import archive, so the F1 -static link
#           accepts it (same mechanism as the existing winmsvcrt imports).
#   winmm — clock.resolution (timeBeginPeriod).
if(WIN32)
  # ole32: kn_audio WASAPI engine (CoCreateInstance/CoTaskMemFree); IIDs are
  # instantiated locally via INITGUID in kn_audio.c, so no -luuid is needed.
  target_link_libraries(kncore INTERFACE d3d9 winmm ole32 ws2_32 secur32
                        crypt32)
else()
  target_link_libraries(kncore INTERFACE m)   # kn_audio_gen: sin()
endif()
# kn_aes.c is the ONLY file that includes tiny-AES's aes.h (see its PROVENANCE.md);
# upstream defaults apply everywhere (AES128, ECB+CBC+CTR) so struct AES_ctx has
# one consistent layout across translation units.
target_include_directories(kncore PRIVATE third_party/tiny-aes-c third_party/crypto-algorithms-sha1)

# --- build.json inputs (§7.8): git rev + dirty flag + the exact effective
# --- flags. Recorded at configure time, compiled into kn_diag (see
# --- kn_diag_render_build_json). "unknown" when not a git checkout.
find_package(Git QUIET)
if(Git_FOUND)
  execute_process(COMMAND "${Git_EXECUTABLE}" rev-parse --short HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE _kn_git_rc
    OUTPUT_VARIABLE KN_GIT_REV OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(_kn_git_rc EQUAL 0)
    execute_process(COMMAND "${Git_EXECUTABLE}" status --porcelain
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      OUTPUT_VARIABLE _kn_git_status OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_kn_git_status STREQUAL "")
      set(KN_GIT_DIRTY "false")
    else()
      set(KN_GIT_DIRTY "true")
    endif()
  endif()
endif()
if(NOT KN_GIT_REV)
  set(KN_GIT_REV "unknown")
  set(KN_GIT_DIRTY "unknown")
endif()
string(REPLACE ";" " " _kn_defs "${KN_TARGET_DEFS}")
string(REPLACE ";" " " _kn_opts "${KN_TARGET_OPTS}")
string(REPLACE "\"" "'" _kn_flags
  "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_RELEASE} ${_kn_opts} ${_kn_defs}")

# --- FFmpeg (D3: libavcodec h264, in-process — no ffmpeg.exe subprocesses,
# --- R11). Point KN_FFMPEG_PREFIX at the dev tree that contains include/
# --- and lib/ (e.g. .../ffmpeg-win7-dev/usr/local). Without it, kn_decode
# --- compiles as an honest stub and decode.synthetic reports "skipped".
set(KN_FFMPEG_PREFIX "" CACHE PATH
    "FFmpeg dev prefix (include/ + lib/); empty = build without FFmpeg")
set(KN_FFMPEG_LIBS "")
if(KN_FFMPEG_PREFIX)
  if(NOT EXISTS "${KN_FFMPEG_PREFIX}/include/libavcodec/avcodec.h")
    message(FATAL_ERROR
      "KN_FFMPEG_PREFIX='${KN_FFMPEG_PREFIX}' has no include/libavcodec/avcodec.h")
  endif()
  # The canonical F1 link is -static (D8). GNU ld with -static IGNORES the
  # .dll.a import libraries and searches only for static archives — a
  # shared-only FFmpeg install fails with a misleading "cannot find
  # -lavcodec / have you installed the static version" (seen live on the
  # target, T10 round 2). Catch it at configure time with the recipe:
  if(NOT EXISTS "${KN_FFMPEG_PREFIX}/lib/libavcodec.a")
    message(FATAL_ERROR
      "KN_FFMPEG_PREFIX='${KN_FFMPEG_PREFIX}' has no lib/libavcodec.a.\n"
      "The F1/D8 link uses -static, which ignores .dll.a import libraries.\n"
      "Rebuild FFmpeg with static libs (same tree, same toolchain):\n"
      "  make distclean\n"
      "  ./configure --arch=x86_64 --target-os=mingw32 --enable-gpl "
      "--enable-static --disable-shared --disable-autodetect\n"
      "  make -j2 && make install\n"
      "(the CLI ffmpeg.exe/ffprobe.exe stay available for G2 verification)")
  endif()
  message(STATUS "FFmpeg: enabled from ${KN_FFMPEG_PREFIX}")
  target_compile_definitions(kncore PUBLIC KN_HAVE_FFMPEG=1)
  target_include_directories(kncore PUBLIC "${KN_FFMPEG_PREFIX}/include")
  target_link_directories(kncore PUBLIC "${KN_FFMPEG_PREFIX}/lib")
  # Static-link closure, dependency order (GNU ld is single-pass):
  #   avcodec -> swresample (DSD/Opus/WavPack decoders call swr_*)
  #   swscale -> avutil; avutil -> bcrypt on Windows (av_random_seed uses
  #   BCryptGenRandom; bcrypt.dll exists since Vista, so Win7 is fine).
  # T10 round 3 proved this exact set is complete for the target build.
  set(KN_FFMPEG_LIBS avcodec swscale swresample avutil)
  if(WIN32)
    list(APPEND KN_FFMPEG_LIBS bcrypt)
  endif()
  string(REPLACE "\"" "'" _kn_ffmpeg "ffmpeg=${KN_FFMPEG_PREFIX}")
else()
  set(_kn_ffmpeg "ffmpeg=disabled")
endif()
string(APPEND _kn_ffmpeg ",tls=${KN_TLS_BACKEND}")

target_compile_definitions(kncore PRIVATE
  KN_DIAG_BUILD_GIT_REV="${KN_GIT_REV}"
  KN_DIAG_BUILD_GIT_DIRTY="${KN_GIT_DIRTY}"
  KN_DIAG_BUILD_FLAGS="${_kn_flags}"
  KN_DIAG_BUILD_EXTRA="${_kn_ffmpeg}")

enable_testing()
foreach(t knlog knring knfec knsrtp kndiag knselftest kngs knaudio knrx knsock knws knrtsp kncm kncert kndtls knsctp kninput)
  add_executable(test_${t} streamer/tests/test_${t}.c)
  target_link_libraries(test_${t} PRIVATE kncore)
  # tests include the generated KAT vector headers, which live in src/
  # next to their consumers (knfec_tables.h precedent; §7.9 self-tests run
  # the same vectors from the production binary)
  target_include_directories(test_${t} PRIVATE streamer/src)
  if(NOT WIN32)
    target_link_libraries(test_${t} PRIVATE pthread)
  endif()
  add_test(NAME test_${t} COMMAND test_${t})
endforeach()

# timeBeginPeriod/timeEndPeriod (clock.resolution self-test, §7.9)
if(WIN32)
  target_link_libraries(test_knselftest PRIVATE winmm)
endif()
if(KN_FFMPEG_LIBS)
  target_link_libraries(test_knselftest PRIVATE ${KN_FFMPEG_LIBS})
endif()

# Round XXXI (R3-B): kn_dtls's real battery needs the mbedTLS DTLS client —
# the SAME backend define kncore gets, so the mbedtls build compiles and
# runs the real handshake suite while schannel builds get the honest stub.
# kn_dtls is portable (unlike kn_net_mbedtls's Win32-only body), so this
# leg executes for real on the Linux host too.
if(KN_TLS_BACKEND STREQUAL "mbedtls")
  target_compile_definitions(test_kndtls PRIVATE KN_TLS_BACKEND_MBEDTLS=1)
endif()

# Round XXXIII (R3-C): the vendored usrsctp static library — same model
# as mbedTLS (pinned tarball fetched/verified by T10, gitignored tree).
# The real kn_sctp body compiles when the tree exists, on BOTH platforms
# (the round-XXII rule); without it every build gets the honest stub.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/usrsctp/usrsctplib/CMakeLists.txt")
  set(sctp_debug 0 CACHE BOOL "" FORCE)
  set(sctp_werror 0 CACHE BOOL "" FORCE)
  set(sctp_invariants 0 CACHE BOOL "" FORCE)
  set(sctp_build_programs 0 CACHE BOOL "" FORCE)
  set(sctp_build_shared_lib 0 CACHE BOOL "" FORCE)
  set(sctp_build_fuzzer 0 CACHE BOOL "" FORCE)
  set(sctp_inet 1 CACHE BOOL "" FORCE)
  set(sctp_inet6 0 CACHE BOOL "" FORCE)
  add_subdirectory(third_party/usrsctp EXCLUDE_FROM_ALL)
  # Vendored code: ALL warnings off for the usrsctp target only (the
  # project's zero-warning gate is for OUR code; the mbedtls precedent).
  target_compile_options(usrsctp PRIVATE -w)
  target_compile_definitions(kncore PRIVATE KN_SCTP_BACKEND_USRSCTP=1)
  target_link_libraries(kncore PUBLIC usrsctp)
  # NOTE: do NOT add target_link_libraries(usrsctp ...) here — usrsctp's
  # own CMakeLists uses the PLAIN signature (it links ws2_32+iphlpapi
  # itself on WIN32, usrsctplib/CMakeLists.txt:195-198) and CMake
  # forbids mixing plain with keyword signatures on one target. The
  # target T10 configure caught this (round XXXIII hotfix) — the POSIX
  # sandbox never executes the WIN32 branch.
  target_compile_definitions(test_knsctp PRIVATE KN_SCTP_BACKEND_USRSCTP=1)
  message(STATUS "kn_sctp: usrsctp backend ENABLED (third_party/usrsctp)")
else()
  message(STATUS "kn_sctp: usrsctp tree absent — honest stub build")
endif()

# knvstd — the streamer executable. Pre-stage-1 skeleton: --selftest,
# --diagnostics, --version, --list-selftests (see knvstd_main.c).
add_executable(knvstd streamer/src/knvstd_main.c)
target_link_libraries(knvstd PRIVATE kncore)
if(WIN32)
  target_link_libraries(knvstd PRIVATE winmm)
else()
  target_link_libraries(knvstd PRIVATE pthread)
endif()
if(KN_FFMPEG_LIBS)
  target_link_libraries(knvstd PRIVATE ${KN_FFMPEG_LIBS})
  if(WIN32)
    # ship the FFmpeg DLLs next to knvstd.exe (shared build: avcodec-63.dll,
    # avutil-61.dll, swscale-10.dll, swresample-7.dll — avcodec pulls in
    # swresample transitively)
    file(GLOB _kn_ff_dlls "${KN_FFMPEG_PREFIX}/bin/*.dll")
    if(_kn_ff_dlls)
      add_custom_command(TARGET knvstd POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${_kn_ff_dlls} "$<TARGET_FILE_DIR:knvstd>"
        COMMENT "Copying FFmpeg DLLs next to knvstd.exe")
    else()
      message(STATUS "FFmpeg: no bin/*.dll — static build, nothing to copy")
    endif()
  endif()
endif()
add_test(NAME knvstd_selftest_fec COMMAND knvstd --selftest=fec.vectors --log-dir=knvstd_testlogs)
add_test(NAME knvstd_selftest_list COMMAND knvstd --list-selftests)
