# Hardware decode fixtures

These silent, one-second clips contain FFmpeg's generated `testsrc2` pattern
(128x96, 12 frames). They contain no downloaded footage. Test execution reads
the checked-in files and does not need encoders or an ffmpeg executable.

Generated with MSYS2 FFmpeg using:

```sh
ffmpeg -f lavfi -i testsrc2=size=128x96:rate=12 -frames:v 12 \
  -pix_fmt yuv420p10le -c:v libx265 -preset ultrafast \
  -x265-params pools=none:frame-threads=1:log-level=error:keyint=6 \
  -an hevc-main10.mp4
ffmpeg -f lavfi -i testsrc2=size=128x96:rate=12 -frames:v 12 \
  -pix_fmt yuv420p10le -c:v libx264 -preset ultrafast -profile:v high10 \
  -g 6 -an h264-high10.mp4
```

- HEVC Main10 verifies D3D11 P010 output, native readback and image comparison
  against software decode.
- H.264 High10 verifies software negotiation for an unsupported hardware path.
