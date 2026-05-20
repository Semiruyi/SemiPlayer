# SemiPlayer Architecture

SemiPlayer is a media playback project built around a cross-platform Rust core.

Current implementation focus:

- first verified platform: Windows
- first host shell: WPF smoke app
- long-term host direction: Avalonia

The core architectural rule remains:

- playback semantics belong to the Rust core
- platform presentation details belong to backend and host adapter layers

## 1. Goals

Build a player core that can own:

- create / destroy
- open / play / pause
- seek / reset
- speed control
- subtitle timing
- synchronized audio/video playback
- host-aware presentation bias compensation

And do that without baking WPF-specific behavior into the core.

## 2. Current System Layers

Today the project is best understood as:

```text
Host shell
  ->
FFI / interop boundary
  ->
Rust playback core
    ->
audio backend + future render backend(s)
```

Inside the Rust playback core, the current shape is:

```text
commands / FFI
  -> serialized player handle access
  -> media decode supply
  -> runtime queues
  -> audio output + audio clock
  -> video sync service
  -> internal sync worker
```

## 3. Current Verified State

Already verified:

- local FFmpeg shared build
- Rust build in `semi_player_rs`
- C ABI interop through `.NET`
- decoded audio/video queueing
- D3D11VA hardware video decode with NV12/P010 GPU surface output
- NV12 to BGRA conversion via GPU staging + CPU swscale
- CPU BGRA frame copy-out to host
- CPAL-based audio output timing feedback
- player-owned sync worker driving playback progression
- player-owned decode worker and render worker
- render service with pipeline planning (passthrough vs transform)
- seek recovery now explicitly identifies software video decode as the dominant remaining seek cost

Reference:

- [docs/env/windows.md](c:/y-s/project/Semi/docs/env/windows.md)

## 4. Current Repository Layout

```text
Semi/
  semi_player_rs/              Rust playback core crate
  tools/
    smoke/
      SemiPlayer.HelloTest/    diagnostic WPF host
  third_party/                 local FFmpeg package and related assets
  docs/
    dev/
    env/
  ARCHITECTURE.md
  TODO.md
```

## 5. Core Ownership

The Rust core currently owns:

- media open / probe / decode
- audio output scheduling
- audio master clock
- video frame scheduling
- current frame selection
- playback snapshots and diagnostics
- internal sync worker wake/sleep logic
- seek recovery state and recovery diagnostics

The host currently owns:

- UI windowing
- presenting current video output
- input wiring
- host-side presentation delay estimation

## 6. Important Current Design Rules

### 6.1 Audio is the master clock

Video follows audio.

```text
target_video_time = audio_presentation_time + host_presentation_offset
```

### 6.2 The player now owns playback timing

This is the biggest current architectural truth.

The player no longer fundamentally depends on host polling cadence to decide when frames should advance.

The internal sync worker now drives progression.

### 6.3 The host still matters

The host is still responsible for the final presentation path.

That means:

- core sync correctness belongs to the player
- end-to-end visible timing still depends partly on host presentation behavior

The planned render boundary therefore is:

- the Rust core owns timed video surfaces
- the host owns final platform presentation of those surfaces

### 6.4 Concurrency is currently conservative

The player handle currently serializes mutable work through one operation lock.

That is not the final scaling design, but it is the current correctness boundary between:

- FFI commands
- sync worker activity
- runtime state mutation

## 7. Current Rust Module Direction

Current important areas:

```text
semi_player_rs/src/
  lib.rs                 C ABI shim
  api/                   public ABI-facing types and errors
  player/                runtime, sync, worker, scheduling, orchestration
    orchestrator.rs      control plane (open/play/pause/seek)
    handle.rs            aggregate root
    runtime.rs           runtime queues and current frame state
    execution/           decode supply, render supply, playback advance
    worker/              decode worker, sync worker, render worker
    diagnostics.rs       lock timing, seek instrumentation
    access.rs            media-open request assembly
    view.rs              read-only snapshots and FFI views
  decode/                decode policy, planner, backends, session
    policy.rs            decode preference and requirements
    output.rs            decoded output model
    decoder/             decoder open, pump, frame mapping, planner
    session/             media session lifecycle, decode loop, shared access
    video.rs             video decode backend and diagnostics
  demux/                 media probing, packet reads, seek positioning, keyframe
  render/                frame types, pipeline planning, backend execution
    core/                portable frame/surface/scheduling contracts
    pipelines/           render transformation strategies
    gpu/                 GPU device abstraction and D3D11 backend
      d3d11/             device, interop, renderer
    service.rs           player-owned render subsystem entry point
  audio/                 audio clock, output control, resampler, backend glue
  sync/                  audio clock, video sync, video scheduler, scheduling hints
  scheduler/             central scheduling decisions
  subtitle/              subtitle model and ASS parsing
  platform/              platform-specific capability providers
  util/                  shared helpers
```

## 8. Current Playback Lifecycle

Visible states today:

- `Idle`
- `Ready`
- `Playing`
- `Paused`

Internally, the important progression is:

```text
create
  -> open
  -> ready
  -> play
  -> internal worker advances playback
  -> pause / seek / reset
```

## 9. Current Threading Model

Today the threading model is:

```text
Host thread(s)
  -> FFI calls

Internal sync worker
  -> evaluates deadlines
  -> runs playback work when due

Shared player handle
  -> guarded by one serialized operation lock
```

This is already a meaningful step beyond the original host-pump prototype.

Still true, though:

- decode supply now has its own worker, but decode-result commit still shares the conservative serialized player mutation boundary
- audio output does not yet own a separate player-side controller thread
- render backend work is not yet split into its own backend-specific execution model

## 10. Current Playback Pipeline

```text
FFmpeg input
  -> demux
  -> decode
  -> normalized audio/video outputs
  -> runtime queues
  -> audio output backend
  -> audio clock
  -> video sync
  -> current video frame
  -> FFI frame read/copy
  -> host presentation
```

The internal sync worker currently drives:

- when decode/audio/video work should be revisited
- when stale video must be corrected immediately
- when audio refill should happen

The decode side now also contains the start of a target-aware seek-recovery path:

- seek installs a recovery target in player-owned state
- decode polling reads a recovery policy derived from that state
- FFmpeg-facing video decode can skip expensive BGRA conversion for frames that only exist to advance the decoder before the seek target

The next video-path step is to replace the "normalized BGRA bytes" assumption with a timed
surface model so hardware decode can lower seek and steady-state video cost without changing the
existing frame-timed scheduling semantics.

Current incremental state of that split:

- `DecodedVideoFrame` and `PresentationFrame` roles now exist in the codebase
- runtime video state now distinguishes:
  - decoded-video queue
  - presentation-video queue
  - current presentation frame
- decode output flows through an explicit render-service entry point
- the render service is functional:
  - decode output is queued as decoded video
  - render supply forwards frames through a render-core pipeline entry point
  - render supply passes an explicit render request into that pipeline
  - that request carries both pixel-format and presentation-surface preferences
  - higher-level presentation target profiles map host intent onto those preferences
  - render-core planning distinguishes passthrough, passthrough with subtitle intent, and requires transform
  - the D3D11 backend executes real NV12 to BGRA conversion via GPU staging + CPU swscale
  - the D3D11 backend also handles BGRA texture passthrough
- a render worker thread exists and runs render supply

The architecture boundary is now visible and functional in code. The next step is replacing
CPU swscale with `libplacebo` GPU-side NV12→BGRA conversion.

## 11. What Is Still Transitional

The current architecture is real, but not final.

Transitional parts:

- decode supply has been split logically from playback advancement, but still runs synchronously on the same execution lane
- NV12 to BGRA conversion currently uses GPU staging + CPU swscale; `libplacebo` GPU-side conversion is the next step
- CPU BGRA copy is still the main host frame-delivery path
- seek recovery is explicit and keyframe-anchored diagnostics are in place, but reset/rebuild cost and post-target recovery cost are still being reduced
- subtitles are not yet integrated into the same playback worker model
- one coarse lock still protects most mutable player state

## 12. Backend Strategy

### Audio

Current practical backend:

- `cpal`

This is treated as backend detail, not core architecture.

### Rendering

Rendering backend is now active with a D3D11 implementation.

Current state:

- D3D11VA hardware decode produces GPU NV12/P010 surfaces
- D3D11 render backend handles NV12→BGRA via GPU staging + CPU swscale
- render service with pipeline planning is functional
- BGRA texture passthrough works for already-decoded BGRA input

Long-term rule:

- render contracts should remain portable
- D3D11 must be an implementation, not the definition of the render subsystem

Near-term render rule:

- first real output backend is Windows D3D11
- the core should expose video surfaces, not WPF objects
- WPF is the first host adapter, not the render definition
- Avalonia should be able to reuse the same surface-oriented core contract later

## 13. Video Pipeline Direction

The video path should now be treated as three distinct responsibilities:

```text
decode
  ->
video render
  ->
platform presenter
```

This is an important refinement of the earlier "surface-oriented host contract" direction.

### 13.1 Decode is not presentation

The decode layer should output:

- timed decoded frames
- carrying decoder-native surfaces

Examples:

- `D3D11 NV12`
- `D3D11 P010`
- software YUV formats when hardware decode is unavailable

The decode layer should not be responsible for:

- WPF object creation
- Avalonia object creation
- final RGB presentation format
- subtitle composition

Its job is:

- get compressed video into a decoder-native surface with stable timing metadata

### 13.2 A dedicated video-render stage should own color conversion

The player should contain a real video-render stage between decode and host presentation.

That stage should own:

- YUV / hardware-native to RGB conversion
- scaling
- future subtitle / OSD composition
- final render-surface preparation for the active host backend

This means the player should not force the host to understand:

- `NV12`
- `P010`
- decoder-specific D3D11 surface semantics
- subtitle composition rules

Instead, the player should produce a host-consumable presentation frame.

Representative direction:

```text
DecodedVideoFrame
  -> pts / duration / dimensions
  -> DecoderSurface

PresentationFrame
  -> pts / duration / dimensions
  -> RenderSurface
```

