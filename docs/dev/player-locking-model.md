# Player Locking Model

This document defines the next-step locking model for `semi_player_rs/src/player`.

Current problem:

- `op_lock` is acting like a whole-object mutex.
- Callers do not actually need the whole player.
- Most call sites only need one resource domain, or one operation-level transaction.

The goal is to replace "lock the player" with "lock the resources required by the operation".

## 1. Current Shape

Today, `SemiPlayerHandle` exposes a single outer mutex through:

- `semi_player_rs/src/player/handle.rs`
- `SemiPlayerHandle::with_locked_ptr_as(...)`

That outer lock protects:

- control state
- runtime queues
- seek recovery
- sync state
- worker coordination
- diagnostics access

Some inner types already have their own locking:

- `media_session: SharedMediaSession`
- `audio_output: SharedAudioOutputController`
- `audio_clock: AudioClock`
- `diagnostics: PlayerDiagnostics`

So the system is already half-way to a finer model. The outer lock is just flattening everything again.

## 2. Target Shape

The player should expose a small number of resource domains, plus one operation gate for heavy transitions.

Suggested domains:

1. `control`
   - `state`
   - `speed`
   - `subtitles_visible`
   - `host_presentation_offset_us`
   - `video_presentation_profile`
   - `media_generation`
   - `seek_recovery`

2. `runtime`
   - `runtime`
   - `video_scheduler`
   - `video_sync`

3. `media`
   - `media_session`

4. `audio`
   - `audio_clock`
   - `audio_output`

5. `diagnostics`
   - `diagnostics`

6. `op_gate`
   - prevents overlapping heavy control operations
   - does not represent a data domain

## 3. Design Rules

- Do not expose a blanket `&mut SemiPlayerHandle` to every caller.
- Do not let callers compose arbitrary lock combinations by hand.
- Keep lock order fixed.
- Keep heavy work outside locks whenever possible.
- Prefer plan/execute/commit for operations that do real work.
- Keep diagnostics readable without waiting on playback work.

## 4. Threads And Resources

This section is the practical concurrency map for the current player.

The point is to answer:

- which threads exist
- which resources they touch
- whether they read, write, or execute heavy work there
- which lock interface should own that access

### 4.1 Threads

| Thread | Current Main Entry | Responsibility |
| --- | --- | --- |
| Host / FFI thread | `semi_player_*` in `src/lib.rs` | Control operations and synchronous queries |
| Decode worker | `src/player/worker/decode.rs` | Decode scheduling, media polling, decoded-output application |
| Sync worker | `src/player/worker/sync.rs` | Playback scheduling, audio submission, video sync advancement |
| Audio backend thread | internal to audio backend | Device timing progression; does not directly lock `SemiPlayerHandle` |

### 4.2 Resource Domains

| Domain | Main Contents | Current Home |
| --- | --- | --- |
| `control` | `state`, `speed`, `subtitles_visible`, `video_presentation_profile`, `host_presentation_offset_us`, `seek_recovery`, `media_generation` | `src/player/handle.rs` |
| `runtime` | audio queue, decoded video queue, presentation video queue, current frame, end-of-stream | `src/player/runtime.rs` |
| `playback_sync` | `video_scheduler`, `video_sync` | `src/sync/video_scheduler.rs`, `src/sync/video_sync.rs` |
| `media` | `media_session` | `src/decode/session/shared.rs` |
| `audio_coord` | `audio_clock`, `audio_output` | `src/sync/clock.rs`, `src/audio/core/output_controller.rs` |
| `render` | `render` / `RenderService` | `src/render/service.rs` |
| `diagnostics` | player metrics and seek telemetry | `src/player/diagnostics.rs` |
| `worker_control` | decode/sync worker wake + shutdown state | `src/player/worker/decode.rs`, `src/player/worker/sync.rs` |

### 4.3 Thread To Resource Matrix

Legend:

- `R`: read
- `W`: write / mutate
- `X`: heavy execution on that resource
- `-`: no direct access

| Thread | control | runtime | playback_sync | media | audio_coord | render | diagnostics | worker_control |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Host / FFI thread | `R/W` | `R/W` | `R/W` | `R/W/X` | `R/W` | `-` | `R` | `-` |
| Decode worker | `R` | `R/W` | `W` | `R/W/X` | `R` | `X` | `W` | `W` |
| Sync worker | `R` | `R/W` | `R/W` | `-` | `R/W/X` | `-` | `W` | `W` |
| Audio backend thread | `-` | `-` | `-` | `-` | internal only | `-` | `-` | `-` |

