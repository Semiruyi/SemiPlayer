# SemiPlayer TODO

This file tracks the current implementation priorities for SemiPlayer.

Related documents:

- [ARCHITECTURE.md](c:/y-s/project/Semi/ARCHITECTURE.md)
- [docs/dev/pipeline.md](c:/y-s/project/Semi/docs/dev/pipeline.md)
- [docs/dev/seek.md](c:/y-s/project/Semi/docs/dev/seek.md)
- [docs/dev/sync.md](c:/y-s/project/Semi/docs/dev/sync.md)
- [docs/env/windows.md](c:/y-s/project/Semi/docs/env/windows.md)

## Current Snapshot

Already done:

- repository and third-party layout have been cleaned up
- root git repository is established
- Windows FFmpeg development baseline is verified
- `semi_player_rs` builds successfully
- `.NET` smoke host loads Rust and FFmpeg DLLs successfully
- media open / probe / decode path is real
- normalized media time is in microseconds
- audio output path exists through `cpal`
- audio clock uses backend playback timing when available
- video scheduler decisions exist
- presentation bias API exists
- `VideoSyncService` owns core video sync decisions
- player-owned sync worker is active
- player-owned decode worker is active
- playback advancement now executes in phased lock-in / lock-out / lock-in form
- decode polling now runs outside the main player lock and applies results back under generation guards
- decode-to-sync wake behavior has started tightening to avoid unnecessary sync wakeups on steady audio refill
- FFI and worker mutations are serialized through the player handle
- current seek diagnostics are strong enough to justify starting the video hardware-decode track
- D3D11VA hardware video decode is live with NV12/P010 GPU surface output
- D3D11 render backend split into device, interop, and renderer submodules
- render service with pipeline planning distinguishes passthrough vs transform
- NV12 decoded frames are copied to player-owned GPU textures before entering runtime
- NV12 to BGRA conversion works via GPU staging + CPU swscale path
- render worker thread exists and runs render supply
- `VideoSurface` abstraction supports both `GpuTexture` and `CpuPacked` storage
- decode output flows through explicit render-service entry point before entering presentation queue

Not done yet:

- lock-independent decode pipeline beyond the shared player handle lock
- GPU-native NV12 to BGRA conversion (currently GPU staging + CPU swscale; next step is libplacebo GPU render)
- presentation-oriented host ABI for GPU-native presentation surfaces
- WPF presenter adapter for presentation-friendly GPU video frames without CPU readback
- subtitle pipeline and libass integration
- real host adapter projects beyond the smoke app
- finer-grained worker/locking model
- cross-platform backend validation

## Priority Labels

- `P0`: current architecture stabilization
- `P1`: output/backend completion
- `P2`: subtitles and host integration
- `P3`: quality, portability, and cleanup

## P0 - Stabilize The Current Worker Architecture

### P0.1 Measure worker-driven sync directly

Status: baseline done, keep for regression tracking

Tasks:

- keep worker-vs-UI-driver comparison in smoke tooling healthy
- keep measuring:
  - `CoreSyncErr` mean
  - absolute mean
  - positive spikes
  - sensitivity to host polling cadence
- keep a repeatable comparison path for regressions

### P0.2 Split decode supply from shared playback advancement flow

Status: major baseline done, deeper concurrency split still pending

Tasks:

- keep decode supply separated from playback advancement at the code-path level
- continue reducing decode worker dependence on the shared player handle commit path
- keep tightening how decoded-frame enqueue decides whether the sync worker really needs a wake

Why this matters:

- decode now has its own worker lane, but it still shares the same serialized player lock

### P0.3 Tighten sync worker wake policy

Status: active tuning, first wake reductions landed

Tasks:

- review stale-video immediate wake rules
- review audio-start / audio-refill immediate wake rules
- keep pure audio refill from waking sync work unless it changes playback readiness
- reduce unnecessary wake churn without reintroducing drift
- validate wake-policy changes against smoke diagnostics and pause/seek behavior

### P0.4 Reduce coarse lock scope

Status: stage behind wake/seek work, but partly unblocked

Tasks:

- first focus on seek-related hot paths before broader lock splitting
- identify hot paths currently blocked by the single handle operation lock
- keep decode refill packet-budgeted while deeper lock splitting is pending
- move playback-side audio output work onto the new shared audio-output boundary
- split read-mostly and write-heavy responsibilities where safe
- preserve correctness first

### P0.5 Improve seek responsiveness and seek-path cost

Status: software seek-path triage mostly complete; next major step is video hardware decode

Tasks:

- keep seek result metrics healthy:
  - API seek duration
  - first video frame latency
  - first audible audio latency
  - stable post-seek settle time
- keep internal seek stage timing metrics healthy:
  - lock wait
  - FFmpeg seek
  - immediate reset
  - decode-to-first-video
  - decode-to-first-audio
  - target-video-ready
  - target-audio-ready
  - stable settle
