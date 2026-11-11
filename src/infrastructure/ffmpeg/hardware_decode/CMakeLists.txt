target_sources(semi_player_core PRIVATE platform_hardware_decode_provider.cpp)
if(WIN32)
    target_sources(semi_player_core PRIVATE d3d11_hardware_decode.cpp)
endif()
