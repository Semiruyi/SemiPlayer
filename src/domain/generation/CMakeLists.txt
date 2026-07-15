# Generation 模块（见本目录 generation.hpp 注释）。
# 把本模块 .cpp 追加到引擎核心目标；用 CMAKE_CURRENT_LIST_DIR 保证路径与调用处无关。
target_sources(semi_player_core PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/generation.cpp
)
