# Audio/Video Synchronization

This document describes the current synchronization model in `semi_player_rs`.

For the overall frame flow, see [pipeline.md](pipeline.md).
For the planned player-owned video timing system, see [internal-video-sync.md](internal-video-sync.md).

## Core Model

Audio is the **master clock**. Video follows audio.

```text
target_video_time = audio_playback_time + host_presentation_offset
```

- `audio_playback_time` is owned by `AudioClock`.
- `host_presentation_offset` is a host-supplied estimate of display pipeline latency.
- `VideoScheduler` compares `target_video_time` against video frame timing to decide whether to keep, present, or drop a frame.

## AudioClock (Current)

File: [`semi_player_rs/src/audio/core/clock.rs`](../../semi_player_rs/src/audio/core/clock.rs)

`AudioClock` is no longer only a software wall-clock projection.

Current behavior:

- `play()` starts timeline progression
- `pause()` freezes the timeline
- `seek()` rebases the timeline
- `set_speed()` changes the speed multiplier
- when device playback timing is available, `AudioClock` prefers backend-derived playback progress

That means the clock can now track:

- logical timeline continuity
- estimated audible presentation time

## VideoScheduler (Current)

File: [`semi_player_rs/src/render/core/scheduler.rs`](../../semi_player_rs/src/render/core/scheduler.rs)

`VideoScheduler::decide` receives:

- `target_time_us`: the synchronized playback position
- `current_frame`: the frame currently considered on screen
- `candidate_frame`: the next frame in the queue

Decisions:

| Decision | Condition |
|---|---|
| `KeepCurrent` | the current frame still covers the target time |
| `PresentFrame` | the candidate frame is the right frame for the target time |
| `DropFrame` | the candidate frame is already stale |
| `NeedMoreFrames` | there is no suitable frame yet |

Important current behavior:

- when a valid next-frame PTS is known, it is treated as a stronger effective end boundary for the current frame than the current frame's own decoded duration

That matches the current `CoreSyncErr` diagnostic semantics.

## Current `pump` Integration

File: [`semi_player_rs/src/core/player/pump.rs`](../../semi_player_rs/src/core/player/pump.rs)

Today the player still uses `pump_player(...)` to do all of the following:

1. decode outputs and push them into `PlayerRuntime`
2. discard consumed audio frames
3. synchronize audio output
4. select the current video frame

This is sufficient for:

- smoke tests
- diagnostics
- simple host integration

But it is not the desired long-term timing architecture.

## Current Diagnostics

The playback snapshot currently exposes:

- `Core A-V`
- `CoreSyncErr`
- `HostOffset`
- `Expected End-to-end A-V`

Interpretation:

- `Core A-V` describes where the audio clock lies relative to the current frame start time
- `CoreSyncErr` describes whether the current frame is actually the correct frame for the target time
- `HostOffset` is the host-supplied display compensation estimate
- `Expected End-to-end A-V` is a model-derived result after applying host offset, not a true end-to-end measured display metric

## Current Limitation

Even with the current audio-clock and scheduler improvements, frame switching is still externally timing-sensitive because:

- frame promotion happens during `pump`
- faster `pump` rates reduce mean sync error
- host/UI polling still affects timing quality

That is the main reason a player-owned internal video sync system is the next architectural step.

## Future Work

- move from `pump`-driven frame promotion toward a player-owned internal video sync loop
- preserve `pump` as a control-plane and decode-supply API
- continue refining late/early frame handling thresholds
- evolve host presentation offset into a richer feedback model only after the internal sync loop exists
