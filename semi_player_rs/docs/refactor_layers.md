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
| `src/core/media/probe.rs` | Demux | Media probing and stream info collection. |
| `src/core/media/demux.rs` | Demux | Seek-time demux diagnostics. |
| `src/core/media/keyframe_probe.rs` | Demux | Keyframe probing helpers. |
| `src/core/media/session.rs` | Demux / Decode | Media session driver. Still owns both input state and decoder state, so it remains the main mixed-responsibility target. |
| `src/core/media/decoder.rs` | Decode | Decoder open, packet send, frame receive, seek-recovery frame skipping. |
| `src/core/media/output.rs` | Decode | Decoded output model and decode policy. |
| `src/core/media/video_decode.rs` | Decode | Video decode backend and diagnostics. |
| `src/render/core/frame.rs` | Render | Video frame and surface abstractions. |
| `src/render/core/pipeline.rs` | Render | Render target negotiation and transform planning. |
| `src/render/service.rs` | Render | Render service orchestration. |
| `src/render/gpu.rs` | Render | GPU device abstraction. |
| `src/render/gpu/d3d11.rs` | Render | D3D11 device implementation. |
| `src/render/pipelines/cpu_bgra.rs` | Render | CPU BGRA render pipeline. |
| `src/render/pipelines.rs` | Render | Render pipeline entry. |
| `src/sync/clock.rs` | AV Sync | Audio clock. |
| `src/sync/video_scheduler.rs` | AV Sync | Video scheduling decisions. |
| `src/sync/video_sync.rs` | AV Sync | Audio/video sync decisions. |
| `src/sync/schedule.rs` | AV Sync | Pump/decode scheduling hints. |
| `src/core/player/orchestrator.rs` | Player Orchestration | Open/play/pause/seek/reset and playback setting orchestration. |
| `src/core/player/handle.rs` | Player Orchestration | Aggregate root that owns runtime, session, workers, and sync state. |
| `src/core/player/runtime.rs` | Player Orchestration | Runtime queues, current frame slots, EOS state. |
| `src/core/player/execution/decode_supply.rs` | Player Orchestration | Drives decode supply and applies decoded outputs into runtime. |
| `src/core/player/execution/render_supply.rs` | Player Orchestration | Turns decoded video frames into presentable frames. |
| `src/core/player/execution/playback_advance.rs` | Player Orchestration | Advances playback state and current-frame promotion. |
| `src/core/player/execution.rs` | Player Orchestration | Execution facade. |
| `src/core/player/decode_worker.rs` | Player Orchestration | Background decode worker implementation. |
| `src/core/player/sync_worker.rs` | Player Orchestration | Background sync worker implementation. |
| `src/core/player/worker.rs` | Player Orchestration | Worker facade. |
| `src/core/player/pump.rs` | Player Orchestration | Synchronous pump entry. |
| `src/core/player/view.rs` | Player Orchestration | Read-only snapshot and FFI view builders. |
| `src/core/player.rs` | Player Orchestration | Player module entry. |
| `src/core/media.rs` | Module Facade | Media facade, not a direct business layer by itself. |
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

- `src/core/media/session.rs` is still the biggest mixed-responsibility file on the media side.
  - The `demux / decode / session` facade split is already in place.
  - The next natural step is to keep separating driver flow from long-lived session state.

- `src/lib.rs` is getting thinner.
  - Playback control has moved into `src/core/player/orchestrator.rs`.
  - Snapshot and query shaping has moved into `src/core/player/view.rs`.

- The current `player` directory can now be read roughly as:
  - `orchestrator`: control plane
  - `execution`: execution plane
  - `worker`: background worker plane
  - `runtime`: runtime state plane
  - `view`: read/query plane

## Next Refactor Targets

1. Keep shrinking `src/core/media/session.rs`.
2. Keep thinning `src/lib.rs` until it behaves like a pure FFI facade.
3. If needed, split `audio` more explicitly into device output, audio frame model, resampling, and clock collaboration.
