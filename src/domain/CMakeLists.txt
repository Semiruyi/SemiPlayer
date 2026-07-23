# domain 层：业务相关模块。
# 每个模块一个子目录，各自 CMakeLists.txt 把 .cpp 追加到 semi_player_core。
add_subdirectory(generation)
add_subdirectory(demuxer)
# add_subdirectory(<其他业务模块>)