### 4.4 Current High-Risk Shared Paths

These are the current shared paths where lock boundaries matter most:

1. `Decode worker -> runtime -> render -> playback_sync dirty`
2. `Sync worker -> runtime -> playback_sync -> audio_coord`
3. `Seek/reset/open -> media -> runtime -> audio_coord -> playback_sync`
4. `Playback snapshot -> control + runtime + playback_sync + audio_coord + diagnostics`

The first and third are the current lock hot spots.

## 5. Thread Phase Resource Tables

This section turns the thread/resource matrix into phase-level tables.

Legend:

- `R`: read
- `W`: write / mutate
- `X`: heavy execution
- `Atomic`: this phase should be treated as one short transaction over the listed domains

### 5.1 Host / FFI Thread

| Phase | Typical Entrypoints | Resources | Notes |
| --- | --- | --- | --- |
| Query | `get_state`, `get_position`, `get_duration`, `get_media_info`, `get_playback_snapshot`, current-frame queries | `control (R)`, `runtime (R)`, `playback_sync (R)`, `audio_coord (R)`, `media (R)`, `diagnostics (R)` | Should stay read-mostly and should not require `playback_phase` |
| Control | `play`, `pause`, `set_speed`, `set_subtitle_visible`, `set_video_presentation_profile`, `set_video_presentation_bias_ms` | `control (W)`, `audio_coord (W)`, `playback_sync (W)`, sometimes `runtime (W)` | Short state transitions; usually no heavy media work |
| Seek/Open/Reset Prepare | `open`, `seek`, `seek_prev_keyframe`, `seek_next_keyframe`, `reset` | `playback_phase`, `control (R/W)`, `runtime (R)`, `diagnostics (W)` | Short preflight / staging section |
| Seek/Open/Reset Execute | `open`, `seek`, keyframe-relative seek | `media (X/W)` | Heavy FFmpeg/media work; should not hold wide player locks |
| Seek/Open/Reset Commit | `open`, `seek`, `reset` | `playback_phase`, `control (W)`, `runtime (W)`, `audio_coord (W)`, `playback_sync (W)`, `diagnostics (W)` | Atomic player-state commit after media work finishes |

### 5.2 Decode Worker

| Phase | Current Main Code | Resources | Notes |
| --- | --- | --- | --- |
| Wait | `wait_for_signal(...)` | `worker_control (W)` | Thread parking only |
| Decode Plan | `plan_decode_action(...)` | `control (R)`, `runtime (R)`, `media` handle presence `(R)` | Decides whether decode supply is needed |
| Decode Execute | `poll_decoded_output_once(...)` | `media (X/W)` | FFmpeg-facing poll/decode step |
| Video Prepare | part of `apply_decoded_output(Video)` | `control (R)`, `runtime (W)`, `diagnostics (W)` | Records diagnostics, stages decoded video into player-owned state |
| Render Execute | currently inside `render_supply(...)` | `render (X/W)` | Expensive transform / render path; should become independent from runtime commit |
| Video Commit | tail of `render_supply(...)` + `mark_dirty` | `runtime (W)`, `playback_sync (W)`, `diagnostics (W)` | Atomic promotion from decoded backlog to presentation backlog |
| Audio Commit | `apply_decoded_output(Audio)` | `control (R)`, `runtime (W)`, `audio_coord (R)`, `diagnostics (W)` | Trim for seek recovery, enqueue audio, decide whether sync worker needs waking |
| EOS Commit | `apply_decoded_output(EndOfStream)` | `runtime (W)`, `playback_sync (W)` | Marks EOS and may trigger wake-up |

### 5.3 Sync Worker

| Phase | Current Main Code | Resources | Notes |
| --- | --- | --- | --- |
| Wait | `wait_for_signal(...)` | `worker_control (W)` | Thread parking only |
| Playback Plan | `evaluate_worker_action(...)`, `plan_playback_advance(...)` | `control (R)`, `runtime (R/W)`, `playback_sync (R)`, `audio_coord (R)`, `diagnostics (W)` | Currently not purely read-only because it may discard/pull audio while planning |
| Playback Execute | `execute_playback_plan(...)` | `audio_coord (X/W)` | Audio backend submission / backend-format alignment |
| Playback Commit | `finish_playback_advance(...)` | `playback_phase`, `runtime (W)`, `playback_sync (W)`, `audio_coord (R/W)`, `control (R/W)`, `diagnostics (W)` | Atomic playback-state advancement and seek-recovery update |

