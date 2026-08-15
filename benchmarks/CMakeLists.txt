add_executable(semi_player_benchmark
    src/main.cpp
)

target_compile_features(semi_player_benchmark PRIVATE cxx_std_23)
target_include_directories(semi_player_benchmark PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(semi_player_benchmark PRIVATE semi_player)

if(MINGW)
    target_link_libraries(semi_player_benchmark PRIVATE psapi)
endif()

set_target_properties(semi_player_benchmark PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
)
