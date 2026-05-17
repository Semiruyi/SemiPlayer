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
- video frames are normalized to BGRA for current host copy-out
- audio output path exists through `cpal`
- audio clock uses backend playback timing when available
- video scheduler decisions exist
- presentation bias API exists
- `VideoSyncService` owns core video sync decisions
- player-owned sync worker is active
- player-owned decode worker is active
- playback advancement now executes in phased lock-in / lock-out / lock-in form
- decode polling now runs outside the main player lock and applies results back under generation guards
- manual `pump` path now follows the same playback/decode scheduling semantics as the worker path
- decode-to-sync wake behavior has started tightening to avoid unnecessary sync wakeups on steady audio refill
- worker-vs-UI pump comparison tooling exists in smoke
- FFI and worker mutations are serialized through the player handle

Not done yet:

- lock-independent decode pipeline beyond the shared player handle lock
- real render backend / output surface abstraction
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

### P0.2 Split decode supply from `pump_player(...)`

Status: major baseline done, deeper concurrency split still pending

Tasks:

- keep decode supply separated from playback advancement at the code-path level
- stop treating `pump_player(...)` as the primary internal execution model
- keep manual pump aligned with worker scheduling semantics
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

Status: active, observability and first pre-target video fast path landed

Tasks:

- add seek result metrics:
  - API seek duration
  - first video frame latency
  - first audible audio latency
  - stable post-seek settle time
- add internal seek stage timing metrics:
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
- document the current seek path and the target seek-recovery model explicitly
- adopt a performance-first keyframe-anchored seek strategy for local playback
- add a dedicated seek-recovery path instead of treating seek as a plain full reset + refill
- reduce work done while holding the shared player handle during seek
- review which state must be cleared immediately vs lazily rebuilt after seek
- avoid unnecessary wake storms or duplicate refill work right after seek
- define a practical short-term seek target for local files:
  - fast first-frame response after keyboard/progress-bar seek
  - stable A/V resettling shortly after
- trim audio to the target point during seek recovery
- keep refining which pre-target video frames can bypass expensive post-processing during seek recovery

Why this matters:

- seek responsiveness is part of the core player feel
- poor seek behavior will be much more visible to users than many backend details
- seek touches decode, runtime reset, audio output, and sync wake policy together, so it is worth treating as a first-class performance track

## P1 - Real Output and Backend Boundaries

### P1.1 Define render output surface abstraction

Tasks:

- add portable render output concepts
- define what the core hands to the host/backend
- avoid making BGRA copy-out the only long-term model

### P1.2 Implement first real Windows render backend

Tasks:

- establish `render/backends/d3d11/`
- create device/resources
- support a host-consumable output path
- keep backend details out of portable core contracts

### P1.3 Clarify host adapter boundary

Tasks:

- keep smoke app diagnostic-only
- define what belongs in:
  - interop layer
  - WPF adapter
  - future Avalonia adapter

## P2 - Subtitle and Host Integration

### P2.1 Define subtitle domain model

Tasks:

- track selection
- visibility
- delay / offset
- embedded vs external subtitle source

### P2.2 Integrate libass

Tasks:

- establish binding strategy
- support text subtitle layout/rasterization
- connect subtitle timing to the player timeline

### P2.3 Unify subtitle timing with worker-owned playback

Tasks:

- evaluate subtitle events against the same master timeline
- make subtitle timing react correctly to:
  - play / pause
  - seek
  - speed
  - host presentation bias rules where relevant

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
4. reduce seek-related coupling to the shared player lock
5. define render output surface abstraction
6. start the first real Windows render backend
7. integrate subtitle timing into the worker-owned playback model
