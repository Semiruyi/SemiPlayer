# Player Locking Model

This document defines the locking model for `semi_player_rs/src/player`.

## 1. Design Goal

Replace "lock the player" with "lock the resources required by the operation".

Each resource domain owns its own lock. Callers go through the access layer
(`src/player/access.rs`) and never compose arbitrary lock combinations by hand.

## 2. Current Shape

`SemiPlayerHandle` has no outer mutex. Each domain is independently locked:

| Domain | Contents | Protection |
| --- | --- | --- |
| `control` | `state`, `speed`, `subtitles_visible`, `video_presentation_profile`, `host_presentation_offset_us`, `seek_recovery` | `Mutex<ControlState>` |
| `runtime` | `PlayerRuntime`, `VideoScheduler`, `VideoSyncState` | `Mutex<RuntimeDomain>` |
| `media` | `media_session` | `RwLock<Option<SharedMediaSession>>` |
| `audio_coord` | `audio_clock`, `audio_output` | `AudioClock` atomics + `SharedAudioOutputController` internal lock |
| `render` | `RenderService` | `Mutex<RenderService>` |
| `diagnostics` | player metrics and seek telemetry | `PlayerDiagnostics` atomics |
| `playback_phase` | operation-level seek/reset/load coordination fence | `Arc<Mutex<()>>` |
| `worker_control` | sync/render/decode worker wake + shutdown state | worker-internal `Condvar` |

## 3. Design Rules

- No blanket `&mut SemiPlayerHandle` to any caller.
- No arbitrary lock composition. All access goes through the access layer.
- Lock order is fixed: `playback_phase -> control -> runtime -> media`.
- Heavy work (FFmpeg decode, render transform) stays outside locks whenever possible.
- Diagnostics are readable without waiting on playback work.
- Plan/execute/commit pattern for operations that touch multiple domains.
- D3D11 device/context access is additionally protected by `ID3D11Multithread`; this is separate
  from player-domain locks and is required because FFmpeg hardware decode and render readback share
  the same D3D11 objects across threads.

## 4. Threads And Resources

### 4.1 Threads

| Thread | Main Entry | Responsibility |
| --- | --- | --- |
| Host / FFI thread | `semi_player_*` in `src/lib.rs` | Control operations and synchronous queries |
| Sync worker | `src/player/worker/sync.rs` | Playback scheduling, audio submission, video sync advancement |
| Render worker | `src/player/worker/render.rs` | Render scheduling, decoded-to-presentation transformation, decode-demand coordination |
| Decode worker | `src/player/worker/decode.rs` | Decode scheduling, media polling, decoded-output application |
| Audio backend thread | internal to audio backend | Device timing progression; does not directly lock `SemiPlayerHandle` |

### 4.2 Thread To Resource Matrix

Legend: `R` read, `W` write, `X` heavy execution, `-` no direct access.

| Thread | control | runtime | media | audio_coord | render | diagnostics | worker_control |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Host / FFI thread | `R/W` | `R/W` | `R/W/X` | `R/W` | `-` | `R` | `-` |
| Sync worker | `R` | `R/W` | `-` | `R/W/X` | `-` | `W` | `W` |
| Render worker | `R` | `R/W` | `-` | `-` | `R/W/X` | `W` | `W` |
| Decode worker | `R` | `R/W` | `R/W/X` | `R` | `-` | `W` | `W` |
| Audio backend thread | `-` | `-` | `-` | internal only | `-` | `-` | `-` |

## 5. Access Layer

All domain access goes through `src/player/access.rs`.

### 5.1 Control

- `control_access()`: `ControlAccess` wrapper with read/write methods
- `control_snapshot()`: cheap `Copy` snapshot of all control fields

### 5.2 Runtime

- `with_runtime_access(f)`: takes `Mutex<RuntimeDomain>`, yields `RuntimeAccess`
- `runtime_snapshot()`: cheap `Copy` snapshot of runtime state
- `video_sync_dirty_snapshot()`, `video_sync_stats_snapshot()`: narrow runtime reads

### 5.3 Audio Coordination

- `audio_coord_access()`: `AudioCoordAccess` wrapper for clock + output

### 5.4 Render

- `with_render_access_mut(f)`: takes `Mutex<RenderService>`, yields `RenderAccess`

### 5.5 Media

- `cloned_media_session()`: clones `Arc` to `SharedMediaSession`
- `with_media_session(f)` / `with_media_session_mut(f)`: scoped read/write access

### 5.6 Playback Phase

- `playback_phase_handle()`: returns `Arc<Mutex<()>>` for seek/reset/load fencing

### 5.7 Composite Contexts

Higher-level operations build composite contexts from the above primitives:

- `decode_plan_context()`: control + runtime + media
- `decode_schedule_inputs()`: control + runtime + video sync
- `render_plan_context()`: control + runtime + render
- `schedule_inputs()`: control + runtime + video sync + audio
- `sync_worker_plan_context()`: control + schedule + phase lock
- `playback_advance_plan_context()`: control + runtime + audio
- `playback_snapshot_inputs()`: all domain snapshots for UI
- `seek_prepare_context()` / `seek_commit_context()`: seek-phase data

## 6. Thread Phase Resource Tables

Legend: `R` read, `W` write, `X` heavy execution.

### 6.1 Host / FFI Thread

