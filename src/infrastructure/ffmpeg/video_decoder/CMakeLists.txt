target_sources(semi_player_core PRIVATE
    ffmpeg_video_frame_buffer.cpp
    ffmpeg_video_decoder_backend.cpp
)
if(WIN32)
    target_sources(semi_player_core PRIVATE ffmpeg_d3d11_video_frame_buffer.cpp)
endif()
