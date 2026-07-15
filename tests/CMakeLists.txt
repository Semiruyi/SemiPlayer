# 测试：链 STATIC 的 semi_player_core（全 C++ 符号可见，不碰导出的 DLL）。
# 测试 exe 在此声明（空源），各模块测试经 add_subdirectory 链，在自己的 CMakeLists 里
# 用 target_sources(semi_player_tests PRIVATE ...) 追加 _test.cpp。
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
# MSVC CRT 一致性（MinGW 下无害）。
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(semi_player_tests)
target_compile_features(semi_player_tests PRIVATE cxx_std_20)
target_link_libraries(semi_player_tests PRIVATE semi_player_core GTest::gtest_main)
# 能 include 内部头（domain/…/x.hpp）。
target_include_directories(semi_player_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)

# 各层测试子目录，与 src 同构。
add_subdirectory(domain)
# add_subdirectory(infrastructure)

include(GoogleTest)
gtest_discover_tests(semi_player_tests)