- separate seek correctness from seek speed so regressions are visible
- start with core-internal observability before adding end-to-end host timing
- maintain the documented seek path and target seek-recovery model explicitly
- keep the performance-first keyframe-anchored seek strategy as the default local-playback baseline
- keep seek recovery as a dedicated path instead of treating seek as a plain full reset + refill
- reduce work done while holding the shared player handle during seek
- review which state must be cleared immediately vs lazily rebuilt after seek
- avoid unnecessary wake storms or duplicate refill work right after seek
- define a practical short-term seek target for local files:
  - fast first-frame response after keyboard/progress-bar seek
  - stable A/V resettling shortly after
- keep the expected-left-keyframe diagnostics path healthy so actual-vs-expected anchor regressions stay visible
- keep refining which pre-target video frames can bypass expensive post-processing during seek recovery
- continue reducing pre-target audio work before full playback-grade conversion
- add explicit reset/rebuild work-accounting metrics:
  - runtime clear
  - audio backend clear
  - audio restart timing
  - first post-target current-video timing
- keep the current measured conclusion explicit:
  - FFmpeg anchor placement is correct on tested local samples
  - reset is not a meaningful seek bottleneck
  - audio recovery is not a meaningful seek bottleneck
  - demux/read-packet cost is not a meaningful seek bottleneck
  - the dominant seek cost is forward video recovery from the left keyframe
- keep software-side seek follow-ups narrowly scoped:
  - review whether recovery-time video frame mapping/copy can be reduced further
  - trim seek diagnostics down to long-term useful fields once hardware-decode work starts
- start the next major seek-performance track:
  - design and integrate video hardware decode for the playing-seek path
  - preserve the current keyframe-anchored recovery semantics while swapping the heavy video decode backend
- defer continuity-seek / buffered-seek complexity unless hardware decode still leaves playing seek unsatisfactory

Why this matters:

- seek responsiveness is part of the core player feel
- poor seek behavior will be much more visible to users than many backend details
- seek touches decode, runtime reset, audio output, and sync wake policy together, so it is worth treating as a first-class performance track

Current conclusion:

- the remaining dominant seek cost is video soft-decode recovery itself
- future seek wins are therefore more likely to come from the video decode backend than from more seek-specific control-flow complexity

## P1 - Real Output and Backend Boundaries

### P1.1 Define render output surface abstraction

Status: done

Completed:

- timed video-frame metadata is split from pixel/surface storage in `VideoFrame` / `VideoSurface`
- portable render surface concepts exist in `render/core/`
- `VideoSurfaceStorage` supports `GpuTexture` and `CpuPacked` variants
- `PixelFormatCategory` covers NV12, P010, Bgra8, and others
- runtime scheduling is based on timed frames, not a naked "latest texture"

### P1.2 Refit the current software path onto the new surface model

Status: done

Completed:

- the current software decode path works under the `VideoSurface` model
- `semi_player_copy_current_video_frame_bgra(...)` exists as a compatibility/debug path
- BGRA copy-out is limited to CPU-backed surfaces
- sync, seek-recovery, and drop/present scheduling work with the new frame type

### P1.3 Implement first real Windows video backend

Status: hardware decode and GPU surface delivery working; next step is GPU-native color conversion

Completed:

- `render/gpu/d3d11/` is established with device, interop, and renderer submodules
- D3D11 device and device context creation work
- FFmpeg D3D11VA hardware decode is configured and active
- decoder-native GPU video surfaces are produced (NV12, P010)
- software decode fallback works for unsupported media/devices
- backend details stay behind the `RenderBackend` trait
- interop module wraps D3D11 device for FFmpeg and copies frames to owned textures

Remaining for GPU-native conversion:

- integrate `libplacebo` for GPU-side NV12/P010 to BGRA conversion
- replace the current GPU staging + CPU swscale path with a GPU render path
- keep CPU BGRA as fallback

### P1.4 Add a real player-owned video-render stage

Status: render service and pipeline planning are landed and functional; current NV12 to BGRA conversion uses GPU staging + CPU swscale

Completed:

- `DecodedVideoFrame` and `PresentationFrame` type roles exist in the codebase
- runtime scheduling and sync are centered on presentation frames
- decode-buffer accounting is distinct from presentation-ready buffer accounting
- frame transformation responsibility is in render supply and render service
- render-context inputs (output preference, subtitle visibility) route through the pipeline
- presentation-surface policy routes through the pipeline
- presentation target profiles exist (e.g., `gpu_bgra_presenter`, `cpu_bgra_copy`)
- render-core planning distinguishes passthrough, passthrough with subtitle intent, and requires transform
- render-stage passthrough-vs-transform demand is visible in diagnostics
- presentation target selection is player-owned
- the D3D11 render path handles:
  - decoder-native NV12 input from D3D11VA
  - BGRA output via GPU staging texture + CPU swscale conversion
- ownership is explicit:
  - player owns render
  - render owns pipeline selection and render-context state
  - pipelines use backend execution

