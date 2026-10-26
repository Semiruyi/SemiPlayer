target_sources(semi_player_core PRIVATE gpu_device_manager.cpp)
target_sources(semi_player_core PRIVATE readback/d3d11_video_readback.cpp)
target_sources(semi_player_core PRIVATE readback/detail/owned_cpu_video_frame_buffer.cpp)

if(WIN32)
    target_sources(semi_player_core PRIVATE d3d11/d3d11_device.cpp)
    target_link_libraries(semi_player_core PRIVATE d3d11)
endif()
