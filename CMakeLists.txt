cmake_minimum_required(VERSION 3.21)

# CMake 4.4+: normalize runtime dependency paths before applying filters, so
# Windows system DLLs can be excluded reliably from portable installs.
if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()

project(SemiPlayer
    VERSION 0.1.0
    LANGUAGES CXX
)

include(GNUInstallDirs)

# ---- C++ standard ----
# C++20: std::atomic<std::shared_ptr> (AudioClock / decoder target_pts 快照)、
#         std::any + std::type_index (Notifier 异构槽) 都需要。
set(CMAKE_CXX_STANDARD 23)
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
option(SEMI_BUILD_SDL_PLAYER "Build the SDL3 playback example host" OFF)
option(SEMI_USE_NULL_AUDIO_OUTPUT
       "Use the headless null audio backend in the composition root"
       OFF)
option(SEMI_INSTALL_RUNTIME_DEPENDENCIES
       "Install third-party runtime DLLs for a portable Windows layout"
       OFF)

if(SEMI_INSTALL_RUNTIME_DEPENDENCIES AND
   (NOT WIN32 OR NOT SEMI_BUILD_DLL OR NOT SEMI_BUILD_SDL_PLAYER))
    message(FATAL_ERROR
        "SEMI_INSTALL_RUNTIME_DEPENDENCIES requires a Windows build with "
        "SEMI_BUILD_DLL=ON and SEMI_BUILD_SDL_PLAYER=ON")
endif()

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

# ---- Third-party dependencies ----
find_package(spdlog CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec
    libavformat
    libavutil
    libswresample
    libswscale
)

add_library(semi_player_ffmpeg INTERFACE)
target_link_libraries(semi_player_ffmpeg INTERFACE PkgConfig::FFMPEG)

add_subdirectory(src)
target_link_libraries(semi_player_core PRIVATE semi_player_ffmpeg)
if(SEMI_USE_NULL_AUDIO_OUTPUT)
    target_compile_definitions(semi_player_core PRIVATE SEMI_USE_NULL_AUDIO_OUTPUT=1)
endif()

if(SEMI_BUILD_SDL_PLAYER)
    if(NOT SEMI_BUILD_DLL)
        message(FATAL_ERROR "SEMI_BUILD_SDL_PLAYER requires SEMI_BUILD_DLL=ON")
    endif()
    add_subdirectory(examples/sdl_player)
endif()

if(SEMI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# A portable Windows install keeps every non-system runtime dependency beside
# the executable. The compiler directory provides the UCRT64 runtime search
# location; dependencies are resolved recursively by CMake at install time.
if(SEMI_INSTALL_RUNTIME_DEPENDENCIES)
    get_filename_component(SEMI_TOOLCHAIN_RUNTIME_DIRECTORY
        "${CMAKE_CXX_COMPILER}" DIRECTORY)
    install(RUNTIME_DEPENDENCY_SET semi_player_runtime_dependencies
        DIRECTORIES "${SEMI_TOOLCHAIN_RUNTIME_DIRECTORY}"
        PRE_EXCLUDE_REGEXES
            "^[Aa][Pp][Ii]-[Mm][Ss]-[Ww][Ii][Nn]-.*"
            "^[Ee][Xx][Tt]-[Mm][Ss]-[Ww][Ii][Nn]-.*"
        POST_EXCLUDE_REGEXES
            "^[A-Za-z]:[/\\\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\].*"
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
                COMPONENT Runtime
    )
endif()

# ---- Public headers ----
install(
    DIRECTORY include/semi_player
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    COMPONENT Development
    FILES_MATCHING PATTERN "*.h"
)

install(FILES
    LICENSE
    NOTICE
    THIRD_PARTY_NOTICES.md
    DESTINATION .
    COMPONENT Runtime
)

if(SEMI_INSTALL_RUNTIME_DEPENDENCIES)
    get_filename_component(SEMI_TOOLCHAIN_ROOT
        "${SEMI_TOOLCHAIN_RUNTIME_DIRECTORY}" DIRECTORY)
    get_filename_component(SEMI_MSYS_ROOT "${SEMI_TOOLCHAIN_ROOT}" DIRECTORY)
    get_filename_component(SEMI_TOOLCHAIN_NAME "${SEMI_TOOLCHAIN_ROOT}" NAME)
    set(SEMI_TOOLCHAIN_PREFIX "/${SEMI_TOOLCHAIN_NAME}")
    set(SEMI_PACMAN_EXECUTABLE "${SEMI_MSYS_ROOT}/usr/bin/pacman.exe")
    find_program(SEMI_POWERSHELL_EXECUTABLE powershell REQUIRED)
    configure_file(
        cmake/InstallThirdPartyLicenses.cmake.in
        ${CMAKE_BINARY_DIR}/InstallThirdPartyLicenses.cmake
        @ONLY
    )
    install(SCRIPT ${CMAKE_BINARY_DIR}/InstallThirdPartyLicenses.cmake
        COMPONENT Runtime)

    set(CPACK_GENERATOR ZIP)
    set(CPACK_PACKAGE_NAME ${PROJECT_NAME})
    set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
    set(CPACK_PACKAGE_FILE_NAME
        "${PROJECT_NAME}-${PROJECT_VERSION}-windows-x64-portable")
    set(CPACK_PACKAGE_DIRECTORY "${PROJECT_SOURCE_DIR}/out/packages")
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
    set(CPACK_INSTALL_CMAKE_PROJECTS
        "${CMAKE_BINARY_DIR};${PROJECT_NAME};Runtime;/")
    include(CPack)
endif()