Where:

- `DecoderSurface` is decoder-native storage
- `RenderSurface` is presentation-oriented storage

Current implementation note:

- these roles are introduced incrementally through `DecodedVideoFrame` and `PresentationFrame`
- they currently still share the same underlying frame storage type
- this is intentional so scheduling and seek behavior can stay stable while the render stage is
  carved out

### 13.2.1 Ownership model

The preferred ownership chain for the render stage is:

```text
player
  ->
render service
  ->
pipeline selection / orchestration
  ->
backend execution
```

That means:

- the player should own render as a subsystem
- render should own pipeline selection and long-lived render state
- render should own orchestration of multi-step frame rendering
- pipelines should express transformation strategy
- backends such as D3D11 should provide platform execution details

This is the preferred direction instead of a process-wide global renderer singleton.

Why:

- renderer state is more naturally tied to a player instance than to the whole process
- future multi-player or multi-device scenarios should not be forced through one shared global
  renderer
- `libplacebo` context, texture pools, and render resources fit render-instance ownership better
  than global ownership

The player/sync layer should still decide which playback time is current.
The render service should decide how that time becomes a final presentation frame, including future:

- video render
- subtitle render
- composition
- overlays / OSD
- other multi-step render work

Reference:

- [docs/dev/render-ownership.md](c:/y-s/project/Semi/docs/dev/render-ownership.md)

### 13.3 Host adapters should consume presentation frames, not decoder internals

The host adapter boundary should move toward:

- "give me the current presentation frame"

not:

- "here is the raw decoder surface, now the host must turn it into displayable RGB"

Why:

- WPF and Avalonia should not each reimplement video color conversion
- subtitle composition should stay inside player-owned timing/render rules
- decoder details should remain isolated from platform UI frameworks

Short-term practical rule:

- internal decode output may remain `D3D11 NV12`
- internal render output for Windows hosts should become a presentation-friendly RGB surface
- WPF should receive a frame/presenter contract that is already display-oriented

### 13.4 Subtitle placement

Subtitles should conceptually belong to the video-render stage, not the decode stage and not the
host shell.

Reason:

- subtitle timing follows the same playback timeline as video
- subtitle composition is part of how the final video image is produced
- host overlays can still exist as an implementation phase, but the architecture should reserve the
  long-term ownership for the player render pipeline

The near-term implementation can still phase this in conservatively:

1. decode to decoder-native surfaces
2. render to presentation-friendly RGB surfaces
3. later add subtitle composition into the same render stage

Preferred orchestration rule:

- player/sync chooses the playback time to render
- render service orchestrates subtitle render alongside video render
- a composition step combines the results into the final presentation frame

### 13.5 Current recommended internal split

The preferred internal direction is:

```text
core/media/
  decode-facing FFmpeg + hardware decode ownership

render/core/
  portable decoded-surface and presentation-surface contracts

render/service/
  player-owned render subsystem entry point
  pipeline selection
  render-context ownership
  multi-step render orchestration

render/pipelines/
  transformation policy
  subtitle/video composition policy

render/backends/d3d11/
  Windows video-render implementation
  - color conversion
  - scaling
  - future subtitle composition support

platform/host adapters
  WPF presenter
  future Avalonia presenter
```

The key rule is:

- decoder-native surfaces are an internal playback/render concern
- presentation-friendly frames are the handoff to host adapters
- render owns transformation policy; backend owns platform execution

Current near-term implementation plan:

1. keep the new decoded/presentation queue split in runtime
2. replace synchronous passthrough promotion with an explicit render service entry point
3. keep the first render service implementation synchronous internally
4. only then decide whether render needs its own worker lane

## 14. Public ABI Direction

The public ABI remains:

- handle-based
- command-oriented
- platform-neutral at the semantic layer

Important current ABI ideas:

- opaque player handle
- explicit result codes
- UTF-8 strings
- playback snapshot queries
- host presentation bias input

For the video path, the ABI direction should now distinguish between:

- decoder-internal surfaces
- host-visible presentation frames

The long-term ABI target should favor presentation-oriented contracts for normal host use, while
still allowing low-level diagnostics where needed.

## 15. Near-Term Architecture Priorities

The next architectural steps should focus on:

1. integrating `libplacebo` for GPU-native NV12/P010 to BGRA color conversion
2. replacing the CPU swscale conversion path with GPU render output
3. defining the presentation-oriented host ABI for GPU-native surfaces
4. building the first WPF GPU presenter adapter without CPU readback
5. integrating subtitle timing into the worker-owned playback/render model
6. reducing coarse locking where safe

Reference:

- [docs/dev/seek.md](c:/y-s/project/Semi/docs/dev/seek.md)

## 16. Summary

SemiPlayer should now be viewed as:

- a cross-platform Rust playback core
- already owning its internal playback timing
- moving toward a player-owned decode-to-render-to-present video pipeline
- still in transition from a shared synchronous execution lane to a fuller multi-service playback engine

Windows is the first verified implementation target.

It is not the architectural definition of the player.
