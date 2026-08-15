find_package(SDL3 CONFIG REQUIRED)

add_executable(semi_player_sdl
    main.cpp
    video_presenter.cpp
)
target_compile_features(semi_player_sdl PRIVATE cxx_std_23)
target_link_libraries(semi_player_sdl PRIVATE semi_player SDL3::SDL3)

set_target_properties(semi_player_sdl PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/bin"
    INSTALL_RPATH "@loader_path;$ORIGIN"
)

if(WIN32)
    add_custom_command(TARGET semi_player_sdl POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:semi_player>
            $<TARGET_FILE_DIR:semi_player_sdl>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL3::SDL3>
            $<TARGET_FILE_DIR:semi_player_sdl>
        COMMENT "Copy runtime libraries next to the SDL player host"
    )
endif()

install(TARGETS semi_player_sdl
    RUNTIME_DEPENDENCY_SET semi_player_runtime_dependencies
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
