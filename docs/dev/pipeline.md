# Decoding and Frame Output Pipeline

This document describes the current implementation of the media decoding pipeline and frame output flow in `semi_player_rs`. It reflects the state of the working tree after the decoder drain-queue and BGRA output changes.

For the overall architecture goals and boundaries, see [ARCHITECTURE.md](../../ARCHITECTURE.md).

---

## 1. Overview

```text
FFmpeg input
    │
    ▼
demux (read packet)
    │
    ▼
decode (send_packet / receive_frame)
    │
    ▼
pending_outputs queue (DecodedOutput buffer)
    │
    ▼
next_decoded_output() ──► Pump loop
    │
    ├──► VideoFrame ──► swscale ──► BGRA ──► runtime video queue
    │
    └──► AudioFrame ──► runtime audio queue
                │
                ▼
        audio >= 8 frames?
                │
                ▼
        select_video_frame()
                │
                ▼
        current_video_frame
                │
                ▼
    FFI: get_current_video_frame_info()
    FFI: copy_current_video_frame_bgra()
                │
                ▼
            UI Host
```

---

## 2. Decode Layer: OpenedMedia

File: [`semi_player_rs/src/core/media/opened.rs`](../../semi_player_rs/src/core/media/opened.rs)

### 2.1 Internal Buffer

`OpenedMedia` owns a `VecDeque<DecodedOutput>` called `pending_outputs`. It exists because FFmpeg's `avcodec_send_packet` / `avcodec_receive_frame` API is push-pull: one packet can produce zero, one, or many frames. The queue smooths this mismatch so that `next_decoded_output()` can still return exactly one item per call.

### 2.2 Entry Point

```rust
pub fn next_decoded_output(&mut self) -> Result<Option<DecodedOutput>, MediaOpenError>
```

Logic on each call:

1. **Check queue** — if `pending_outputs` is not empty, pop front and return.
2. **Draining** — if the file has reached EOF and decoders are being flushed, collect any remaining frames from the decoder internals. Once both video and audio decoders are fully drained, emit `DecodedOutput::EndOfStream`.
3. **Read next packet** — if no packet is available, enter draining mode and loop back to step 2.
4. **Route to decoder** — based on `stream_index`, send the packet to the video or audio decoder.
5. **Collect frames** — after `send_packet`, loop `receive_frame` until `EAGAIN` and push all produced frames into `pending_outputs`.
6. Return the first frame from `pending_outputs`.

### 2.3 Packet-to-Frames: No Longer One-to-One

Previous code returned at most one frame per packet. Current code uses `collect_video_frames` and `collect_audio_frames`:

```rust
fn collect_video_frames(
    decoder: &mut OpenedVideoDecoder,
    outputs: &mut VecDeque<DecodedOutput>,
    draining: bool,
) -> Result<bool, MediaOpenError>
```

This loops `receive_frame` until `EAGAIN` (or `EOF` when draining), pushing every decoded frame into `outputs`. The boolean return indicates whether the decoder has fully drained.

### 2.4 EAGAIN Handling on Send

If `send_packet` returns `EAGAIN`, the decoder's input buffer is full. The code now:

1. Calls `collect_*_frames` to drain already-decoded frames.
2. Checks whether the output queue grew.
3. If no frames were produced and the decoder is still full, propagates the error.
4. Otherwise retries `send_packet`.

### 2.5 Draining at End-of-File

When `read_next_packet` returns `None`:

1. `enter_draining_mode()` sends `send_eof()` to both video and audio decoders.
2. `collect_drained_outputs()` repeatedly calls `collect_video_frames` and `collect_audio_frames` with `draining = true`.
3. When both decoders report fully drained, `EndOfStream` is emitted once.

---

## 3. Video Frame Processing

### 3.1 Pixel Format Conversion

Every decoded `frame::Video` from FFmpeg is converted to **BGRA** before it reaches the rest of the pipeline.

File: [`semi_player_rs/src/core/media/opened.rs`](../../semi_player_rs/src/core/media/opened.rs)

```rust
fn convert_video_frame_to_bgra(
    decoder: &mut OpenedVideoDecoder,
    input: &frame::Video,
) -> Result<frame::Video, MediaOpenError>
```

- Uses `ffmpeg_next::software::scaling` (`swscale`).
- `OpenedVideoDecoder` caches a `ScalingContext`. The context is rebuilt only when the input format, width, or height changes.
- Output format is hard-coded to `format::Pixel::BGRA` with bilinear scaling flags.

### 3.2 VideoFrame Structure

File: [`semi_player_rs/src/render/core/frame.rs`](../../semi_player_rs/src/render/core/frame.rs)

```rust
pub struct VideoFrame {
    pub pts_us: MediaTimeUs,
    pub duration_us: Option<MediaTimeUs>,
    pub width: u32,
    pub height: u32,
    pub pixel_format: PixelFormatCategory,  // currently always Bgra8
    pub stride: usize,                      // bytes per row (including padding)
    pub data: Vec<u8>,                      // full BGRA pixel data
    pub is_key_frame: bool,
}
```

