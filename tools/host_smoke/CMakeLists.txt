# 模拟宿主：链接 C ABI 共享库，不碰内部头。
add_executable(semi_player_host main.cpp)
target_compile_features(semi_player_host PRIVATE cxx_std_23)
target_link_libraries(semi_player_host PRIVATE semi_player)

file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/tests/resources/sample.mp4" SEMI_PLAYER_SMOKE_MEDIA_PATH)
target_compile_definitions(semi_player_host PRIVATE
    "SEMI_PLAYER_SMOKE_MEDIA_PATH=\"${SEMI_PLAYER_SMOKE_MEDIA_PATH}\"")

# 运行时从同目录加载 DLL（产物都在 ${CMAKE_BINARY_DIR}/bin）。
set_target_properties(semi_player_host PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/bin"
    INSTALL_RPATH "@loader_path;$ORIGIN"
)

if(WIN32)
    # 保证从 build/bin 直接运行时能找到 semi_player.dll
    add_custom_command(TARGET semi_player_host POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:semi_player>
            $<TARGET_FILE_DIR:semi_player_host>
        COMMENT "Copy semi_player.dll next to host smoke binary"
    )
endif()
