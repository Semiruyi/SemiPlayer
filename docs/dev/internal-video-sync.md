# Internal Video Sync System

This document tracks the internal video sync subsystem in `semi_player_rs`.

It started as a forward-looking design note.
It should now be read as a "current architecture plus next steps" document.

For the user-facing sync contract, see [sync.md](sync.md).

## 1. What Is Already True

The player now has a real internal sync worker.

That means these statements are now true:

- video timing is player-owned
- frame advancement is no longer primarily host-driven
- the player computes its own wake deadlines
- the player can wake itself and advance playback without waiting for UI polling

This is the key architectural shift of the current stage.

## 2. Current Components

### `VideoSyncService`

File:

- [`semi_player_rs/src/core/player/video_sync.rs`](../../semi_player_rs/src/core/player/video_sync.rs)

Owns:

- target video time evaluation
- frame correctness checks
- present/drop decisions
- next wake deadline evaluation
- sync snapshots and counters

### `VideoSyncState`

Current state includes:

- last snapshot
- dirty flag
- tick count
- sync count
- keep count
- present count
- drop count
- underflow count
- late-hit count

### `PlayerScheduleService`

File:

- [`semi_player_rs/src/core/player/schedule.rs`](../../semi_player_rs/src/core/player/schedule.rs)

Owns:

- next video sync deadline
- next audio refill deadline
- next combined pump deadline
- suggested wait interval

This layer is what bridges:

- video timing
- audio refill timing
- worker wake timing

### `SyncWorker`

File:

- [`semi_player_rs/src/core/player/sync_worker.rs`](../../semi_player_rs/src/core/player/sync_worker.rs)

Owns:

- sleeping until next work
- waking on control-path notifications
- choosing between:
  - playback advancement
  - render wakeups when presentation-ready supply needs attention

### `RenderWorker`

Planned file:

- [`semi_player_rs/src/player/worker/render.rs`](../../semi_player_rs/src/player/worker/render.rs)

Should own:

- decoded-to-presentation supply policy
- render-stage staging / execute / commit flow
- decode-demand coordination
- waking sync when new presentation frames arrive

## 3. Current Execution Model

Current high-level behavior:

```text
playback command
  -> wake worker
  -> worker evaluates schedule
  -> worker advances playback when due
  -> worker wakes render when presentation-ready supply needs attention
  -> worker sleeps until next deadline
```

This is intentionally still conservative:

- render worker does not exist yet
- decoded-video to presentation-video work is still synchronous on the decode lane
- one operation lock currently serializes worker activity and FFI activity

That is acceptable for the first worker-backed stage.

Target execution model:

```text
playback command
  -> wake sync
  -> sync evaluates playback schedule from presentation-ready state
  -> sync advances playback when due
  -> sync wakes render if presentation supply needs attention
  -> render either stages and renders decoded frames or wakes decode
  -> decode polls media and enqueues decoded frames
  -> decode wakes render
  -> render commits presentation frames
  -> render wakes sync
```

Worker-to-worker wakeups should use `Condvar` signals over shared state snapshots, not task channels.

## 4. Wake Conditions

The worker currently reevaluates when:

- play starts or resumes
- pause occurs
- seek occurs
- reset occurs
- speed changes
- host presentation bias changes

It also forces immediate work when:

- sync state is dirty
- current video is already stale
- a queued video frame exists but no current frame has been promoted
- audio is playing but the backend has not actually started yet

## 5. Responsibility Split

### Player Core

Now owns:

- audio master clock
- frame validity rules
- current frame promotion
- wake scheduling
- core sync diagnostics

### Host

Still owns:

- frame copying / presentation
- host display-latency estimation
- feeding presentation bias back into the player
- application/UI event handling

## 6. What This Solved

Compared with the original host-polling prototype, this stage solved:

- frame advancement waiting on host timer cadence
- strong dependence of sync quality on fixed UI polling intervals
- stale-frame accumulation when the host failed to service playback in time

The specific failure mode that was recently fixed:

- worker waiting on a future deadline even though the current frame was already stale

The fix was to treat stale video, dirty state, and unstarted audio as immediate work.

## 7. What Is Still Missing

This is not yet the final playback architecture.

Still missing:

- dedicated render worker ownership between sync and decode
- explicit `sync -> render -> decode -> render -> sync` wake flow
- subtitle timing integration into the same worker-owned progression model
- richer worker diagnostics over FFI
- finer-grained locking / ownership than the current single operation lock

## 8. Current Risks

Known current risks:

- decode, sync, and FFI reads still share one coarse serialization boundary
- heavy host frame-copy paths can still interfere with worker responsiveness
- decoded-video to presentation-video work still shares the decode lane, so future separation will matter for scalability

## 9. Next Steps

The most useful next implementation steps are:

1. measure worker-driven sync quality directly
2. introduce the dedicated render worker and `Condvar` wake flow
3. turn the logical decode split into a real dedicated execution path behind render-owned demand
4. reduce coarse lock scope where safe
5. integrate subtitle timing into the same worker-owned timeline
6. prepare real render backend ownership boundaries
