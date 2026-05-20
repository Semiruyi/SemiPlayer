# Semi Player Refactor Layers

## Goal

Make the main playback path explicit as five layers:

1. Demux
2. Decode
3. Render
4. AV Sync
5. Player Orchestration

This file records the current file-to-layer mapping so future refactors can keep moving in one direction.

## File To Layer

| File | Layer | Notes |
| --- | --- | --- |
| `src/demux/probe.rs` | Demux | Media probing and stream info collection. |
| `src/demux/diagnostics.rs` | Demux | Seek-time demux diagnostics. |
| `src/demux/keyframe.rs` | Demux | Keyframe probing helpers. |
| `src/decode/session/mod.rs` | Session Facade | Media session facade and public session methods. |
| `src/decode/session/decode.rs` | Decode | Session-local decode loop state machine. |
| `src/decode/session/lifecycle.rs` | Demux / Session Lifecycle | Open/seek lifecycle and session construction flow. |
| `src/decode/session/shared.rs` | Session Sharing | Shared session wrapper and lock-based access facade. |
| `src/decode/decoder.rs` | Decode | Decoder open, packet send, frame receive, seek-recovery frame skipping. |
| `src/decode/output.rs` | Decode | Decoded output model and decode policy. |
| `src/decode/video.rs` | Decode | Video decode backend and diagnostics. |
| `src/render/core/frame.rs` | Render | Video frame and surface abstractions. |
| `src/render/core/pipeline.rs` | Render | Render target negotiation and transform planning. |
| `src/render/service.rs` | Render | Render service orchestration. |
| `src/render/gpu/mod.rs` | Render | GPU device abstraction. |
| `src/render/gpu/d3d11.rs` | Render | D3D11 device implementation. |
| `src/render/pipelines/cpu_bgra.rs` | Render | CPU BGRA render pipeline. |
| `src/render/pipelines/mod.rs` | Render | Render pipeline entry. |
| `src/sync/clock.rs` | AV Sync | Audio clock. |
| `src/sync/video_scheduler.rs` | AV Sync | Video scheduling decisions. |
| `src/sync/video_sync.rs` | AV Sync | Audio/video sync decisions. |
| `src/sync/schedule.rs` | AV Sync | Pump/decode scheduling hints. |
| `src/player/orchestrator.rs` | Player Orchestration | Open/play/pause/seek/reset and playback setting orchestration. |
| `src/player/diagnostics.rs` | Player Orchestration | Player diagnostics state, lock-wait metrics, and seek instrumentation. |
| `src/player/handle.rs` | Player Orchestration | Aggregate root that owns runtime, session, workers, and sync state. |
| `src/player/runtime.rs` | Player Orchestration | Runtime queues, current frame slots, EOS state. |
| `src/player/execution/decode_supply.rs` | Player Orchestration | Decode polling entry for worker-side supply. |
| `src/player/execution/decoded_output_apply.rs` | Player Orchestration | Applies decoded outputs into runtime and wake-policy facing results. |
| `src/player/execution/render_supply.rs` | Player Orchestration | Turns decoded video frames into presentable frames. |
| `src/player/execution/playback_advance.rs` | Player Orchestration | Advances playback state and current-frame promotion. |
| `src/player/execution/mod.rs` | Player Orchestration | Execution facade. |
| `src/player/worker/decode.rs` | Player Orchestration | Background decode worker implementation. |
| `src/player/worker/sync.rs` | Player Orchestration | Background sync worker implementation. |
| `src/player/worker/mod.rs` | Player Orchestration | Worker facade. |
| `src/player/pump.rs` | Player Orchestration | Synchronous pump entry. |
| `src/player/view.rs` | Player Orchestration | Read-only snapshot and FFI view builders. |
| `src/player/mod.rs` | Player Orchestration | Player module entry. |
| `src/decode/mod.rs` | Module Facade | Decode module entry and session facade exports. |
| `src/demux/mod.rs` | Module Facade | Demux module entry. |
| `src/render/mod.rs` | Module Facade | Render module entry. |
| `src/audio/mod.rs` | Module Facade | Audio module entry. |
| `src/sync/mod.rs` | Module Facade | Sync module entry. |
| `src/subtitle/mod.rs` | Module Facade | Subtitle module entry. |
| `src/platform/mod.rs` | Module Facade | Platform module entry. |
| `src/api/mod.rs` | Module Facade | API module entry. |
| `src/util/mod.rs` | Module Facade | Utility module entry. |
| `src/lib.rs` | FFI Facade | External API entrypoint. Should stay thin and mostly forward into player/media modules. |
| `src/api/error.rs` | API Support | FFI error codes. |
| `src/api/types.rs` | API Support | FFI structs and enums. |
| `src/audio/core/frame.rs` | Audio Support | Audio frame model. |
| `src/audio/core/output.rs` | Audio Support | Audio output format model. |
| `src/audio/core/output_controller.rs` | Audio Support | Audio device/output control. |
| `src/audio/core/resampler.rs` | Audio Support | Audio resampling. |
| `src/audio/backends.rs` | Audio Support | Backend timing and playback simulation. |
| `src/subtitle/ass.rs` | Subtitle Support | Subtitle parsing/render input. |
| `src/subtitle/model.rs` | Subtitle Support | Subtitle data model. |
| `src/util/time.rs` | Common Support | Time conversion helpers. |

