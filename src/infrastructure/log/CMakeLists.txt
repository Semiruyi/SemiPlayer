# log 模块：内部异步日志封装。
target_sources(semi_player_core
    PRIVATE
        log.cpp
)

target_link_libraries(semi_player_core
    PUBLIC
        spdlog::spdlog
)
