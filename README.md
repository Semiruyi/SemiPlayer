# SemiPlayer

C++ 跨平台播放器内核，导出 C ABI 供 Flutter(Dart) 经 dart:ffi 调用。
FFmpeg 解封装/解码、miniaudio 播音频，单例全局 + 命令队列/句柄控制模型。
架构见 `docs/` 下设计文档（architecture / lifecycle / 各模块）。
错误约定见 `docs/error_convention.md`，状态码见 `include/semi_player/status.h`。

## Windows 构建（MSYS2 UCRT64 + Ninja）

### 1. 安装 MSYS2

先安装 MSYS2，然后从开始菜单打开 `MSYS2 UCRT64` 终端。

如果你更习惯命令行，也可以用 `winget`：

```powershell
winget install -e --id MSYS2.MSYS2
```

### 2. 更新系统并安装工具链

打开MSYS2窗口，项目路径替换为自己的项目路径

```sh
C:\msys64\msys2_shell.cmd -ucrt64 -where C:\y-s\project\SemiPlayer
```

在 `MSYS2 UCRT64` 里执行：

```sh
pacman -Syu
```

如果提示关闭窗口，重新打开 `MSYS2 UCRT64` 后再执行一次：

```sh
pacman -Syu
```

安装构建依赖：

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-spdlog mingw-w64-ucrt-x86_64-gtest mingw-w64-ucrt-x86_64-ffmpeg git
```

如果下载慢，可先设置你自己的代理再执行上面的命令：

```sh
export http_proxy=http://127.0.0.1:7890
export https_proxy=http://127.0.0.1:7890
export all_proxy=http://127.0.0.1:7890
```

### 3. 配置并构建

在项目根目录执行：

```sh
cmake --preset windows-all
cmake --build --preset windows-all
```

`windows-all` 会一次性构建：

- `semi_player_core` 静态库
- `semi_player.dll` C ABI 动态库
- `semi_player_tests` 单元测试
- `semi_player_host.exe` C ABI 冒烟宿主

产物：

- `build-windows/lib/libsemi_player_core.a`
- `build-windows/bin/semi_player.dll`
- `build-windows/bin/semi_player_tests.exe`
- `build-windows/bin/semi_player_host.exe`

### 4. 运行测试和宿主冒烟

运行单元测试：

```sh
ctest --test-dir build-windows
```

运行 C ABI 冒烟宿主：

```sh
./build-windows/bin/semi_player_host
```

## 模拟宿主（C ABI 冒烟）

在开启 `SEMI_BUILD_DLL` 的 preset 下会编 `semi_player_host`，只调公开 C API：

```sh
cmake --preset windows-all
cmake --build --preset windows-all
./build-windows/bin/semi_player_host
```