## Current Boundary Notes

- The media session area now has a real directory boundary:
  - `src/decode/session/mod.rs`
  - `src/decode/session/decode.rs`
  - `src/decode/session/lifecycle.rs`
  - `src/decode/session/shared.rs`
- The demux and decode areas now also have physical directory boundaries:
  - `src/demux/*`
  - `src/decode/*`
- That is a structural improvement, and it makes the next decode-side hotspot more clearly `src/decode/decoder.rs`.

- `src/lib.rs` is getting thinner.
  - Playback control has moved into `src/player/orchestrator.rs`.
  - Snapshot and query shaping has moved into `src/player/view.rs`.
  - Media-open backend wiring has moved below FFI; the FFI open path now delegates into player-owned open request assembly.

- The decode open path now has an explicit request contract:
  - `src/decode/session/mod.rs` owns `MediaOpenRequest`
  - `src/player/access.rs` builds player-side `MediaOpenContext`
  - `src/decode/session/lifecycle.rs` now treats request-shaped open as the internal primary path

- The decoded-output apply path now has a real boundary:
  - `src/player/execution/decode_supply.rs` focuses on decode polling
  - `src/player/execution/decoded_output_apply.rs` owns decoded-output application policy
  - `src/render/service.rs` now owns decoded-frame preparation for runtime-facing render usage

- The current `player` directory can now be read roughly as:
  - `orchestrator`: control plane
  - `diagnostics`: diagnostics and seek instrumentation plane
  - `execution`: execution plane
  - `worker`: background worker plane
  - `runtime`: runtime state plane
  - `view`: read/query plane

## Coupling Hotspots

These files are the current cross-layer hotspots. They are not all wrong, but they are the places where responsibilities still bleed across boundaries.

