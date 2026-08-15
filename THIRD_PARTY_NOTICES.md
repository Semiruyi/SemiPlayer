# Third-party notices

SemiPlayer's Windows portable package includes third-party software. The
release packaging step replaces this source-tree summary with a generated
manifest containing every runtime DLL, its MSYS2 package version, declared
license, homepage, and all license files shipped by that package.

## FFmpeg

- Usage: demuxing, audio/video decoding, resampling, and pixel conversion
- Portable build: MSYS2 UCRT64 shared libraries
- Current packaged license: GPL-3.0-or-later
- Homepage: https://ffmpeg.org/

The current MSYS2 build enables GPL and GPLv3 components, including x264 and
x265. Run `ffmpeg -buildconf` and `ffmpeg -L` in the release environment for
the exact build configuration and license statement.

## SDL 3

- Usage: example window, input, and video presentation
- License: Zlib
- Homepage: https://libsdl.org/

## spdlog

- Usage: logging
- License: MIT
- Homepage: https://github.com/gabime/spdlog

## miniaudio

- Usage: audio output; compiled into SemiPlayer
- License choice used by SemiPlayer: MIT-0
- Homepage: https://miniaud.io/

## GCC runtime libraries and MinGW-w64

- Usage: C++ and threading runtime libraries in Windows binary distributions
- Licenses: GPL-3.0-with-GCC-exception and MinGW-w64 runtime licenses
- GCC runtime exception: permits eligible non-GPL programs to use covered
  runtime libraries; SemiPlayer itself is GPL-3.0-or-later.

The generated release manifest is authoritative for the exact binary package.
