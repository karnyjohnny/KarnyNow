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
  third_party/tiny-aes-c/aes.c
  third_party/crypto-algorithms-sha1/sha1.c
)
target_include_directories(kncore PUBLIC streamer/include)
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
  message(STATUS "FFmpeg: enabled from ${KN_FFMPEG_PREFIX}")
  target_compile_definitions(kncore PUBLIC KN_HAVE_FFMPEG=1)
  target_include_directories(kncore PUBLIC "${KN_FFMPEG_PREFIX}/include")
  target_link_directories(kncore PUBLIC "${KN_FFMPEG_PREFIX}/lib")
  set(KN_FFMPEG_LIBS avcodec swscale avutil)
  string(REPLACE "\"" "'" _kn_ffmpeg "ffmpeg=${KN_FFMPEG_PREFIX}")
else()
  set(_kn_ffmpeg "ffmpeg=disabled")
endif()

target_compile_definitions(kncore PRIVATE
  KN_DIAG_BUILD_GIT_REV="${KN_GIT_REV}"
  KN_DIAG_BUILD_GIT_DIRTY="${KN_GIT_DIRTY}"
  KN_DIAG_BUILD_FLAGS="${_kn_flags}"
  KN_DIAG_BUILD_EXTRA="${_kn_ffmpeg}")

enable_testing()
foreach(t knlog knring knfec knsrtp kndiag knselftest)
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
      message(WARNING "KN_FFMPEG_PREFIX has no bin/*.dll — put the FFmpeg "
                      "DLLs next to knvstd.exe manually")
    endif()
  endif()
endif()
add_test(NAME knvstd_selftest_fec COMMAND knvstd --selftest=fec.vectors --log-dir=knvstd_testlogs)
add_test(NAME knvstd_selftest_list COMMAND knvstd --list-selftests)
