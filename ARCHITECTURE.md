# SemiPlayer Architecture

SemiPlayer is a media playback project built around a cross-platform Rust core.

Current implementation focus:

- first verified platform: Windows
- first host shell: WPF smoke app
- long-term host direction: Avalonia

The core architectural rule remains:

- playback semantics belong to the Rust core
- platform presentation details belong to backend and host adapter layers

## 1. Goals

Build a player core that can own:

- create / destroy
- open / play / pause
- seek / reset
- speed control
- subtitle timing
- synchronized audio/video playback
- host-aware presentation bias compensation

And do that without baking WPF-specific behavior into the core.

## 2. Current System Layers

Today the project is best understood as:

```text
Host shell
  ->
FFI / interop boundary
  ->
Rust playback core
    ->
audio backend + future render backend(s)
```

Inside the Rust playback core, the current shape is:

```text
commands / FFI
  -> serialized player handle access
  -> media decode supply
  -> runtime queues
  -> audio output + audio clock
  -> video sync service
  -> internal sync worker
```

## 3. Current Verified State

Already verified:

- local FFmpeg shared build
- Rust build in `semi_player_rs`
- C ABI interop through `.NET`
- decoded audio/video queueing
- BGRA frame copy-out to host
- CPAL-based audio output timing feedback
- player-owned sync worker driving playback progression
- seek recovery now explicitly identifies software video decode as the dominant remaining seek cost

Reference:

- [docs/env/windows.md](c:/y-s/project/Semi/docs/env/windows.md)

## 4. Current Repository Layout

```text
Semi/
  semi_player_rs/              Rust playback core crate
  tools/
    smoke/
      SemiPlayer.HelloTest/    diagnostic WPF host
  third_party/                 local FFmpeg package and related assets
  docs/
    dev/
    env/
  ARCHITECTURE.md
  TODO.md
```

## 5. Core Ownership

The Rust core currently owns:

- media open / probe / decode
- audio output scheduling
- audio master clock
- video frame scheduling
- current frame selection
- playback snapshots and diagnostics
- internal sync worker wake/sleep logic
- seek recovery state and recovery diagnostics

The host currently owns:

- UI windowing
- presenting current video output
- input wiring
- optional explicit pump calls for diagnostics
- host-side presentation delay estimation

## 6. Important Current Design Rules

### 6.1 Audio is the master clock

Video follows audio.

```text
target_video_time = audio_presentation_time + host_presentation_offset
```

### 6.2 The player now owns playback timing

This is the biggest current architectural truth.

The player no longer fundamentally depends on host polling cadence to decide when frames should advance.

The internal sync worker now drives progression.

### 6.3 The host still matters

The host is still responsible for the final presentation path.

That means:

- core sync correctness belongs to the player
- end-to-end visible timing still depends partly on host presentation behavior

The planned render boundary therefore is:

- the Rust core owns timed video surfaces
- the host owns final platform presentation of those surfaces

### 6.4 Concurrency is currently conservative

The player handle currently serializes mutable work through one operation lock.

That is not the final scaling design, but it is the current correctness boundary between:

- FFI commands
- sync worker activity
- runtime state mutation

## 7. Current Rust Module Direction

Current important areas:

```text
semi_player_rs/src/
  lib.rs                 C ABI shim
  api/                   public ABI-facing types and errors
  core/
    media/               FFmpeg-facing decode/probe/open logic
    player/              runtime, sync, worker, scheduling
  audio/                 audio clock, output control, backend glue
  render/                frame types and scheduling decisions
  subtitle/              reserved growth area
  platform/              reserved platform-specific growth area
  util/                  shared helpers
```

Planned growth around rendering:

```text
render/
  core/                  portable frame/surface/scheduling contracts
  backends/
    d3d11/               first Windows hardware video backend
```

## 8. Current Playback Lifecycle

Visible states today:

- `Idle`
- `Ready`
- `Playing`
- `Paused`

Internally, the important progression is:

```text
create
  -> open
  -> ready
  -> play
  -> internal worker advances playback
  -> pause / seek / reset
```

## 9. Current Threading Model

Today the threading model is:

```text
Host thread(s)
  -> FFI calls

Internal sync worker
  -> evaluates deadlines
  -> runs playback work when due

Shared player handle
  -> guarded by one serialized operation lock
```

This is already a meaningful step beyond the original host-pump prototype.

Still true, though:

- decode supply now has its own worker, but decode-result commit still shares the conservative serialized player mutation boundary
- audio output does not yet own a separate player-side controller thread
- render backend work is not yet split into its own backend-specific execution model

## 10. Current Playback Pipeline

```text
FFmpeg input
  -> demux
  -> decode
  -> normalized audio/video outputs
  -> runtime queues
  -> audio output backend
  -> audio clock
  -> video sync
  -> current video frame
  -> FFI frame read/copy
  -> host presentation
```

The internal sync worker currently drives:

- when decode/audio/video work should be revisited
- when stale video must be corrected immediately
- when audio refill should happen

The decode side now also contains the start of a target-aware seek-recovery path:

- seek installs a recovery target in player-owned state
- decode polling reads a recovery policy derived from that state
- FFmpeg-facing video decode can skip expensive BGRA conversion for frames that only exist to advance the decoder before the seek target

The next video-path step is to replace the "normalized BGRA bytes" assumption with a timed
surface model so hardware decode can lower seek and steady-state video cost without changing the
existing frame-timed scheduling semantics.

## 11. What Is Still Transitional

The current architecture is real, but not final.

Transitional parts:

- decode supply has been split logically from playback advancement, but still runs synchronously on the same execution lane
- CPU BGRA copy is still the main host frame-delivery path
- seek recovery is now explicit and keyframe-anchored diagnostics are in place, but reset/rebuild cost and post-target recovery cost are still being reduced
- subtitles are not yet integrated into the same playback worker model
- one coarse lock still protects most mutable player state

## 12. Backend Strategy

### Audio

Current practical backend:

- `cpal`

This is treated as backend detail, not core architecture.

### Rendering

Rendering backend design is still ahead of implementation.

Long-term rule:

- render contracts should remain portable
- D3D11 must be an implementation, not the definition of the render subsystem

Near-term render rule:

- first real output backend is Windows D3D11
- the core should expose video surfaces, not WPF objects
- WPF is the first host adapter, not the render definition
- Avalonia should be able to reuse the same surface-oriented core contract later

## 13. Public ABI Direction

The public ABI remains:

- handle-based
- command-oriented
- platform-neutral at the semantic layer

Important current ABI ideas:

- opaque player handle
- explicit result codes
- UTF-8 strings
- playback snapshot queries
- host presentation bias input

## 14. Near-Term Architecture Priorities

The next architectural steps should focus on:

1. separating decode supply into a real dedicated execution path
2. measuring worker-driven sync behavior objectively
3. improving seek responsiveness with a real recovery-oriented seek model and hardware-backed video path
4. defining real render backend/output surface boundaries
5. integrating subtitle timing into the worker-owned playback model
6. reducing coarse locking where safe

Reference:

- [docs/dev/seek.md](c:/y-s/project/Semi/docs/dev/seek.md)

## 15. Summary

SemiPlayer should now be viewed as:

- a cross-platform Rust playback core
- already owning its internal playback timing
- still in transition from a shared synchronous execution lane to a fuller multi-service playback engine

Windows is the first verified implementation target.

It is not the architectural definition of the player.
