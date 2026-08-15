# SemiPlayer

[![Windows 持续集成](https://github.com/Semiruyi/SemiPlayer/actions/workflows/windows-ci.yml/badge.svg)](https://github.com/Semiruyi/SemiPlayer/actions/workflows/windows-ci.yml)

C++ 跨平台播放器内核，导出 C ABI 供上层宿主调用。
FFmpeg 解封装/解码、miniaudio 播音频，单例全局 + 命令队列/句柄控制模型。
架构、生命周期及各模块设计见 `docs/` 下的设计文档。
错误约定见 `docs/error_convention.md`，状态码见 `include/semi_player/status.h`。

## 许可证

SemiPlayer 采用 GNU 通用公共许可证第 3 版或更高版本（`GPL-3.0-or-later`），
详见 [`LICENSE`](LICENSE)。二进制发布包包含自动生成的
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)，以及对应 MSYS2 运行时包
提供的许可证文件。

## Windows 构建（MSYS2 UCRT64 + Ninja）

### 1. 安装 MSYS2

先安装 MSYS2，然后从开始菜单打开 `MSYS2 UCRT64` 终端。

如果你更习惯命令行，也可以用 `winget`：

```powershell
winget install -e --id MSYS2.MSYS2
```

### 2. 更新系统并安装工具链

打开 MSYS2 窗口，将示例中的项目路径替换为自己的项目路径：

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
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-spdlog mingw-w64-ucrt-x86_64-gtest mingw-w64-ucrt-x86_64-ffmpeg mingw-w64-ucrt-x86_64-miniaudio mingw-w64-ucrt-x86_64-sdl3 git
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
- `semi_player_abi_tests` C ABI 动态库边界测试
- `semi_player_sdl.exe` SDL3 播放示例宿主

产物：

- `build-windows/lib/libsemi_player_core.a`
- `build-windows/bin/semi_player.dll`
- `build-windows/bin/semi_player_tests.exe`
- `build-windows/bin/semi_player_abi_tests.exe`
- `build-windows/bin/semi_player_sdl.exe`

### 4. 运行测试和播放宿主

运行单元测试：

```sh
ctest --test-dir build-windows
```

运行 SDL3 播放宿主：

```sh
./build-windows/bin/semi_player_sdl
./build-windows/bin/semi_player_sdl path/to/media.mp4
```

不传媒体路径时会播放随示例安装的合成 `sample.mp4`；也可以把媒体文件拖到
`semi_player_sdl.exe` 上打开。按空格播放/暂停，左右方向键前后跳转 5 秒，F11
切换全屏，Esc 退出。详细设计与线程边界见
[`examples/sdl_player/README.md`](examples/sdl_player/README.md)。

### 发布版本构建与安装目录

`windows-release` 预设只构建发布版本的 C ABI 动态库和 SDL3 示例宿主，
不构建测试程序：

```sh
cmake --preset windows-release
cmake --build --preset windows-release
cmake --install build-windows-release --prefix out/SemiPlayer
```

安装完成后的自产物布局如下：

```text
out/SemiPlayer/
├── bin/
│   ├── semi_player.dll
│   └── semi_player_sdl.exe
├── include/semi_player/
│   ├── semi_player.h
│   └── status.h
└── lib/
    └── libsemi_player.dll.a
```

这个安装目录目前只包含 SemiPlayer 自身产物；SDL3、FFmpeg、MinGW 运行库等
第三方依赖由独立的 `windows-portable` 预设在安装时递归收集：

```sh
cmake --preset windows-portable
cmake --build --preset windows-portable
cmake --install build-windows-portable --prefix out/SemiPlayer-portable
```

便携版安装会将 SDL3、FFmpeg、spdlog、MinGW 运行库及其非系统传递依赖复制到
`out/SemiPlayer-portable/bin/`。Windows 系统 DLL 会被排除。当前 MSYS2 FFmpeg
启用了大量可选组件，因此目录仍然较大；后续可以通过定制 FFmpeg 构建进一步
缩小发布包体积。

生成带版本号且只包含运行组件的便携版 ZIP：

```sh
cmake --build --preset windows-portable --target package
```

产物位于
`out/packages/SemiPlayer-0.1.0-windows-x64-portable.zip`，解压后双击
`bin/semi_player_sdl.exe` 即可播放随包示例。

### 持续集成（CI）无声卡构建

GitHub Actions 使用独立的 `windows-ci` 预设，将组合根切换为
`NullAudioOutputBackend`，避免依赖运行器的音频设备；默认 `windows-all` 仍使用
miniaudio：

```sh
cmake --preset windows-ci
cmake --build --preset windows-ci
ctest --test-dir build-windows-ci --output-on-failure
```
