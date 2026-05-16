# Decoding and Playback Pipeline

This document describes the current playback pipeline in `semi_player_rs`.

For synchronization rules, see [sync.md](sync.md).
For higher-level system boundaries, see [ARCHITECTURE.md](../../ARCHITECTURE.md).

## 1. Current Shape

Today the player is split into four cooperating parts:

```text
FFI / host commands
  -> serialized player handle access
  -> player runtime state
  -> internal sync worker
  -> FFmpeg decode supply
  -> audio output backend
```

Important current rule:

- playback progression is now primarily driven by the internal sync worker
- `semi_player_pump(...)` still exists and is still useful
- decode supply now has its own internal worker lane
- decode supply still shares the same serialized player lock, so it is not yet an independently concurrent pipeline
- lock-wait diagnostics are now split by `ffi`, `sync worker`, and `decode worker`

That means the player has already crossed the line from:

```text
host polling decides when frames advance
```

to:

```text
player-owned workers decide when playback should advance and when decode should refill
```

but decode is not yet split into a lock-independent concurrent pipeline.

## 2. End-to-End Flow

```text
open_media()
  -> OpenedMedia
  -> demux + decode
  -> DecodedOutput queue
  -> PlayerRuntime audio/video queues
  -> AudioOutputController
  -> AudioClock
  -> VideoSyncService
  -> current video frame
  -> FFI frame read / copy
  -> host presentation
```

## 3. Decode Supply

File: [`semi_player_rs/src/core/media/opened.rs`](../../semi_player_rs/src/core/media/opened.rs)

`OpenedMedia` owns the FFmpeg-side state:

- input context
- selected audio/video decoders
- scaling / resampling helpers
- pending decoded outputs

The external-facing decode API is still:

```rust
pub fn next_decoded_output(&mut self) -> Result<Option<DecodedOutput>, MediaOpenError>
```

Behavior:

1. if decoded outputs are already buffered, return one
2. otherwise read packets from FFmpeg
3. send packets to the right decoder
4. drain all available frames from that decoder
5. queue normalized `DecodedOutput` items
6. return one item to the caller

This layer does not own playback timing.

## 4. Runtime Queues

File: [`semi_player_rs/src/core/player/runtime.rs`](../../semi_player_rs/src/core/player/runtime.rs)

`PlayerRuntime` owns the short-lived playback buffers:

- queued decoded audio frames
- queued decoded video frames
- the current promoted video frame
- end-of-stream flag

Current ownership split:

- decode supply writes queued audio/video frames
- video sync owns promotion into `current_video_frame`
- FFI readers only observe current state

## 5. Audio Path

Relevant files:

- [`semi_player_rs/src/audio/core/clock.rs`](../../semi_player_rs/src/audio/core/clock.rs)
- [`semi_player_rs/src/audio/core/output_controller.rs`](../../semi_player_rs/src/audio/core/output_controller.rs)
- [`semi_player_rs/src/audio/backends.rs`](../../semi_player_rs/src/audio/backends.rs)

Current audio path:

```text
decoded AudioFrame
  -> runtime audio queue
  -> pull_audio_chunk()
  -> SharedAudioOutputController
  -> CPAL backend
  -> backend timing snapshot
  -> AudioClock
```

Important current behavior:

- the player uses audio as the master clock
- `AudioClock` prefers backend playback timing when available
- audio output control now has its own shared handle boundary, like decode media state
- the CPAL backend exposes:
  - buffered frames
  - pending device frames
  - rendered frame counters
  - audible frame counters

This gives the player a better estimate of what the user is actually hearing.

## 6. Video Path

Relevant files:

- [`semi_player_rs/src/render/core/frame.rs`](../../semi_player_rs/src/render/core/frame.rs)
- [`semi_player_rs/src/render/core/scheduler.rs`](../../semi_player_rs/src/render/core/scheduler.rs)
- [`semi_player_rs/src/core/player/video_sync.rs`](../../semi_player_rs/src/core/player/video_sync.rs)

Current video path:

```text
decoded video frame
  -> swscale to BGRA
  -> VideoFrame
  -> runtime queued video frames
  -> VideoScheduler decision
  -> current video frame
  -> FFI metadata / BGRA copy
  -> host UI
```

The current frame-selection rules already support:

- keep current
- present next
- drop stale
- wait for more frames

The effective end of the current frame prefers the next frame PTS when available.

## 7. Internal Sync Worker

Relevant files:

