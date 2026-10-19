target_sources(semi_player_core PRIVATE gpu_device_manager.cpp)

if(WIN32)
    target_sources(semi_player_core PRIVATE d3d11/d3d11_device.cpp)
    target_link_libraries(semi_player_core PRIVATE d3d11)
endif()
