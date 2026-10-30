include(FetchContent)

FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    SYSTEM)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

add_executable(axon_unit_tests
    unit/bytes_test.cpp
    unit/crc32_test.cpp
    unit/dataset_test.cpp
    unit/fd_test.cpp
    unit/file_test.cpp
    unit/frame_test.cpp
    unit/iov_test.cpp
    unit/messages_test.cpp
    unit/socket_test.cpp
    unit/version_test.cpp)
target_link_libraries(axon_unit_tests PRIVATE axon_core GTest::gtest_main)
# White-box tests of internal helpers include private headers from src/.
target_include_directories(axon_unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
# A hang (blocked I/O, endless loop) fails the test instead of stalling the run.
gtest_discover_tests(axon_unit_tests PROPERTIES TIMEOUT 30)
