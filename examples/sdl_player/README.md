# SDL3 playback host

This example is an external consumer of the public SemiPlayer C ABI. SemiPlayer
continues to own audio playback; SDL3 provides the window, input handling, CPU
frame upload, and presentation.

The VideoSync callback never calls SDL rendering APIs. It copies the borrowed
RGBA frame into a host-owned latest-frame mailbox and posts an SDL user event.
The SDL main thread takes only the newest frame, uploads it to a streaming
texture, and preserves the video aspect ratio while presenting.

## Build and run

Install SDL3 together with the other UCRT64 dependencies:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-sdl3
cmake --preset windows-all
cmake --build --preset windows-all
./build-windows/bin/semi_player_sdl.exe path/to/media.mp4
```

Controls:

- Space: play or pause
- Left/Right: request five seconds backward/forward, landing on the previous/next keyframe
- F11: toggle fullscreen
- Escape: quit