### 5.4 Audio Backend Thread

| Phase | Current Main Code | Resources | Notes |
| --- | --- | --- | --- |
| Device Timing Progression | internal backend thread / callback | `audio_coord` internal only | Does not directly lock `SemiPlayerHandle`; exposes timing through snapshots |

### 5.5 Current Atomicity Boundaries To Preserve

These are the phase boundaries where intermediate state is currently dangerous:

1. Decode video prepare -> render execute -> video commit
2. Playback plan pull/discard decisions -> playback execute -> playback commit
3. Seek/open/reset prepare -> media execute -> commit

If these are split further, the design must explicitly represent any "in flight" state that other schedulers might otherwise misread.

## 6. Decode Worker Gap Analysis

This section compares the current decode worker structure against the target phase model.

### 6.1 Current Decode Worker Shape

Current main path:

1. `plan_decode_action(...)`
2. `poll_decoded_output_once(...)`
3. `complete_decode_action(...)`
4. `apply_decoded_output(...)`
5. for video: immediate `render_supply(...)`

That means the current code already has one good split:

- decode polling happens outside the outer player operation lock

But it still has one large coupled commit:

- `Video(frame)` apply and `render_supply(...)` are treated as one combined player-state transaction

### 6.2 Current Phase Mapping

| Target Phase | Current Implementation | Status | Notes |
| --- | --- | --- | --- |
| Wait | `wait_for_signal(...)` | Good | Already isolated to worker-control state |
| Decode Plan | `plan_decode_action(...)` | Mixed | Reasonably small, but still reads runtime and control through the broad player access path |
| Decode Execute | `poll_decoded_output_once(...)` | Good | Already isolated to `media_session` |
| Video Prepare | first half of `apply_decoded_output(Video)` | Coupled | Diagnostics + decoded-queue mutation are bundled with downstream render work |
| Render Execute | inside `render_supply(...)` | Coupled | Heavy render work still happens inside the broad video apply transaction |
| Video Commit | second half of `render_supply(...)` + `mark_dirty` | Coupled | Presentation queue commit and sync dirtying are not explicitly separated from render execution |
| Audio Commit | `apply_decoded_output(Audio)` | Acceptable first pass | Much simpler than video; can stay coupled longer |
| EOS Commit | `apply_decoded_output(EndOfStream)` | Good | Short and clear |

### 6.3 Why Video Is The Hard Part

The dangerous part is not only "rendering is heavy".

The real issue is that current video apply owns a backlog transaction:

1. push decoded frame into decoded-video queue
2. drain decoded-video queue in `render_supply(...)`
3. render the entire drained batch
4. push rendered frames into presentation queue
5. mark video sync dirty

This means the current logic does not treat video apply as a single-frame commit.
It treats it as:

- a decoded backlog transfer
- plus render execution
- plus presentation backlog commit

If that sequence is split without an explicit in-flight model, schedulers may observe a false intermediate state such as:

- decoded queue already empty
- presentation queue not yet filled
- sync not yet dirtied

That is exactly the kind of state that can produce startup white-screen or stalled playback behavior.

### 6.4 What Is Already Safe To Separate

These decode worker boundaries are already safe or mostly safe:

1. wait vs decode plan
2. decode plan vs decode execute
3. decode execute vs audio commit
4. decode execute vs EOS commit

The main unsafe split is:

- video prepare vs render execute vs video commit

unless the player first gains an explicit "render in flight" representation.

### 6.5 What The First Safe Decode Refactor Should Aim For

The first safe decode-worker refactor should not start by moving `render_supply(...)` out of the lock by itself.

It should first establish one of these models:

1. Keep decoded backlog ownership inside one atomic runtime transaction until rendered results are committed
2. Or introduce an explicit render-staging / in-flight domain that schedulers understand

Until one of those exists, the current video path should be treated as preserving an important atomicity boundary.

### 6.6 Practical Decode Worker Refactor Order

Recommended order:

1. Narrow decode plan reads behind `with_control(...)` and `with_runtime(...)`
2. Keep decode execute on `media_session`
3. Leave audio commit and EOS commit mostly as they are
4. Introduce an explicit render staging model for video backlog transfer
5. Only then split video prepare / render execute / video commit across separate lock regions

