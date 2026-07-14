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
option(SEMI_BUILD_TESTS "Build unit tests" OFF)

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