| Phase | Entrypoints | Resources | Notes |
| --- | --- | --- | --- |
| Query | `get_state`, `get_position`, `get_duration`, `get_media_info`, `get_playback_snapshot`, current-frame queries | `control (R)`, `runtime (R)`, `audio_coord (R)`, `media (R)` | No `playback_phase` needed |
| Control | `play`, `pause`, `set_speed`, `set_subtitle_visible`, `set_video_presentation_profile`, `set_video_presentation_bias_ms` | `control (W)`, `audio_coord (W)`, `runtime (W)` | Short state transitions |
| Seek | `seek`, `seek_prev_keyframe`, `seek_next_keyframe` | `playback_phase`, `control (R/W)`, `media (X)`, `runtime (W)`, `audio_coord (W)` | `execute_seek` is atomic: media seek + runtime reset + clock reset in one step |

### 6.2 Sync Worker

| Phase | Code | Resources | Notes |
| --- | --- | --- | --- |
| Wait | `wait_for_signal(...)` | `worker_control (W)` | Thread parking only |
| Evaluate | `evaluate_worker_action(...)` | `control (R)`, `runtime (R)`, `audio_coord (R)` | Read-only, decides playback work from presentation-ready state; does not inspect decode backlog directly |
| Playback Plan | `plan_playback_advance(...)` | `control (R)`, `runtime (R/W)`, `audio_coord (R)` | Pulls audio chunks under runtime lock |
| Playback Execute | `execute_playback_plan(...)` | `audio_coord (X/W)` | Audio backend submission; no player lock |
| Playback Commit | `finish_playback_advance(...)` | `runtime (W)`, `audio_coord (R/W)`, `control (R/W)`, `diagnostics (W)` | Video sync tick + seek recovery + diagnostics; wakes render when presentation supply needs attention |

### 6.3 Render Worker

| Phase | Code | Resources | Notes |
| --- | --- | --- | --- |
| Wait | `wait_for_signal(...)` | `worker_control (W)` | Thread parking only |
| Evaluate | `plan_render_action(...)` | `control (R)`, `runtime (R)` | Reads presentation demand and decoded backlog; may request decode |
| Render Stage | `begin_render_stage(...)` | `runtime (W)` | Moves one decoded batch into in-flight staging under generation tracking |
| Render Execute | `execute_render_supply(...)` | `render (X/W)` | Runs render pipeline outside runtime lock |
| Render Commit | `commit_render_supply(...)` | `runtime (R/W)`, `diagnostics (W)` | Commits presentation frames or drops stale generation work; wakes sync on new presentation frames |

### 6.4 Decode Worker

| Phase | Code | Resources | Notes |
| --- | --- | --- | --- |
| Wait | `wait_for_signal(...)` | `worker_control (W)` | Thread parking only |
| Decode Plan | `plan_decode_action(...)` | `control (R)`, `runtime (R)`, `media (R)` | Read-only snapshot, no lock held across the phase |
| Decode Execute | `poll_decoded_output_once(...)` | `media (X)` | FFmpeg poll; uses `SharedMediaSession` internal lock |
| Complete | `complete_decode_action(...)` | `control (R)`, `runtime (R/W)`, `diagnostics (W)` | Applies decoded output into audio/decoded queues; wakes render if new decoded video arrived |
| Audio Commit | inside `apply_decoded_output(Audio)` | `control (R)`, `runtime (W)`, `audio_coord (R)` | Trim for seek recovery, enqueue audio |
| EOS Commit | `apply_decoded_output(EndOfStream)` | `runtime (W)` | Marks end of stream |

### 6.5 Audio Backend Thread

| Phase | Resources | Notes |
| --- | --- | --- |
| Device Timing Progression | `audio_coord` internal only | Does not directly lock `SemiPlayerHandle` |

## 7. Atomicity Boundaries

These boundaries must preserve atomicity:

1. **Decoded-video to presentation-video path**: decoded-video enqueue and render execution are separate worker phases. Atomicity is preserved by runtime staging (`begin_render_stage` / `commit_render_stage` / `cancel_render_stage`) plus generation checks at render commit.

2. **Playback advance**: plan pull/discard -> execute -> commit. Separated into three functions, each acquiring domain locks independently.

3. **Seek**: `execute_seek` combines media seek + runtime reset + clock reset as one atomic step under `playback_phase`. No intermediate state visible to other threads.

## 8. Lock Order

Fixed order for nested acquisition:

`playback_phase -> control -> runtime -> media`

Audio and diagnostics are internally locked and should remain short-hold helpers, not broad nesting points.

Rules:

- No call path should hold `runtime` while waiting on a long media operation.
- No call path should hold `runtime` while waiting on a render transform.
- No path should do diagnostics work under the playback critical section.
- No path should re-enter the player through a different lock order.

## 9. Worker Communication

Worker-to-worker communication should use `Condvar` wake signals, not cross-worker task channels.

Reasoning:

- the source of truth is shared player state, not queued command payloads
- wakeups are level-triggered "re-evaluate now" events, not one-shot jobs
- seek/reset/profile changes can invalidate pending work, so workers must resnapshot shared state on wake

Target demand flow:

- sync wakes render when presentation-ready video needs attention
- render wakes decode when decoded-video supply is insufficient
- decode wakes render when new decoded video arrives
- render wakes sync when new presentation frames arrive

This preserves a directional pipeline:

```text
sync
  -> presentation demand
render
  -> decoded demand
decode
```

## 10. Open Questions

- Should `audio_clock` and `audio_output` be physically merged into one mutex?
- Should `playback_phase_lock` be renamed to `op_gate` for clarity?