Remaining:

- replace CPU swscale with `libplacebo` GPU NV12→BGRA conversion
- output D3D11 BGRA presentation surfaces that stay on GPU until host readback or direct presentation
- follow the integration plan documented in:
  - [docs/dev/d3d11-libplacebo-render.md](c:/y-s/project/Semi/docs/dev/d3d11-libplacebo-render.md)
  - [docs/dev/d3d11-libplacebo-integration-plan.md](c:/y-s/project/Semi/docs/dev/d3d11-libplacebo-integration-plan.md)

### P1.5 Define the presentation-oriented host ABI

Tasks:

- add ABI-visible presentation-frame / render-surface descriptors
- add explicit acquire/release rules for host-visible presentation surfaces
- keep raw decoder-surface exposure diagnostic-first, not the default host contract
- keep host contracts presentation-oriented instead of WPF-object-oriented
- make room for both:
  - CPU compatibility read path
  - GPU-native host presentation path

Near-term note:

- current host-visible frame metadata still comes from the presentation side of runtime
- low-level decoder-surface diagnostics can stay available while presentation-oriented ABI grows

### P1.6 Clarify host adapter boundary

Tasks:

- keep smoke app diagnostic-only
- define what belongs in:
  - interop layer
  - WPF adapter
  - future Avalonia adapter
- keep video color conversion and future subtitle composition in the player render stage
- treat WPF as the first presenter adapter, not as the render definition

### P1.7 Deliver the first WPF GPU presentation path

Tasks:

- build the first WPF-facing adapter on top of the presentation-oriented ABI
- present player-rendered GPU video without requiring GPU readback
- keep WPF-specific interop details out of the portable playback core

## P2 - Subtitle and Host Integration

### P2.1 Define subtitle domain model

Tasks:

- track selection
- visibility
- delay / offset
- embedded vs external subtitle source
- keep subtitle timing independent from decoded video surfaces
- make subtitle state consumable by a future render-service subtitle pipeline

### P2.2 Integrate libass

Tasks:

- establish binding strategy
- support text subtitle layout/rasterization
- connect subtitle timing to the player timeline
- define subtitle-render outputs that the render service can compose with video outputs

### P2.3 Unify subtitle timing with worker-owned playback

Tasks:

- evaluate subtitle events against the same master timeline
- make subtitle timing react correctly to:
  - play / pause
  - seek
  - speed
  - host presentation bias rules where relevant
- keep subtitle composition out of decode output itself
- first allow a transitional overlay path, then fold subtitle composition into the player render stage
- make the long-term orchestration target explicit:
  - video pipeline and subtitle pipeline may prepare work independently
  - render service owns composition ordering and synchronization

## P3 - Quality and Portability

### P3.1 Speed control beyond timing state

Tasks:

- move toward real audio speed control / time-stretch
- keep pitch-correct playback as the long-term target

### P3.2 Better diagnostics surface

Tasks:

- keep sync-worker and decode-worker contention visible separately
- expose richer worker diagnostics if needed
- keep smoke and automated measurement paths aligned

### P3.3 Avalonia adapter

Tasks:

- validate that the host contract works outside WPF
- keep the Rust core unchanged

### P3.4 macOS compile baseline

Tasks:

- get `semi_player_rs` compiling cleanly on macOS
- isolate Windows-shaped assumptions early

## Cross-Cutting Rules

### C1. Keep docs aligned

Whenever these change:

- worker ownership
- sync contract
- output surface model
- host responsibilities

update:

- [ARCHITECTURE.md](c:/y-s/project/Semi/ARCHITECTURE.md)
- [docs/dev/pipeline.md](c:/y-s/project/Semi/docs/dev/pipeline.md)
- [docs/dev/sync.md](c:/y-s/project/Semi/docs/dev/sync.md)
- [TODO.md](c:/y-s/project/Semi/TODO.md)

### C2. Keep the smoke host diagnostic-first

Rule:

- smoke host is for wiring, debugging, and measurement
- smoke host is not the final application architecture

### C3. Keep platform details behind boundaries

Rule:

- Windows-only details belong in backend / platform layers
- not in the core playback semantics

## Recommended Next Sequence

Do these next, in order:

1. keep worker-vs-host sync measurement as a regression tool
2. finish the current round of sync/decode wake-policy tightening
3. improve seek responsiveness and reduce seek-path cost
4. integrate `libplacebo` for GPU-native NV12/P010 to BGRA conversion (replacing CPU swscale path)
5. pin D3D11 texture lifetime and ownership rules for `libplacebo` integration
6. choose and document the Windows `libplacebo` acquisition/link strategy
7. implement the first narrow `D3D11 NV12 -> D3D11 BGRA` GPU render path
8. define presentation-oriented host ABI for GPU-native presentation surfaces
9. build the first WPF GPU presenter adapter
10. reduce seek-related coupling to the shared player lock
11. integrate subtitle timing into the worker-owned playback/render model
