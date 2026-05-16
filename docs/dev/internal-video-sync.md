# Internal Video Sync System

This document describes the planned internal video synchronization system for `semi_player_rs`.

It is a forward-looking design note for the next stage of the player core.

For the current synchronization model, see [sync.md](sync.md).

## Why This Exists

The current prototype still relies on an external `pump` loop to advance video frame selection.

That has been useful for:

- ABI bring-up
- smoke testing
- diagnostics
- simple host integration

However, it is not a strong long-term basis for high-quality playback timing.

Observed limitations:

- video frame changes happen only when the host calls `semi_player_pump(...)`
- host UI timing jitter leaks into video timing quality
- higher `pump` rates improve mean `CoreSyncErr`, which proves timing quality is still partly tied to external polling
- even with faster `pump` rates, positive sync-error spikes remain, which suggests the player core should own frame-switch timing more directly

The core conclusion is:

- `pump` is a valid control-plane API
- `pump` should not remain the only timing-plane driver for video presentation

## Design Goal

Move from:

```text
host-driven polling decides when video state advances
```

to:

```text
player-owned video sync loop decides when video state advances
```

while preserving:

- audio as the master clock
- host-supplied presentation offset
- portable Rust core behavior
- a simple external `pump` API for tests and hosts

## Scope

This design is about:

- internal video timing
- current-frame promotion
- stale-frame dropping
- sync diagnostics
- how the host reads presentation results

This design is not yet about:

- GPU present callbacks
- subtitle timing integration details
- adaptive A/V drift correction against device feedback beyond the existing audio-clock model
- full render-thread architecture

## Responsibility Split

### Player Core

The Rust core should own:

- the audio master clock
- video frame queue inspection
- the decision of which frame is current
- when to wake up and reevaluate frame choice
- sync error measurement
- late-frame dropping policy

### Host

The host should own:

- copying or presenting the current frame
- measuring host-side display latency
- feeding host presentation offset back into the player
- user input and application state

### `pump`

`pump` should remain useful for:

- decode supply
- command processing
- state refresh for simple hosts
- headless tests and diagnostics

But `pump` should stop being the only mechanism that advances video presentation state.

## Target Architecture

```text
host/UI
  -> control commands
  -> frame reads

player core
  -> command state
  -> decode/buffer path
  -> audio output path
  -> audio clock
  -> internal video sync service
       -> current frame selection
       -> stale frame dropping
       -> wake-up scheduling
```

## Core Model

Audio remains the master clock.

The video sync system computes:

```text
target_video_time = audio_presentation_time + host_presentation_offset
```

The sync system then decides:

- whether the current frame is still valid
- whether the next frame should become current
- whether one or more queued frames are already stale and should be dropped
- when the next sync reevaluation should happen

## Key Idea

The internal sync system should be deadline-driven.

Instead of waiting for the host to ask "what frame should I show now?", the player should already know:

- what frame is current
- when that will stop being true
- when the next reevaluation should happen

That lets the core behave like a real player rather than a passive polling helper.

## Proposed Components

### 1. `VideoSyncService`

A dedicated internal service responsible for video-timing decisions.

Possible module direction:

```text
semi_player_rs/src/core/player/video_sync.rs
```

Primary responsibilities:

- read target timeline from the audio clock
- examine `current_video_frame` and queued video frames
- promote the next frame when needed
- drop stale frames when needed
- calculate the next wake-up deadline
- record sync diagnostics

### 2. `VideoSyncState`

Internal state owned by the sync service.

Likely fields:

- current frame handle or owned frame
- last sync target time
- last sync error
- next wake-up deadline
- counters:
  - present count
  - keep count
  - drop count
  - late count
  - underflow count

### 3. `VideoSyncDiagnostics`

A stable diagnostics surface for debugging and tests.

Likely fields:

- `core_av_delta_us`
- `core_sync_error_us`
- `target_video_time_us`
- `current_frame_pts_us`
- `current_frame_effective_end_us`
- `next_frame_pts_us`
- `drop_count`
- `late_count`
- `wake_count`

## Threading Direction

The long-term direction should be:

- a dedicated internal video sync thread or task

The first implementation does not need to be complex.

A simple first step is enough:

- one background worker
- one wake/sleep loop
- one shared state boundary for reads and writes

The worker should wake:

- when play starts
- when seek happens
- when speed changes
- when new video frames arrive
- when the next frame deadline is reached

## Wake-Up Strategy

The sync worker should not spin constantly.

It should:

1. compute the current target video time
2. decide current/present/drop
3. compute the next interesting moment
4. sleep until that time or until externally notified

The next interesting moment is usually one of:

- current frame effective end
- next candidate frame PTS
- immediate wake because the queue changed
- immediate wake because a control command changed timing state

## Effective Frame End

The current frame should not be considered expired using only its own decoded duration when a better boundary is available.

Preferred rule:

- if the next frame has a valid future PTS, that PTS is the stronger end boundary
- otherwise use the current frame's own duration-derived end time
- if neither is available, keep the frame until better information arrives

This matches the recent correction already made to `CoreSyncErr` semantics.

## Relation to Current Runtime

Today `PlayerRuntime` owns:

- queued audio frames
- queued video frames
- current video frame

That ownership is acceptable for the prototype, but once a dedicated sync worker exists, access must be tightened.

The important rule should become:

- the video sync service owns writes to `current_video_frame`
- decode supply owns writes to the queued video frame buffer
- read-side consumers only observe snapshots or guarded references

## Interaction With Commands

The internal sync service must react correctly to:

### `play`

- start or resume the sync loop
- compute deadlines against the running audio clock

### `pause`

- stop advancing the current frame
- preserve the current frame for display
- stop periodic wake-ups except for command-driven wakes

### `seek`

- clear stale timing assumptions
- flush or rebase video state as needed
- force immediate reevaluation after new decode supply appears

### `set_speed`

- force immediate timing reevaluation
- recompute deadlines against the new clock behavior

### `reset`

- stop the worker
- clear current and queued timing state

## Interaction With Decode Supply

The sync loop does not decode.

Decode supply continues to happen through the existing pipeline.

But the sync loop must be notified when:

- the video queue transitions from empty to non-empty
- a new candidate frame arrives earlier than the previously known next-deadline assumption

That means decode supply should eventually signal the sync worker when new video frames are pushed.

## Host Read Model

The host should continue to read:

- the current video frame snapshot
- playback diagnostics

The host should not need to tell the player when to switch frames.

That is the main architectural improvement.

## Metrics of Success

After introducing the internal video sync system, we should expect:

- lower dependence on host `pump` frequency
- smaller positive `CoreSyncErr` spikes
- more stable frame-switch timing under UI load
- better seek recovery behavior

More concretely:

- `CoreSyncErr` mean should become less sensitive to external `pump` rate
- positive outliers should shrink
- headless and UI-hosted runs should look more similar

## Migration Plan

### Phase 1: Documentation and state cleanup

- document the internal sync design
- align terminology around host presentation offset
- keep current diagnostics in place

### Phase 2: Extract sync ownership

- isolate video-timing logic behind a dedicated module
- stop scattering current-frame promotion logic across unrelated code paths

### Phase 3: Add internal sync worker

- create a background sync loop
- let it own current-frame promotion and stale-frame dropping
- keep decode supply external for now

### Phase 4: Reduce timing dependence on `pump`

- keep `pump` for decode and command integration
- remove its role as the only frame-advance trigger

### Phase 5: Tune policy

- refine late/early thresholds
- tune wake-up policy
- validate `CoreSyncErr` behavior with the existing sweep tooling

## Open Questions

- should the first internal sync worker use a plain thread, or a shared player-runtime worker model?
- should video frame reads expose snapshots, handles, or copied frame state?
- how should subtitle timing hook into the same internal sync boundary?
- when should decode supply actively notify the sync worker versus letting the worker poll with short sleeps?
- should the player eventually expose explicit sync diagnostics over FFI rather than bundling them into the playback snapshot?

## Recommended Next Implementation Step

The next code step should be:

1. add a dedicated `video_sync` module under `core/player/`
2. move frame-promotion rules behind that module
3. define the worker-facing state and notification points
4. keep the first implementation intentionally small

That is the smallest useful step toward making video synchronization a true player-owned subsystem.
