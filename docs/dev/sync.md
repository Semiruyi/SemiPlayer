# Audio/Video Synchronization

This document describes the current synchronization model in `semi_player_rs`.

For the overall frame flow, see [pipeline.md](pipeline.md).

## Core Model

Audio is the **master clock**. Video follows audio.

```text
target_video_time = audio_playback_time + presentation_bias
```

- `audio_playback_time` is owned by `AudioClock`.
- `presentation_bias` is a host-supplied estimate of display pipeline latency.
- `VideoScheduler` compares `target_video_time` against video frame PTS values to decide whether to keep, present, or drop a frame.

## AudioClock (Current)

File: [`semi_player_rs/src/audio/core/clock.rs`](../../semi_player_rs/src/audio/core/clock.rs)

`AudioClock` is currently a **software projection** based on `std::time::Instant`:

- `play()` records the wall-clock anchor instant.
- `pause()` freezes time by clearing the anchor.
- `seek()` moves the anchor media time.
- `set_speed()` changes the speed multiplier applied to elapsed wall-clock time.

```rust
pub fn presentation_time_us(&self) -> MediaTimeUs {
    // paused: return frozen anchor time
    // playing: anchor_time + (elapsed_wall_clock * speed)
}
```

**Limitation**: This is not yet tied to actual audio hardware playback position. When an audio output backend (e.g., WASAPI via `cpal`) is implemented, `AudioClock` should derive its position from the cumulative number of samples consumed by the audio device rather than from `Instant::now()`.

## VideoScheduler

File: [`semi_player_rs/src/render/core/scheduler.rs`](../../semi_player_rs/src/render/core/scheduler.rs)

`VideoScheduler::decide` receives:

- `target_time_us`: the synchronized playback position.
- `current_frame`: the frame currently on screen (if any).
- `candidate_frame`: the next frame in the queue (if any).

Decisions:

| Decision | Condition |
|---|---|
| `KeepCurrent` | `current_frame` covers `target_time_us` |
| `PresentFrame` | `candidate` is the right frame for `target_time_us` |
| `DropFrame` | `candidate` is already stale (`target_time >= candidate.end_time`) |
| `NeedMoreFrames` | Queue is empty or candidate is in the future |

## Pump Loop Integration

File: [`semi_player_rs/src/core/player/pump.rs`](../../semi_player_rs/src/core/player/pump.rs)

During `pump_player`:

1. Decode outputs are pushed into `PlayerRuntime` queues.
2. When the audio queue reaches `TARGET_AUDIO_QUEUE_LEN` (8 frames), `select_video_frame` is called.
3. `select_video_frame` computes `target_video_time_us` from `AudioClock + bias` and asks `VideoScheduler` to pick the current frame.
4. If a current frame is selected, the pump exits to avoid over-decoding.

## Future Work

- Replace `Instant::now()`-based `AudioClock` with sample-count-based tracking once an audio backend is implemented.
- Add explicit drift compensation when the audio hardware clock and system clock diverge.
- Evaluate whether `presentation_bias` should evolve into a richer present-feedback loop.
