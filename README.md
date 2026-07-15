# SemiPlayer

C++ 跨平台播放器内核，导出 C ABI 供 Flutter(Dart) 经 dart:ffi 调用。
FFmpeg 解封装/解码、miniaudio 播音频，单例全局 + 命令队列/句柄控制模型。
架构见 `docs/` 下设计文档（architecture / lifecycle / 各模块）。

## 构建（MinGW-w64）

```sh
cmake --preset mingw-default
cmake --build --preset mingw-default
```

## 测试构建&运行
```sh
cmake --preset mingw-tests
cmake --build --preset mingw-tests
ctest --test-dir build-tests
```

产物：`build/bin/semi_player.dll`（C ABI 共享库，给 Flutter 加载）。
