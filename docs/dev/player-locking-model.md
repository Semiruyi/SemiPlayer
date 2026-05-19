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

## 4. Operation Contracts

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

## 5. Recommended Lock Order

Use one consistent order for the player-owned domains:

`op_gate -> control -> runtime -> media`

Audio and diagnostics are internally locked resources and should remain short-hold helpers, not broad nesting points.

Important rule:

- no call path should hold `runtime` while waiting on a long media operation
- no path should do diagnostics work under the playback critical section
- no path should re-enter the player through a different lock order

## 6. Migration Plan

### Phase 1

- Add this document.
- Split the outer lock model into named domains in `SemiPlayerHandle`.
- Keep the old outer API as a temporary compatibility layer if needed.

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

## 7. Success Criteria

The refactor is working when:

- diagnostics queries no longer block on decode or seek
- seek does not hold a player-wide mutex across FFmpeg seek
- decode/render work can progress without serializing unrelated reads
- lock ownership is obvious from the API surface
- new call sites must choose a domain instead of receiving the whole player

## 8. Open Questions

- Should `seek_recovery` live in `control` or get its own small domain?
- Should `runtime` and `video_sync` stay together for the first pass?
- Should `playback_phase_lock` be renamed to `op_gate` or remain as the current semantic fence?

The recommended answer for the first pass is:

- keep `runtime` and `video_sync` together
- keep `seek_recovery` with `control`
- rename only if the code change stays small and mechanical

