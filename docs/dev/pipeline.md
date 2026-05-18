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

Current incremental split:

```text
decoded output
  -> decoded-video queue
  -> render supply
  -> presentation-video queue
  -> current presentation frame
```

Important current rule:

- video sync and scheduling should now be thought of as operating on presentation frames
- decode output is no longer the only video queue concept in runtime
- decoded-video to presentation-video flow now goes through an explicit render-supply step
- playback readiness should be judged from presentation-ready frames, not just total decoded backlog

Current limitation:

- the first render-supply implementation is still synchronous passthrough
- render-core now has a pipeline entry point, but it still returns the decoded frame unchanged
- no independent render worker exists yet

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
  - follows the same schedule-driven playback/decode split as the worker path

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

## 11. Hardware Decode and Output Surface Plan

The next video-path milestone is to move from CPU-copy BGRA delivery to GPU-native decoded
surfaces on Windows, while keeping the Rust core portable enough for a later Avalonia host.

Current product direction:

- short term host: WPF
- long term host direction: Avalonia
- short term video backend target: D3D11 hardware decode
- non-goal for the first hardware path: GPU-to-CPU readback

That means the player core should not define its output as:

- a WPF-specific object
- a copied BGRA byte buffer
- a permanently D3D11-only host contract

Instead, the core should define its output as:

- a video frame with timing metadata
- carrying a backend-owned video surface

### 11.1 Core design rule

The player core should hand off:

```text
timed video surface
```

not:

```text
WPF image object
```

and not:

```text
BGRA copy-out as the only supported representation
```

### 11.2 Planned video frame split

Today `VideoFrame` combines:

- timing
- dimensions
- pixel format
- CPU-side packed bytes

The planned shape is to split that into:

1. frame timing / scheduling metadata
2. surface ownership / storage metadata

Representative direction:

```text
VideoFrame
  -> pts / duration / dimensions / key-frame flag
  -> Arc<VideoSurface>
```

and:

```text
VideoSurface
  -> kind
  -> format
  -> backend-owned storage
```

Planned surface kinds:

- `CpuBgra`
- `D3d11Texture2D`

Planned surface formats:

- `Bgra8`
- `Nv12`
- `P010`

This preserves the existing scheduling model while removing the assumption that every decoded
video frame must become a CPU-owned `Vec<u8>`.

### 11.3 Decode-path target

The Windows hardware-decode path should prefer native decoder-friendly formats such as:

- `NV12`
- `P010`

The decode layer should be responsible for:

- opening the D3D11 video device/context
- configuring FFmpeg hardware decode
- receiving hardware-backed decoded frames
- wrapping the decoded texture as a `VideoSurface`

The decode layer should not be responsible for:

- creating WPF objects
- baking in WPF presentation rules
- forcing all hardware output back through BGRA conversion

The existing software BGRA path should remain available as:

- fallback
- compatibility path
- debug path

### 11.4 Runtime and scheduler impact

The current runtime and scheduling model should stay mostly intact.

The important rule is:

- queue and schedule timed frame objects
- do not collapse the runtime into a single mutable "latest texture"

Why:

- seek recovery already depends on frame-level timing
- drop/present/keep decisions already exist at frame granularity
- later subtitle timing needs a stable video-time anchor
- surface lifetime should naturally follow frame lifetime

### 11.5 Video-render boundary

The next important architectural step is to stop treating:

```text
decoded surface
```

and:

```text
host-presentable frame
```

as the same thing.

The preferred pipeline direction is:

```text
compressed packet
  ->
decoder-native surface
  ->
player-owned video render
  ->
presentation-friendly frame
  ->
host presenter
```

Representative internal model split:

```text
DecodedVideoFrame
  -> pts / duration / dimensions
  -> DecoderSurface

PresentationFrame
  -> pts / duration / dimensions
  -> RenderSurface
```

Where:

- `DecoderSurface` keeps decoder-native formats such as `D3D11 NV12`
- `RenderSurface` keeps presentation-oriented outputs such as `D3D11 BGRA`

This lets the player own:

- color conversion
- scaling
- future subtitle composition

Current implementation status:

- `DecodedVideoFrame` and `PresentationFrame` roles now exist
- `PlayerRuntime` now contains separate decoded-video and presentation-video queues
- `execution/render_supply.rs` now owns the decoded-to-presentation handoff entry point
- `render/core/pipeline.rs` now owns the render-core frame transformation entry point
- render-core pipeline input is now an explicit render request carrying output preference and
  subtitle-visibility intent
