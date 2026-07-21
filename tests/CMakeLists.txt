# 测试：链 STATIC 的 semi_player_core（全 C++ 符号可见，不碰导出的 DLL）。
# 测试 exe 在此声明（空源），各模块测试经 add_subdirectory 链，在自己的 CMakeLists 里
# 用 target_sources(semi_player_tests PRIVATE ...) 追加 _test.cpp。
find_package(GTest CONFIG REQUIRED)

add_executable(semi_player_tests)
target_compile_features(semi_player_tests PRIVATE cxx_std_20)
target_link_libraries(semi_player_tests PRIVATE semi_player_core GTest::gtest_main)
# 能 include 内部头（domain/…/x.hpp）。
target_include_directories(semi_player_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)

# 各层测试子目录，与 src 同构。
add_subdirectory(application)
add_subdirectory(domain)
add_subdirectory(infrastructure)

include(GoogleTest)
gtest_discover_tests(semi_player_tests)