- [`semi_player_rs/src/core/player/execution.rs`](../../semi_player_rs/src/core/player/execution.rs)
- [`semi_player_rs/src/core/player/decode_worker.rs`](../../semi_player_rs/src/core/player/decode_worker.rs)
- [`semi_player_rs/src/core/player/sync_worker.rs`](../../semi_player_rs/src/core/player/sync_worker.rs)
- [`semi_player_rs/src/core/player/schedule.rs`](../../semi_player_rs/src/core/player/schedule.rs)

This is the biggest current architectural change.

The player now starts an internal sync worker when the handle is created.
It also starts a dedicated decode worker.

Worker loop:

```text
lock player
  -> inspect current state
  -> evaluate schedule
  -> if playback should advance:
       capture a playback plan
unlock player
  -> execute audio-output work outside the main player lock
lock player
  -> finish playback advancement and video sync
  -> if Ready or Paused:
       run one stabilization pass if work is still pending
       then stop active waiting
  -> if Idle:
       wait for explicit wake
repeat
```

Decode worker loop:

```text
lock player
  -> build a decode plan
  -> capture shared media handle + generation
unlock player
  -> poll FFmpeg decode with a small packet budget
lock player
  -> discard stale results if media generation changed
  -> apply decoded output into runtime queues
  -> wake sync worker if new frames arrived
  -> decide whether to continue or sleep
repeat
```

Current worker modes:

- `Playing`
  - normal continuous timing mode
  - follows computed deadlines
- `Ready` / `Paused`
  - stabilization mode
  - lets the player settle internal state after open/seek/pause
  - does not stay in an active timed loop
- `Idle`
  - no media-owned work
  - sleeps until explicit wake

The worker is woken on:

- play
- pause
- seek
- reset
- speed change
- host presentation bias change
- explicit external `semi_player_pump(...)`

Current scheduling input combines:

- next video sync deadline
- next audio refill deadline
- decode-supply-needed state
- a dedicated decode-schedule hint used by:
  - decode worker
  - manual pump path
  - internal decode wake requests
- immediate wake conditions such as:
  - dirty sync state
  - stale current video frame
  - unstarted audio backend while playing

Execution ownership is now split more explicitly:

- `schedule.rs`
  - decides playback-facing work and timing deadlines
- `execution.rs`
  - execution facade
  - coordinates playback advancement and decode supply
- `execution/playback_advance.rs`
  - advances audio/video playback state
- `execution/decode_supply.rs`
  - runs synchronous decode supply
- `decode_worker.rs`
  - owns decode refill wake/sleep policy
- `pump.rs`
  - remains as an external/manual entry point

## 8. Serialized FFI Access

Relevant file:

- [`semi_player_rs/src/core/player/handle.rs`](../../semi_player_rs/src/core/player/handle.rs)

The player handle now serializes mutable access through a single operation lock.
Playback advancement also uses a separate phase lock so host mutations such as open, seek, reset,
or manual pump do not interleave with a sync-worker playback step while it is executing outside the
main player lock.

Why this exists:

- the sync worker and FFI calls can both touch the same runtime state
- first correctness priority is avoiding unsafe concurrent mutation

Current rule:

- one player operation runs at a time

Current observability:

- FFI lock wait is measured separately
- sync-worker lock wait is measured separately
- decode-worker lock wait is measured separately

That is intentionally conservative. It is a good first boundary before deeper task splitting.

## 9. Host Read Path

Relevant FFI:

- `semi_player_get_playback_snapshot(...)`
- `semi_player_get_audio_output_snapshot(...)`
- `semi_player_get_current_video_frame_info(...)`
- `semi_player_copy_current_video_frame_bgra(...)`

The host currently interacts in two broad ways:

1. control:
   - open
   - play
   - pause
   - seek
   - speed
   - presentation bias
2. observation:
   - playback snapshot
   - audio output snapshot
   - current video frame metadata
   - BGRA frame copy

The host no longer needs to be the primary driver of frame advancement.

## 10. Current Limitations

The current pipeline is much healthier than the original pump-only prototype, but it is still not the final architecture.

Main limitations:

- runtime queue mutation and FFmpeg media control are now split, but media open/seek/reset still coordinate through the player handle
- decode output application still serializes with other player mutations
- audio output access is now independently lockable, and playback advancement now executes its audio-output phase outside the main player lock
- runtime/audio/video commit still serializes back through the player handle
- video frame delivery is still CPU-copy BGRA, not GPU-native
- subtitle timing and composition are not yet integrated into the worker-driven pipeline
- smoke tooling still mixes diagnostic and host responsibilities more than a final host should

## 11. Near-Term Direction

The most likely next architecture steps are:

1. reduce coupling between decode worker and the serialized player lock
2. tighten notification flow between decode enqueue and sync wake-up
3. add worker-vs-host diagnostic modes for objective sync measurement
4. introduce real render backend and subtitle composition path
