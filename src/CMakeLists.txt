# SemiPlayer engine + C ABI export (minimal skeleton).
#
# 产物：semi_player.dll / libsemi_player.so / libsemi_player.dylib —— 给 Flutter 经 dart:ffi 加载。
# 只导出 C 函数（SEMI_API），C++ 符号不外泄，保证 ABI 稳定。

add_library(semi_player SHARED
    player.cpp
    api_export.cpp
)

target_compile_features(semi_player PUBLIC cxx_std_20)

# PRIVATE: 仅本目标内部头路径；PUBLIC: C ABI 头给消费者（Dart 侧编译 FFI 绑定时）。
target_include_directories(semi_player
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# 编译本目标时定义此宏，使头中 SEMI_API 展开为 dllexport（否则消费方见 dllimport）。
target_compile_definitions(semi_player PRIVATE SEMI_PLAYER_DLL_EXPORT=1)

# 产物名
# Windows：FFI 按名加载 semi_player.dll，去掉 MinGW 默认的 "lib" 前缀。
# Unix/macOS：保留默认 "lib" 前缀，产出 libsemi_player.so / .dylib（平台惯例，FFI 同样按此名加载）。
set_target_properties(semi_player PROPERTIES OUTPUT_NAME semi_player)
if(WIN32)
    set_target_properties(semi_player PROPERTIES PREFIX "")
endif()

# ---- 符号导出（编译宏 / 链接器层面）----
# Unix(ELF/Mach-O) 默认导出所有符号；要像 Windows 的 dllexport 那样"只导出 SEMI_API 标记的符号"，
# 需把默认可见性设为 hidden：默认隐藏，只有头里 SEMI_API(=visibility("default")) 才导出。
# Windows(PE) 是相反模型（默认不导出，dllexport 才导出），实测已只导出 12 个 C ABI，故不在 Windows 设置。
if(NOT WIN32)
    set_target_properties(semi_player PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
endif()

# ---- 可选：Unix 共享库 soname 版本（发布带版本号时启用，FFI 装发布可按需）----
# if(NOT WIN32)
#     set_target_properties(semi_player PROPERTIES
#         VERSION ${PROJECT_VERSION}
#         SOVERSION ${PROJECT_VERSION_MAJOR}
#     )
# endif()

