# SemiPlayer TODO

This file tracks the current implementation priorities for SemiPlayer.

Related documents:

- [ARCHITECTURE.md](c:/y-s/project/Semi/ARCHITECTURE.md)
- [docs/env/windows.md](c:/y-s/project/Semi/docs/env/windows.md)

## Current Snapshot

Already done:

- repository and third-party layout have been cleaned up
- root git repository is established
- Windows FFmpeg development baseline is verified
- `semi_player_rs` builds successfully
- `.NET` smoke test loads Rust and FFmpeg DLLs successfully
- initial cross-platform module layout exists
- `lib.rs` has been reduced toward an ABI shim
- FFI error codes and player state types have been moved into `src/api/`
- `SemiPlayerHandle` has been moved into `src/core/player/`
- internal media time has been normalized to microseconds in `src/util/time.rs`
- architecture docs now reflect:
  - cross-platform core
  - backend/adaptor separation
  - host-measured presentation bias compensation

Not done yet:

- real FFmpeg media open and stream inspection
- real audio clock and playback pipeline
- real video frame model and scheduler
- presentation bias API on the FFI surface
- subtitle pipeline and libass integration
- platform render/audio backends

## Working Rule

Priority meanings:

- `P0`: do now
- `P1`: do after P0 is stable
- `P2`: real output and integration
- `P3`: quality, portability, and host expansion

## P0 - Timeline and Scheduling Foundations

### P0.1 Keep `lib.rs` as ABI-only shim

Status: mostly done, continue tightening

Tasks:

- keep exports in `lib.rs`
- move new logic into `api/`, `core/`, `audio/`, `render/`, `subtitle/`
- avoid putting playback rules back into the FFI file

Exit condition:

- `lib.rs` is mostly parameter validation, pointer handling, and delegation

### P0.2 Define audio presentation clock

Status: next active task

Tasks:

- add `semi_player_rs/src/audio/core/clock.rs`
- define how playback time advances when:
  - playing
  - paused
  - seeking
  - speed changes
- distinguish:
  - logical timeline position
  - estimated audible presentation time
- keep internal unit in microseconds

Why this matters:

- the sync model cannot be implemented cleanly until the master clock is explicit

### P0.3 Define normalized video frame contract

Status: next active task

Tasks:

- add `semi_player_rs/src/render/core/frame.rs`
- define a portable `VideoFrame` shape
- include at least:
  - pts / timeline timestamp
  - duration if known
  - pixel format category
  - width / height
- avoid backend-specific handle fields in the core type

### P0.4 Implement video scheduling decisions

Status: blocked only by P0.2/P0.3

Tasks:

- add `semi_player_rs/src/render/core/scheduler.rs`
- optionally add `render/core/queue.rs` if the queue contract becomes useful
- define decision outcomes such as:
  - keep current frame
  - present next frame
  - drop stale frame
  - no frame available
- schedule against:
  - audio presentation clock
  - host-supplied presentation bias

Deliverable:

- scheduler logic can answer "which frame should be shown now"

### P0.5 Add presentation bias API

Status: after scheduler types exist

Tasks:

- add `semi_player_set_video_presentation_bias_ms(...)`
- store bias in `SemiPlayerHandle`
- convert input milliseconds to internal microseconds
- wire scheduler inputs to consume this bias

Why this matters:

- this is the chosen sync contract between player core and host shell

### P0.6 Clarify seek/reset timing semantics

Status: should be defined before real decode threads appear

Tasks:

- define what state changes on seek
- define what queues/frames/clocks get flushed on reset
- define which timestamp becomes authoritative after seek
- document exact versus non-exact seek expectations

## P1 - Real FFmpeg Media Open

### P1.1 Replace fake `semi_player_open()`

Tasks:

- initialize FFmpeg usage properly
- open media input
- inspect stream topology
- detect:
  - video stream
  - audio stream
  - subtitle streams
- populate real duration and basic stream metadata

Deliverable:

- `semi_player_open()` fails meaningfully on invalid media
- `semi_player_get_duration_ms()` returns real duration when available

### P1.2 Define media info model

Tasks:

- add media metadata structures under `core/media/` or `api/`
- capture at least:
  - duration
  - stream presence
  - video dimensions
  - audio sample rate / channels
  - subtitle track count

### P1.3 Expose media info query API

Tasks:

- add a minimal FFI query surface for hosts
- keep it stable and platform-neutral
- do not leak FFmpeg-native types through the ABI

## P1 - Audio Path

### P1.4 Define normalized audio frame format

Tasks:

- add `semi_player_rs/src/audio/core/frame.rs`
- choose a stable internal format, likely:
  - `f32`
  - interleaved or clearly documented planar policy
  - explicit sample rate / channel layout metadata

### P1.5 Add resampling stage

Tasks:

- add `semi_player_rs/src/audio/core/resampler.rs`
- convert FFmpeg decoded frames into the engine's normalized format

### P1.6 Define audio output backend contract

Tasks:

- add queue/ring buffer types
- define portable audio backend traits/interfaces
- keep first practical target as `cpal`

## P1 - Video Path

### P1.7 Define output surface abstraction

Tasks:

- add `semi_player_rs/src/render/core/surface.rs`
- define portable output surface categories
- keep D3D11 shared-handle details out of the core contract

### P1.8 Prepare renderer/backend boundary

Tasks:

- define what the core hands to the backend
- define where subtitle composition joins the frame path
- keep backend ownership under `render/backends/`

## P2 - First Real Playback Output

### P2.1 Add controller loop

Tasks:

- introduce a command-driven playback controller
- stop treating FFI calls as direct long-term state mutation
- prepare ownership boundaries for demux/decode/render/audio workers

### P2.2 Implement first Windows render backend

Tasks:

- build `render/backends/d3d11/`
- create device/resources
- upload normalized video frames
- produce a host-consumable surface

### P2.3 Wire first host adapter path

Tasks:

- keep current smoke test small
- introduce a real WPF-facing adapter when output contract is ready
- feed measured presentation bias back into the player

### P2.4 Implement real play/pause progression

Tasks:

- connect clock, decode, scheduler, and output
- make `play`/`pause` affect real progression rather than only state flags

## P2 - Subtitle Foundations

### P2.5 Define subtitle domain model

Tasks:

- expand `subtitle/model/`
- define track identity, visibility, and delay state

### P2.6 Integrate libass

Tasks:

- establish binding strategy
- add text subtitle pipeline under `subtitle/ass/`
- support embedded and external text subtitle sources

### P2.7 Unify subtitle timing with player timeline

Tasks:

- evaluate subtitle events against the same playback timeline
- keep subtitle/video timing semantics aligned with presentation bias design

## P3 - Playback Quality and Portability

### P3.1 Speed control path

Tasks:

- move from state-only speed flag to real speed-aware playback
- prepare time-stretch backend integration
- keep audio clock semantics correct under non-1.0x playback

### P3.2 Frame drop policy

Tasks:

- define lateness thresholds
- formalize drop/keep heuristics
- document policy in code and architecture docs

### P3.3 Better host feedback model

Tasks:

- keep simple static presentation bias first
- leave room for:
  - dynamic latency estimates
  - per-frame present feedback
  - redraw hints

### P3.4 Avalonia adapter

Tasks:

- validate the cross-platform host strategy on a second UI shell
- reuse the same core playback and sync contracts

### P3.5 macOS compile baseline

Tasks:

- get `semi_player_rs` compiling cleanly on macOS
- identify Windows-shaped assumptions early
- isolate them into backend/platform layers

## Cross-Cutting Rules

### C1. Keep docs aligned

Whenever these change:

- sync contract
- output surface model
- host responsibilities
- environment assumptions

update:

- [ARCHITECTURE.md](c:/y-s/project/Semi/ARCHITECTURE.md)
- [docs/env/windows.md](c:/y-s/project/Semi/docs/env/windows.md)
- [TODO.md](c:/y-s/project/Semi/TODO.md)

### C2. Keep the smoke test small

Rule:

- smoke test verifies wiring
- smoke test does not become the real host application

### C3. Keep platform details behind boundaries

Rule:

- Windows-only details belong in:
  - `platform/windows/`
  - `render/backends/d3d11/`
  - platform audio backend layers
- not in `core/`, `api/`, or portable render/audio contracts

## Recommended Next Sequence

Do these next, in order:

1. implement `audio/core/clock.rs`
2. implement `render/core/frame.rs`
3. implement `render/core/scheduler.rs`
4. add `semi_player_set_video_presentation_bias_ms(...)`
5. define seek/reset timing semantics around the new clock model
6. replace fake `semi_player_open()` with real FFmpeg stream inspection
