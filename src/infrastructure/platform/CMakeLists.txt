# platform 模块：封装进程/系统相关查询，不向调用方暴露平台头。
target_sources(semi_player_core PRIVATE
    process_path.cpp
)