| File | Current Main Home | Cross-Layer Coupling | Suggested Direction |
| --- | --- | --- | --- |
| `src/lib.rs` | FFI Facade | Still knows media open error mapping and FFI view output details, but no longer assembles backend-specific media-open wiring. | Keep only pointer checks, C conversions, and forwarding. Move more error mapping and API shaping behind `api` or `player` facades. |
| `src/player/handle.rs` | Player Orchestration | Aggregates runtime, sync, render, audio output, media session access, and worker lifecycle. | Keep as aggregate root, but continue pushing behavior into `orchestrator`, `view`, `worker`, and smaller helper modules. |
| `src/player/diagnostics.rs` | Player Orchestration | Centralizes player lock timing, stale-audio discard stats, render counters, and seek instrumentation. | Good new home; later it could split further if generic metrics and seek telemetry diverge. |
| `src/player/execution/playback_advance.rs` | Player Orchestration | Mixes runtime queue advancement, audio device submission, sync ticking, and output started-state transitions. | Candidate split into playback state advancement vs audio device submission coordination. |
| `src/player/execution/decode_supply.rs` | Player Orchestration | Now mostly focused on decode polling budget and decode worker fairness. | Likely close to stable unless decode worker planning changes again. |
| `src/player/execution/decoded_output_apply.rs` | Player Orchestration | Owns decoded-output application, audio seek trim policy, runtime enqueue, and wake-result shaping. | Good new seam; later decoded video preparation policy may move further toward a render/decode bridge contract. |
| `src/sync/schedule.rs` | AV Sync | Reads player runtime, audio output snapshot, and video sync state to drive both pump and decode timing. | Likely stays in AV sync, but the player-facing schedule input model could be narrowed. |
| `src/decode/session/mod.rs` | Session Facade | Still owns the concrete session state layout while forwarding into decode and lifecycle helpers. | Keep shrinking until it is mostly a shell plus narrow public methods. |
| `src/decode/session/lifecycle.rs` | Demux / Session Lifecycle | Ties ffmpeg open/seek flow to session construction and decoder lifecycle reset. | Likely the right home for now; later it may split into open vs seek helpers. |
| `src/decode/session/shared.rs` | Session Sharing | Shared session lock wrapper plus shared query forwarding. | Likely stable now. |
| `src/decode/decoder.rs` | Decode | Owns software decode, hardware decode bootstrap, frame mapping, and color/pixel translation into render-facing frame models. | Biggest decode-side mixed file; likely future split into decoder open, packet/frame loop helpers, and frame mapping. |
| `src/demux/keyframe.rs` | Demux | Reopens ffmpeg input and does seek probing outside the main media session. | Probably stays Demux, but could join a dedicated seek-probe submodule with session seek helpers. |
| `src/render/gpu/d3d11.rs` | Render | Now mostly acts as a backend-local facade over split device/interop/renderer files. | Likely stable unless a broader backend directory rename happens. |
| `src/audio/core/output_controller.rs` | Audio Support | Mixes audio backend control, buffer accounting, playback timing snapshots, and started-state policy. | Candidate split into device queue state vs timing/reporting facade if audio is refactored further. |

## Current Best-Fit Ownership

These are the most important files where the current physical file and the long-term conceptual owner are not yet perfectly aligned.

| File | Best-Fit Owner |
| --- | --- |
| `src/lib.rs` | `api` + thin FFI facade |
| `src/player/handle.rs` | player aggregate root only, with behavior continuing to move outward |
| `src/player/execution/playback_advance.rs` | player execution, but with a likely future `audio_submit` helper |
| `src/player/execution/decode_supply.rs` | player execution decode polling shell |
| `src/player/execution/decoded_output_apply.rs` | player execution policy seam that may later narrow into runtime-commit and wake-result helpers |
| `src/decode/decoder.rs` | decode core facade over open/pump/map/planner internals |
| `src/render/gpu/d3d11.rs` | render gpu backend facade over `device` + `interop` + `renderer` |
| `src/audio/core/output_controller.rs` | audio output orchestration, but likely split into `device queue` + `timing/snapshot` |

## Next Refactor Targets

1. Keep thinning `src/lib.rs` until it behaves like a pure FFI facade, especially around error mapping and host-view shaping.
2. Keep shrinking `src/player/handle.rs` by moving request assembly and orchestration helpers into `player/access` and `player/orchestrator`.
3. Consider aligning `MediaSession::from_input*` around the same request-shaped open language as `MediaOpenRequest`.
4. If needed, split `audio` more explicitly into device output, audio frame model, resampling, and clock collaboration.
