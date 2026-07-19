# SemiPlayer

C++ 跨平台播放器内核，导出 C ABI 供 Flutter(Dart) 经 dart:ffi 调用。
FFmpeg 解封装/解码、miniaudio 播音频，单例全局 + 命令队列/句柄控制模型。
架构见 `docs/` 下设计文档（architecture / lifecycle / 各模块）。
错误约定见 `docs/error_convention.md`，状态码见 `include/semi_player/status.h`。

## 构建（MinGW-w64）

```sh
cmake --preset mingw-default
cmake --build --preset mingw-default
```

## 构建（macOS），会拉取相关依赖，注意代理配置

```sh
cmake --preset macos-default
cmake --build --preset macos-default
```

## 测试构建&运行
```sh
cmake --preset mingw-tests
cmake --build --preset mingw-tests
ctest --test-dir build-tests
```

## 测试构建&运行（macOS）

```sh
cmake --preset macos-tests
cmake --build --preset macos-tests
ctest --test-dir build-tests-macos
```

产物：
- MinGW-w64: `build/bin/semi_player.dll`
- macOS: `build-macos/bin/libsemi_player.dylib`

以上产物均为 C ABI 共享库，给 Flutter 加载。

## 模拟宿主（C ABI 冒烟）

在开启 `SEMI_BUILD_DLL` 的 preset 下会编 `semi_player_host`，只调公开 C API：

```sh
cmake --preset macos-default
cmake --build --preset macos-default
./build-macos/bin/semi_player_host
```