That order keeps the current startup and scheduling assumptions intact while still shrinking broad player locking around simpler decode phases first.

## 7. Suggested Lock Interfaces

The first stable interface set should be small and explicit.

### 5.1 `with_control(...)`

Owns:

- `control`

Used by:

- host control operations
- decode plan reads
- sync plan reads
- seek prepare and commit

### 5.2 `with_runtime(...)`

Owns:

- `runtime`
- first-pass `playback_sync` if they remain coupled

Used by:

- decode output commit
- render result commit
- sync worker playback advancement
- host current-frame queries

### 5.3 `with_media_session(...)`

Owns:

- `media`

Used by:

- decode polling
- seek execute
- debug decode access

This already exists in practice through `SharedMediaSession`.

### 5.4 `with_audio_coord(...)`

Owns, at least conceptually:

- `audio_clock`
- `audio_output`

Used by:

- play / pause / seek / reset
- playback advance plan and commit
- audio output snapshot reads

The first pass does not have to physically merge these into one mutex, but higher-level code should stop assembling them ad hoc.

### 5.5 `with_render(...)`

Owns:

- `render`

Used by:

- render execution

This should be independent from `runtime` so render execution can happen outside the runtime commit section.

### 5.6 `with_playback_phase(...)`

Owns:

- operation-level playback coordination only

Used by:

- seek
- reset
- open/load
- sync worker playback commit window

This is not a data lock. It is a transaction fence.

## 8. Operation Contracts

### 4.1 Diagnostics Read

Needs:

- `diagnostics`

Should not need:

- `runtime`
- `media`
- `op_gate`

This should become the cheapest public query path.

### 4.2 Playback Snapshot

Needs:

- `control`
- `runtime`
- `audio_clock`
- `audio_output`
- `video_sync`

This is a multi-domain read. It does not need to be globally atomic in the database sense, but it should be internally sensible.

### 4.3 Decode Plan

Needs:

- `control`
- `runtime`
- `media_session` presence

The plan step should be read-heavy and short-lived.

### 4.4 Decode Execute

Needs:

- `media_session`

This is where FFmpeg-facing polling belongs.

### 4.5 Decode Commit

Needs:

- `runtime`
- `control`
- `video_sync`

If render supply is involved, it should be split so the expensive render work runs outside the player lock domain.

### 4.6 Playback Advance

Needs:

- `runtime`
- `control`
- `audio_clock`
- `audio_output`
- `video_sync`

The plan and commit phases should be separated from backend submission.

### 4.7 Seek

Seek should become a three-stage transaction:

1. `seek_prepare`
2. `seek_execute`
3. `seek_commit`

Needs by stage:

- prepare: `op_gate`, `control`, read-only runtime data, diagnostics
- execute: `media_session`
- commit: `runtime`, `audio_output`, `audio_clock`, `video_sync`, `control`

The seek path should not hold a wide player mutex across FFmpeg seek.

## 9. Recommended Lock Order

Use one consistent order for the player-owned domains:

`op_gate -> control -> runtime -> media`

Audio and diagnostics are internally locked resources and should remain short-hold helpers, not broad nesting points.

Important rule:

- no call path should hold `runtime` while waiting on a long media operation
- no path should do diagnostics work under the playback critical section
- no path should re-enter the player through a different lock order

## 10. Migration Plan

### Phase 1

- Add this document.
- Split the outer lock model into named domains in `SemiPlayerHandle`.
- Keep the old outer API as a temporary compatibility layer if needed.
- Add access-layer skeleton helpers first, without rerouting behavior yet.

### Current Implementation Status

The skeleton is no longer only a proposal. The current tree already has a first-pass
`src/player/access.rs` layer and several migrated paths.

Landed access helpers:

- `control_access()` / `control_snapshot()`
- `with_runtime_access(...)`
- `audio_coord_access()`
- `playback_phase_handle()`
- `decode_plan_context()`
- `decode_audio_commit_context()`
- `sync_worker_plan_context()`
- `playback_advance_plan_context()`
- `playback_advance_observe_context()`

Paths already migrated onto the skeleton:

- Host control path in `src/player/orchestrator.rs`
- Playback snapshot and read helpers in `src/player/access.rs` and `src/player/view.rs`
- Pump scheduling reads in `src/player/pump.rs`
- Decode worker plan reads in `src/player/worker/decode.rs`
- Sync worker plan reads and playback advance plan/commit in
  `src/player/worker/sync.rs` and `src/player/execution/playback_advance.rs`
- Decode audio / skipped-audio / EOS commit branches in
  `src/player/execution/decode_supply.rs`

Still intentionally conservative:

- The decode video path still couples `push_decoded_video_frame -> render_supply -> mark_dirty`
- `render_supply(...)` is still treated as one atomic backlog transfer
- The outer `with_locked_ptr_as(...)` path still takes both `op_lock` and `runtime_lock`

That means the project is currently in a hybrid stage:

- access boundaries are becoming explicit
- high-risk atomicity boundaries are still preserved on purpose

### Phase 1a Skeleton Goal

Before large behavior changes, establish a small access-layer skeleton in code:

- `with_control(...)` or `control_snapshot()`
- `with_runtime_access(...)`
- `audio_coord_access()`
- `with_render_access(...)`
- `with_playback_phase(...)`

The purpose of that skeleton is:

- give new code a place to target
- let old code continue to run unchanged
- migrate one call path at a time instead of forcing a lock rewrite in one jump

### Phase 1b Incremental Migration Rules

The refactor should proceed by thread, from the safest paths to the most coupled paths.

Two rules matter here:

1. change access entrypoints before changing behavior boundaries
2. change low-risk read paths before high-risk commit paths

In practice that means:

- first move a call path onto `access.rs`
- keep its behavior and atomicity the same
- only revisit its internal lock split after the access boundary is explicit

This is important because an earlier thread migration can affect later threads in two ways:

- later code may start calling a new access helper instead of touching `SemiPlayerHandle` directly
- later code may need to respect a more explicit phase contract instead of assuming "`op_lock` makes everything safe"

That kind of influence is expected and desirable.
What must be avoided is changing the observed scheduling or commit semantics of a high-risk path too early.

### Phase 1c Thread Migration Order

Recommended order:

1. Host / FFI query paths
2. Host / FFI simple control paths
3. Diagnostics-only and other narrow read helpers
4. Decode worker plan reads
5. Decode worker audio and EOS commit paths
6. Sync worker plan/access cleanup
7. Seek/open/reset prepare and commit reshaping
8. Decode video path
9. Sync worker execute/commit split, if still needed

Why this order:

- steps 1 to 3 are mostly read-only or short control transitions
- steps 4 to 6 clarify ownership without breaking the core media transaction boundaries
- steps 7 to 9 touch the strongest atomicity assumptions and should happen only after the access skeleton is proven

### Phase 1d Paths That Must Stay Conservative

The following paths should not be behaviorally split just because the new access skeleton exists:

1. decode video prepare -> render execute -> video commit
2. playback plan/discard decisions -> playback execute -> playback commit
3. seek/open/reset prepare -> media execute -> commit

For these paths, the first job is to make resource ownership explicit.
The second job, later, is to decide whether a true plan/execute/commit split needs an explicit in-flight model.

Until then:

- keep current atomicity boundaries intact
- avoid moving heavy execution out of a critical section unless the intermediate state is modeled
- treat startup/video-present paths as correctness-first, not lock-granularity-first

### Phase 2

- Move diagnostics reads off the player-wide lock.
- Introduce `control` and `runtime` access helpers.
- Keep `op_gate` for seek/reset/load style operations.

### Phase 3

- Refactor seek into prepare / execute / commit.
- Refactor decode application into plan / execute / commit.
- Refactor playback advance the same way if needed.

### Phase 4

- Remove the old whole-object lock path.
- Tighten helper APIs so new code cannot silently reintroduce the big lock.

## 11. Success Criteria

The refactor is working when:

- diagnostics queries no longer block on decode or seek
- seek does not hold a player-wide mutex across FFmpeg seek
- decode/render work can progress without serializing unrelated reads
- lock ownership is obvious from the API surface
- new call sites must choose a domain instead of receiving the whole player

## 12. Open Questions

- Should `seek_recovery` live in `control` or get its own small domain?
- Should `runtime` and `video_sync` stay together for the first pass?
- Should `playback_phase_lock` be renamed to `op_gate` or remain as the current semantic fence?

The recommended answer for the first pass is:

- keep `runtime` and `video_sync` together
- keep `seek_recovery` with `control`
- rename only if the code change stays small and mechanical
