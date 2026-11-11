target_sources(semi_player_tests PRIVATE
    ffmpeg_video_decoder_backend_test.cpp
    ffmpeg_hardware_decode_test.cpp
)
if(WIN32)
    target_sources(semi_player_tests PRIVATE ffmpeg_d3d11_video_frame_buffer_test.cpp)
endif()
