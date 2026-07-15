cmake_minimum_required(VERSION 3.21)

project(SemiPlayer
    VERSION 0.1.0
    LANGUAGES CXX
)

# ---- C++ standard ----
# C++20: std::atomic<std::shared_ptr> (AudioClock / decoder target_pts 快照)、
#         std::any + std::type_index (Notifier 异构槽) 都需要。
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- Default build type ----
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# ---- Output layout ----
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---- Options ----
option(SEMI_BUILD_DLL    "Build the C ABI shared library for Dart FFI" ON)
option(SEMI_BUILD_TESTS  "Build unit tests (links the static core, not the DLL)" OFF)

# ---- 符号可见性（全局默认）----
# 非 Windows(ELF/Mach-O)：默认隐藏，只有头里 SEMI_API(=visibility("default")) 才导出，
# 与 Windows 的 dllexport 行为对齐。STATIC 库不受影响——可见性只管 SHARED 的动态导出，
# 静态链接/exe 仍能见到全部符号（故测试链 STATIC core 不受此影响）。
if(NOT WIN32)
    set(CMAKE_CXX_VISIBILITY_PRESET hidden)
    set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
endif()

# ---- Warnings (GCC/Clang) ----
add_compile_options(
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-Wall>
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-Wextra>
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-Wpedantic>
)

add_subdirectory(src)

if(SEMI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
