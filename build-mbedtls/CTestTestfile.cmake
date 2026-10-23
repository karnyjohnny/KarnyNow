# CMake generated Testfile for 
# Source directory: /workspaces/KarnyNow
# Build directory: /workspaces/KarnyNow/build-mbedtls
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(test_knlog "/workspaces/KarnyNow/build-mbedtls/test_knlog")
set_tests_properties(test_knlog PROPERTIES  _BACKTRACE_TRIPLES "/workspaces/KarnyNow/CMakeLists.txt;175;add_test;/workspaces/KarnyNow/CMakeLists.txt;0;")
add_test(test_knring "/workspaces/KarnyNow/build-mbedtls/test_knring")
set_tests_properties(test_knring PROPERTIES  _BACKTRACE_TRIPLES "/workspaces/KarnyNow/CMakeLists.txt;175;add_test;/workspaces/KarnyNow/CMakeLists.txt;0;")
add_test(test_knfec "/workspaces/KarnyNow/build-mbedtls/test_knfec")
set_tests_properties(test_knfec PROPERTIES  _BACKTRACE_TRIPLES "/workspaces/KarnyNow/CMakeLists.txt;175;add_test;/workspaces/KarnyNow/CMakeLists.txt;0;")
add_test(test_knsrtp "/workspaces/KarnyNow/build-mbedtls/test_knsrtp")
set_tests_properties(test_knsrtp PROPERTIES  _BACKTRACE_TRIPLES "/workspaces/KarnyNow/CMakeLists.txt;175;add_test;/workspaces/KarnyNow/CMakeLists.txt;0;")
add_test(test_kndiag "/workspaces/KarnyNow/build-mbedtls/test_kndiag")
set_tests_properties(test_kndiag PROPERTIES  _BACKTRACE_TRIPLES "/workspaces/KarnyNow/CMakeLists.txt;175;add_test;/workspaces/KarnyNow/CMakeLists.txt;0;")
add_test(test_knselftest "/workspaces/KarnyNow/build-mbedtls/test_knselftest")
set_tests_properties(test_knselftest PROPERTIES  _BACKTRACE_TRIPLES "/workspaces/KarnyNow/CMakeLists.txt;175;add_test;/workspaces/KarnyNow/CMakeLists.txt;0;")
add_test(test_kngs "/workspaces/KarnyNow/build-mbedtls/test_kngs")
set_tests_properties(test_kngs PROPERTIES  _BACKTRACE_TRIPLES "/workspaces/KarnyNow/CMakeLists.txt;175;add_test;/workspaces/KarnyNow/CMakeLists.txt;0;")
add_test(test_knaudio "/workspaces/KarnyNow/build-mbedtls/test_knaudio")
set_tests_properties(test_knaudio PROPERTIES  _BACKTRACE_TRIPLES "/workspaces/KarnyNow/CMakeLists.txt;175;add_test;/workspaces/KarnyNow/CMakeLists.txt;0;")
add_test(knvstd_selftest_fec "/workspaces/KarnyNow/build-mbedtls/knvstd" "--selftest=fec.vectors" "--log-dir=knvstd_testlogs")
set_tests_properties(knvstd_selftest_fec PROPERTIES  _BACKTRACE_TRIPLES "/workspaces/KarnyNow/CMakeLists.txt;212;add_test;/workspaces/KarnyNow/CMakeLists.txt;0;")
add_test(knvstd_selftest_list "/workspaces/KarnyNow/build-mbedtls/knvstd" "--list-selftests")
set_tests_properties(knvstd_selftest_list PROPERTIES  _BACKTRACE_TRIPLES "/workspaces/KarnyNow/CMakeLists.txt;213;add_test;/workspaces/KarnyNow/CMakeLists.txt;0;")
