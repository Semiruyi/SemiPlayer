# Audio/Video Synchronization

This document describes the current synchronization model in `semi_player_rs`.

For pipeline flow, see [pipeline.md](pipeline.md).
For deeper worker-specific design notes, see [internal-video-sync.md](internal-video-sync.md).

## 1. Core Rule

Audio is the master clock.

```text
target_video_time = audio_presentation_time + host_presentation_offset
```

Where:

- `audio_presentation_time` comes from `AudioClock`
- `host_presentation_offset` is host-supplied display compensation
- video selection is evaluated against `target_video_time`

## 2. Current Synchronization Stack

Current sync behavior is spread across these layers:

- [`audio/core/clock.rs`](../../semi_player_rs/src/audio/core/clock.rs)
- [`render/core/scheduler.rs`](../../semi_player_rs/src/render/core/scheduler.rs)
- [`core/player/decode_worker.rs`](../../semi_player_rs/src/core/player/decode_worker.rs)
- [`core/player/video_sync.rs`](../../semi_player_rs/src/core/player/video_sync.rs)
- [`core/player/schedule.rs`](../../semi_player_rs/src/core/player/schedule.rs)
- [`core/player/sync_worker.rs`](../../semi_player_rs/src/core/player/sync_worker.rs)

In practical terms:

```text
audio backend timing
  -> AudioClock
  -> target video time
  -> VideoSyncService
  -> current frame selection
  -> next wake deadline
  -> sync worker
```

## 3. AudioClock

`AudioClock` is no longer just a software projection.

Current behavior:

- `play()` starts progression
- `pause()` freezes progression
- `seek()` rebases the timeline
- `set_speed()` changes logical progression rate
- device playback timing overrides naive wall-clock projection when available

This gives the player a closer estimate of audible playback position.

## 4. Video Selection

`VideoScheduler::decide(...)` receives:

- target time
- current frame
- next queued frame

Possible decisions:

- `KeepCurrent`
- `PresentFrame`
- `DropFrame`
- `NeedMoreFrames`

Important current rule:

- if the next frame has a known future PTS, that PTS becomes the stronger effective end boundary for the current frame

This rule is also reflected in `CoreSyncErr`.

## 5. VideoSyncService

`VideoSyncService` is now the player-owned synchronization surface for video timing.

Responsibilities:

- compute target video time
- evaluate current frame correctness
- promote queued frames when needed
- drop stale queued frames when needed
- expose the next wake deadline
- maintain sync diagnostics and counters

It owns `VideoSyncState`, which currently tracks:

- last sync snapshot
- dirty flag
- tick count
- sync count
- present count
- drop count
- underflow count
- late-hit count

## 6. Sync Worker

The internal sync worker is now active.

Its job is to:

1. inspect current player state
2. evaluate the next playback deadline
3. advance playback when playback work is due
4. run decode supply when buffering is insufficient
5. sleep until the next interesting moment or an explicit wake

Important current consequence:

- frame advancement is no longer primarily driven by UI polling

Current worker state policy:

- `Playing`
  - stays deadline-driven
  - keeps advancing playback continuously
- `Ready` / `Paused`
  - may run a stabilization pass after wake
  - does not remain in a timed wait loop once no immediate work is pending
- `Idle`
  - sleeps until explicit wake

The worker currently wakes for:

- play
- pause
- seek
- reset
- speed changes
- host presentation bias changes
- explicit host pump calls

Decode refill is now owned by a separate decode worker.
The sync worker can request decode wakeups, but no longer needs to perform decode work itself.
Whether decode should actually run is now normalized through a shared decode-schedule hint,
so `idle` / unloaded states do not spuriously wake decode work.
The decode worker now polls FFmpeg outside the main player lock and only re-enters the player
lock to apply decoded outputs, guarded by a media-generation check.
Playback advancement now follows the same broad idea: the sync worker captures a playback plan
under lock, executes audio-output work outside the main player lock, and then re-enters to commit
clock, runtime, and video-sync updates under a playback phase lock.

## 7. Pump Role Now

`semi_player_pump(...)` still exists, but its role has changed.

Today it is:

- a decode supply entry
- a control/debug hook
- a useful diagnostic API

It is no longer supposed to be the only timing-plane driver.

## 8. Current Diagnostics

Playback snapshots expose:

- `Core A-V`
- `CoreSyncErr`
- `HostOffset`
- `Expected End-to-end A-V`
- current frame effective end
- next wake deadline
- next audio refill deadline
- next combined pump deadline
- sync worker counters
- `ffi` / `sync worker` / `decode worker` lock-wait diagnostics

Interpretation:

- `Core A-V`:
  audio position relative to current frame start
- `CoreSyncErr`:
  signed correctness error for the current selected frame
- `HostOffset`:
  host-supplied display compensation
- `Expected End-to-end A-V`:
  model-derived result after host offset, not a measured display metric

## 9. What "Healthy" Looks Like

A healthy current run usually means:

- `CoreSyncErr` stays near `0`
- `VideoPos` keeps advancing
- the current frame changes at expected cadence
- positive sync-error spikes are limited
- playback quality is no longer strongly tied to a fixed host timer interval

## 10. Current Limitations

The current model is much stronger than the original host-pump prototype, but there are still limits:

- decode output application and playback-state commit still serialize on the player handle
- end-to-end display timing is still partly host-dependent
- subtitle timing has not yet been folded into the same worker-owned progression path

## 11. Near-Term Direction

Near-term sync work should focus on:

1. objective worker-vs-host sync measurement
2. further decoupling decode refill from the shared player lock
3. tighter wake policy between decode enqueue and video sync
4. subtitle timing integration