The `data` field is populated by `copy_packed_plane`, which copies the first plane of the swscaled frame into a `Vec<u8>`.

---

## 4. Pump Loop and Runtime Queues

File: [`semi_player_rs/src/core/player/pump.rs`](../../semi_player_rs/src/core/player/pump.rs)

### 4.1 Pump Entry

```rust
pub fn pump_player(player: &mut SemiPlayerHandle, max_iterations: u32) -> ResultCode
```

Called by the host through `semi_player_pump`. It loops up to `max_iterations` (default 256), pulling decoded outputs and pushing them into the runtime.

### 4.2 Frame Distribution

```rust
match output {
    DecodedOutput::Video(frame) => player.runtime.push_video_frame(frame),
    DecodedOutput::Audio(frame) => player.runtime.push_audio_frame(frame),
    DecodedOutput::EndOfStream => player.runtime.mark_end_of_stream(),
}
```

### 4.3 Early Exit Condition

```rust
if player.runtime.audio_queue_len() >= TARGET_AUDIO_QUEUE_LEN {
    select_video_frame(player);
    if player.runtime.has_current_video_frame() {
        break;
    }
}
```

`TARGET_AUDIO_QUEUE_LEN` is `8`. When at least 8 audio frames are buffered and a current video frame has been selected, the pump stops to avoid over-decoding.

### 4.4 Video Frame Selection

```rust
fn select_video_frame(player: &mut SemiPlayerHandle) {
    let target_video_time_us = add_media_time_us(
        player.audio_clock.presentation_time_us(),
        player.video_presentation_bias_us,
    );
    let _ = player
        .runtime
        .select_video_frame(&player.video_scheduler, target_video_time_us);
}
```

- Audio clock is the master clock.
- `video_presentation_bias_us` is a host-supplied display latency compensation.
- `VideoScheduler` decides `KeepCurrent`, `PresentFrame`, `DropFrame`, or `NeedMoreFrames`.

---

## 5. Frame Output to Host

File: [`semi_player_rs/src/lib.rs`](../../semi_player_rs/src/lib.rs)

After `pump_player`, the host can query the currently selected video frame through two FFI functions:

### 5.1 Query Metadata

```rust
#[no_mangle]
pub extern "C" fn semi_player_get_current_video_frame_info(
    player: *mut SemiPlayerHandle,
    out_frame_info: *mut SemiVideoFrameInfo,
) -> c_int
```

Returns:
- `pts_ms`, `duration_ms`
- `width`, `height`
- `stride` — bytes per scanline
- `pixel_format` — currently always `4` (`Bgra8`)
- `byte_len` — total size of `data` in bytes
- `flags` — `1` if key frame

### 5.2 Copy Pixel Data

```rust
#[no_mangle]
pub extern "C" fn semi_player_copy_current_video_frame_bgra(
    player: *mut SemiPlayerHandle,
    destination: *mut u8,
    destination_len: u32,
) -> c_int
```

Copies the full BGRA payload from `VideoFrame.data` into the host-provided buffer using `ptr::copy_nonoverlapping`. The host must allocate at least `byte_len` bytes.

### 5.3 Typical Host Usage

```csharp
// C# / .NET example from smoke test
EnsureOk(Native.semi_player_pump(player, 0), "semi_player_pump");
EnsureOk(Native.semi_player_get_current_video_frame_info(player, out var info), "get_info");
byte[] frameBytes = new byte[info.ByteLen];
EnsureOk(Native.semi_player_copy_current_video_frame_bgra(player, frameBytes, info.ByteLen), "copy");
// frameBytes now contains raw BGRA pixels
```

---

## 6. Responsibility Split

| Component | Owns | Does Not Own |
|---|---|---|
| `OpenedMedia` | demux, decode, pixel conversion, internal frame queue | playback timing, synchronization |
| `PumpPlayer` | calling decode, distributing frames, triggering selection | rendering, audio output |
| `PlayerRuntime` | video/audio queues, current frame slot | decode logic |
| `VideoScheduler` | frame keep/present/drop decision | clock source |
| `AudioClock` | master playback time (currently software-based) | audio hardware output |
| Host (FFI caller) | calling pump, measuring display latency, copying pixels, presenting | decode, sync rules |

---

## 7. Known Limitations

- Audio output backend is not implemented. `AudioClock` is currently a software projection (`Instant::now()` based), not tied to actual audio hardware playback position.
- `queued_audio_frames` in `PlayerRuntime` are buffered but not consumed by any audio renderer.
- Video frames are always converted to BGRA. No path yet for passing YUV/NV12 directly to a GPU backend.
- The pump loop is single-threaded. Decoding happens synchronously inside `semi_player_pump`.
