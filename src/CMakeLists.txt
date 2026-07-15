# SemiPlayer 引擎核心 + C ABI 导出。
#
# semi_player_core (STATIC)：聚合所有模块源。目标在此声明（空源），各模块经
#                            add_subdirectory 链，在自己的 CMakeLists.txt 里用
#                            target_sources(semi_player_core PRIVATE ...) 追加 .cpp。
#                            仍是一个目标——不爆 25 个子库（架构文档反对过度设计）。
# semi_player      (SHARED)：只 api_export.cpp，链 core，导出 C ABI（可由 SEMI_BUILD_DLL 关掉）。

# ---- 引擎核心（STATIC）：目标在此声明，源由各子目录追加 ----
add_library(semi_player_core STATIC)
target_compile_features(semi_player_core PUBLIC cxx_std_20)
target_include_directories(semi_player_core
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include      # C ABI 公共头给消费者
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}      # 内部头根 src/：使 include 形如 domain/.../x.hpp 生效
)

# 各层子目录：domain（业务相关）、infrastructure（与业务无关的机制）。
# 每层 CMakeLists 管理该层各模块子目录；模块 CMakeLists 把自己的 .cpp 追加到 core。
add_subdirectory(domain)
# add_subdirectory(infrastructure)   # 第一个机制模块（Notifier）落地时开启

# ---- C ABI 共享库（给 Flutter 经 dart:ffi 加载）----
# 纯逻辑/测试开发阶段可 -DSEMI_BUILD_DLL=OFF 跳过，只编 core + tests，不碰 FFI/导出。
if(SEMI_BUILD_DLL)
    add_library(semi_player SHARED
        api_export.cpp
    )
    target_compile_features(semi_player PUBLIC cxx_std_20)
    target_include_directories(semi_player
        PUBLIC
            ${CMAKE_SOURCE_DIR}/include
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(semi_player PRIVATE semi_player_core)
    # 编 DLL 时定义此宏，使头中 SEMI_API 展开为 dllexport（core/tests 不定义）。
    target_compile_definitions(semi_player PRIVATE SEMI_PLAYER_DLL_EXPORT=1)

    # Windows：去掉 MinGW 默认 "lib" 前缀，产物 semi_player.dll；Unix 保留 lib 前缀。
    set_target_properties(semi_player PROPERTIES OUTPUT_NAME semi_player)
    if(WIN32)
        set_target_properties(semi_player PROPERTIES PREFIX "")
    endif()
endif()