- that render request now also carries presentation-surface-kind preference
- render requests can now be constructed from higher-level presentation target profiles such as
  CPU-BGRA compatibility or D3D11-presenter intent
- render-core planning now distinguishes passthrough-safe requests from requests that already
  require a real transform implementation
- playback diagnostics can now expose how many rendered frames were passthrough versus still
  requiring a real transform path
- the current default render-supply request now targets the CPU-BGRA compatibility contract used
  by the existing host copy-out path
- that target profile is now player-owned state so a host can switch presentation contracts without
  redefining render-supply logic
- transform-required frames are now tracked separately from temporary fallback-passthrough
  execution so true passthrough and stopgap execution are no longer conflated
- the D3D11-BGRA presenter path now has an explicit backend call site even though the backend still
  reports unavailable work instead of completing the transform
- the D3D11 backend now has explicit request / target / plan / renderer skeleton types so the first
  real GPU execution path can land behind a stable internal contract
- D3D11 BGRA texture input can now complete a true backend passthrough path while NV12/YUV render
  work still reports backend-unavailable instead of pretending to succeed
- that first render-stage implementation still promotes decoded frames immediately into
  presentation frames

without forcing each host to understand decoder-native formats.

### 11.6 Host adapter boundary

The Rust core should expose a surface-oriented contract over FFI.

The host-specific adapters should live above that boundary:

- WPF adapter
- future Avalonia adapter

But the normal host contract should trend toward presentation-oriented frames, not raw decoder
surfaces.

In other words:

- decoder-native surfaces are primarily an internal decode/render concern
- host adapters should usually consume presentation frames

This means the ABI should move toward:

- presentation-frame descriptors
- explicit acquire/release semantics for host-visible render surfaces
- optional low-level decoder-surface diagnostics where needed

instead of only:

- current-frame BGRA metadata
- current-frame BGRA copy

Short-term WPF delivery can therefore be implemented as:

```text
Rust core
  -> D3D11 decoder-native surface
  -> player-owned video render
  -> presentation-friendly RGB surface
  -> FFI frame/surface descriptor
  -> WPF-specific presenter adapter
  -> final host presentation
```

without redefining the Rust core around WPF types.

### 11.7 Subtitle compatibility rule

Subtitles should not be burned into decoded video surfaces in the first hardware-decode design.

Instead, subtitle work should remain a parallel timing/composition path:

```text
subtitle source
  -> subtitle cues
  -> subtitle scheduler
  -> player-owned video render or transitional host overlay
```

The short-term preferred approach is:

- decode video into decoder-native surfaces
- render video into presentation-friendly RGB surfaces
- keep subtitle timing independent first
- allow a transitional host overlay path before folding subtitle composition into player-owned video render

Reasons:

- WPF can ship sooner
- Avalonia can reuse the same subtitle timing model
- subtitle visibility/style changes stay outside the decode path
- later GPU composition remains possible without redesigning seek/sync semantics

Long-term architectural target:

- subtitle composition should belong to the player render stage

### 11.8 First implementation phases

Recommended implementation order:

1. split frame timing from surface storage
2. preserve the existing software BGRA path under the new surface model
3. add explicit decoded-surface vs presentation-surface contracts
4. add D3D11 surface types and resource ownership
5. implement a player-owned video-render stage:
   - native surface in
   - RGB presentation surface out
6. add a presentation-oriented FFI contract
7. build the first WPF presenter adapter on top of that contract
8. introduce subtitle timing first, then subtitle composition in the render stage

Immediate next sub-steps:

1. keep the first render-service implementation synchronous passthrough while the interface settles
2. make sync/error diagnostics explicitly presentation-frame-oriented
3. teach render supply to own real color conversion / surface transformation
4. only then decide whether render needs asynchronous execution

## 12. Near-Term Direction

The most likely next architecture steps are:

1. reduce coupling between decode worker and the serialized player lock
2. tighten notification flow between decode enqueue and sync wake-up
3. add worker-vs-host diagnostic modes for objective sync measurement
4. introduce the decoded-surface / presentation-surface split
5. introduce the player-owned video-render stage and Windows D3D11 decode path
6. add subtitle timing and host overlay composition boundaries, then move composition into the render stage
