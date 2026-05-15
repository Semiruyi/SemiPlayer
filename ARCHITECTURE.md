# SemiPlayer Architecture

SemiPlayer is a media playback project built around a cross-platform Rust core.

Current implementation focus:

- first verified platform: Windows
- first UI host: WPF
- long-term UI host direction: Avalonia

The key architectural rule is:

- the playback core should be cross-platform
- platform-specific rendering and presentation should live behind backend and adapter boundaries

## 1. Goals

- Build a Rust-driven player with:
  - initialize
  - open/play
  - pause
  - seek
  - reset
  - speed control
  - subtitle support
- Use FFmpeg for demux/decode
- Use libass for text subtitle layout/rasterization
- Use a custom rendering pipeline in Rust
- Keep the playback core portable across platforms
- Allow platform-specific presentation adapters without redesigning the player core

## 2. Non-Goals

- Do not make WPF-specific behavior part of the core player design
- Do not render subtitles in the UI layer
- Do not let the public C ABI expose FFmpeg or renderer internals directly
- Do not assume the first Windows rendering path is the only long-term backend

## 3. Current Verified State

The repository has already verified the current Windows development chain:

- command-line `ffmpeg`
- Rust build in `semi_player_rs`
- .NET interop smoke test in `tools/smoke/SemiPlayer.HelloTest`

Reference:

- [FFMPEG_ENV_SETUP.md](c:/y-s/project/Semi/FFMPEG_ENV_SETUP.md)

Important current facts:

- Rust dependency: `ffmpeg-next = 8.1`
- FFmpeg package: local shared build under `third_party/ffmpeg/current/ffmpeg`
- `FFMPEG_DIR` is configured in `.cargo/config.toml`
- the verified environment document is currently Windows-specific

## 4. Platform Strategy

The project should be understood as three layers:

```text
Cross-platform playback core
    ->
platform rendering backend
    ->
UI presentation adapter
```

This means:

- demux/decode/sync/subtitle logic belongs in the portable Rust core
- GPU surface creation and presentation details belong in platform backends
- WPF and Avalonia belong in host adapter layers, not in the core player

## 5. Current Repository Layout

```text
Semi/
  semi_player_rs/              Rust playback core crate
  tools/
    smoke/
      SemiPlayer.HelloTest/    temporary Windows/.NET smoke test
  third_party/                 local FFmpeg package and related assets
  FFMPEG_ENV_SETUP.md          current Windows development baseline
  ARCHITECTURE.md              this document
```

## 6. Target Repository Layout

```text
Semi/
  semi_player_rs/
    src/
      lib.rs
      api/
      core/
      render/
      audio/
      subtitle/
      platform/
      util/
  SemiPlayer.Interop/          .NET interop declarations
  SemiPlayer.Wpf/              Windows WPF presentation adapter
  SemiPlayer.Avalonia/         Avalonia presentation adapter
  third_party/
  docs/
    env/
      windows.md
  ARCHITECTURE.md
```

Notes:

- `core/` is platform-agnostic player logic
- `platform/` is where OS/backend-specific implementations live
- `render/` should separate portable rendering concepts from backend-specific code

## 7. System Overview

```text
UI Host
    ->
Interop Adapter
    ->
Rust Playback Core
        ->
    Platform Backend(s)
```

Rust core owns:

- media open/close
- demux/decode
- subtitle processing
- playback state machine
- clocks and sync
- output contracts

Platform backends own:

- GPU device creation
- audio device binding details where required
- platform surface/export semantics

UI hosts own:

- application windowing
- input wiring
- presenting the exported frame output
- user-facing controls

## 8. Core Design Rule

The Rust core should not know whether the consumer is:

- WPF
- Avalonia
- Windows
- another future host

It should know only:

- how to open and control playback
- how to decode media
- how to synchronize outputs
- how to produce renderable frame output through an abstract contract

## 9. Rust Module Direction

Recommended direction:

```text
semi_player_rs/src/
  lib.rs                 C ABI exports only
  api/
    mod.rs
    types.rs
    error.rs
  core/
    mod.rs
    player/
    media/
    sync/
  render/
    mod.rs
    core/
    backends/
  audio/
    mod.rs
    core/
    backends/
  subtitle/
    mod.rs
    ass/
    model/
  platform/
    mod.rs
    windows/
    common/
  util/
    mod.rs
```

### Why This Split

- `core/` holds platform-neutral player behavior
- `render/core/` defines portable render concepts
- `render/backends/` contains implementations such as D3D11
- `audio/backends/` allows later separation if platform-specific handling grows
- `platform/windows/` is where Windows-only glue belongs

## 10. Playback Lifecycle

The player should expose a handle-based lifecycle.

High-level states:

```text
Idle
  -> Initialized
  -> Opening
  -> Ready
  -> Playing
  -> Paused
  -> Seeking
  -> Stopped
  -> Ended
  -> Error
```

Expected operations:

- create / initialize
- open media
- play
- pause
- seek
- reset
- set playback speed
- configure subtitles
- destroy

## 11. Threading Model

FFI calls should not directly mutate decoder, renderer, or audio objects.

Recommended shape:

```text
UI Thread
  -> FFI
  -> command push

Controller Thread
  -> owns state transitions

Worker Threads
  - demux
  - video decode
  - audio decode
  - subtitle processing
  - render
  - audio output
```

Why:

- keeps UI responsive
- makes seek/reset safer
- avoids leaking host/UI assumptions into the core

## 12. Playback Pipeline

```text
input
  -> demux
  -> packet queues
  -> decode
  -> audio/video/subtitle processing
  -> backend-specific output path
```

Video:

- FFmpeg decodes compressed packets into frames
- the core schedules and forwards frames into the active render backend

Audio:

- FFmpeg decodes PCM frames
- resample and optional time-stretch happen before output

Subtitles:

- embedded text subtitles route through libass
- bitmap subtitles are decoded and composited through the active rendering path
- external subtitle files join the same logical timeline

## 13. Rendering Strategy

### 13.1 Portable Rule

The render layer should expose portable concepts such as:

- frame input
- render target
- subtitle overlay composition
- output surface contract

It should not expose D3D11-specific concepts as the only valid model.

### 13.2 First Verified Backend

The first verified rendering backend can still be Windows D3D11:

- D3D11 device created in Rust
- output surface exported for WPF interop
- WPF adapter presents through `D3DImage`

But this should be implemented as:

- a Windows backend
- not as the definition of the render subsystem itself

### 13.3 Backend Direction

Recommended shape:

```text
render/
  core/
    renderer.rs
    frame.rs
    surface.rs
    compositor.rs
  backends/
    d3d11/
      device.rs
      renderer.rs
      shared_surface.rs
```

Later backends can be added without changing player semantics.

## 14. Subtitle Architecture

### 14.1 Text Subtitles

Use libass inside Rust for:

- embedded ASS/SSA/SRT-derived tracks
- external subtitle files
- future style override support

Flow:

```text
subtitle packets / subtitle text
  -> libass track state
  -> ass_render_frame
  -> bitmap fragments
  -> backend compositor
```

### 14.2 Bitmap Subtitles

For formats like PGS/DVD subtitle:

- decode with FFmpeg
- upload or compose through the active render backend

### 14.3 Subtitle Controls

Reserve support for:

- subtitle track enumeration
- selecting subtitle track
- loading external subtitle file
- subtitle show/hide
- subtitle delay offset

## 15. Audio and Speed Control

### 15.1 Portable Intention

The audio subsystem should expose a portable output model and isolate platform-specific output details.

### 15.2 First Practical Backend

Short-term backend:

- `cpal`

On Windows this currently means WASAPI underneath, but the architecture should treat that as backend detail.

### 15.3 Time-Stretch

Pure resampling is not enough because it changes pitch.

Preferred direction:

- primary: SoundTouch
- alternative: RubberBand

### 15.4 Sync Rule

Recommended sync rule:

- audio clock is the master clock
- video schedules itself against audio
- subtitles follow the same timeline basis

## 16. UI Host Strategy

### 16.1 WPF Short-Term

Short-term Windows presentation path:

- Rust exports a Windows-compatible output surface
- WPF adapter opens it through the Windows presentation path
- `D3DImage` hosts the frame

This belongs in a WPF adapter project, not in the portable player core.

### 16.2 Avalonia Long-Term

Avalonia should reuse:

- the same core playback API
- the same output contract category

But its presentation adapter should be free to differ from WPF.

## 17. Public C ABI

The public ABI should stay handle-based and command-oriented.

Illustrative shape:

```c
typedef struct SemiPlayerHandle SemiPlayerHandle;

int semi_player_create(SemiPlayerHandle** out_player);
void semi_player_destroy(SemiPlayerHandle* player);

int semi_player_open(SemiPlayerHandle* player, const char* path_utf8);
int semi_player_play(SemiPlayerHandle* player);
int semi_player_pause(SemiPlayerHandle* player);
int semi_player_seek(SemiPlayerHandle* player, int64_t position_ms, int exact);
int semi_player_reset(SemiPlayerHandle* player);
int semi_player_set_speed(SemiPlayerHandle* player, double speed);

int semi_player_select_subtitle_track(SemiPlayerHandle* player, int32_t track_id);
int semi_player_load_external_subtitle(SemiPlayerHandle* player, const char* path_utf8);
int semi_player_set_subtitle_visible(SemiPlayerHandle* player, int visible);

int semi_player_get_position_ms(SemiPlayerHandle* player, int64_t* out_position_ms);
int semi_player_get_duration_ms(SemiPlayerHandle* player, int64_t* out_duration_ms);
int semi_player_get_state(SemiPlayerHandle* player, int32_t* out_state);
```

Design notes:

- strings should be UTF-8
- functions return explicit error codes
- opaque pointer hides Rust internals
- platform-specific surface/export APIs should not define the whole ABI shape

## 18. Temporary Scaffolding

The current smoke test in:

- `tools/smoke/SemiPlayer.HelloTest`

exists only to verify:

- Rust DLL loading
- FFmpeg DLL loading
- basic P/Invoke plumbing

It is not the target application structure.

## 19. Dependency Direction

Current verified dependency:

```toml
ffmpeg-next = { version = "8.1", features = ["codec", "format", "software-resampling", "software-scaling"] }
```

Planned dependency groups:

- core playback
- subtitle integration
- audio backend
- platform render backend
- host interop adapter

This grouping matters more than the exact crate list because it defines portability boundaries.

## 20. Risks and Deferred Decisions

Known risks:

- the first Windows rendering path may tempt the core API to become Windows-shaped
- subtitle composition becomes backend-sensitive once bitmap subtitles are added
- low-latency audio and seek/reset semantics will need careful backend isolation
- Avalonia presentation details should be validated only after the output contract is stable

Deferred decisions:

- exact render backend portfolio beyond the first Windows implementation
- whether `cpal` remains sufficient across target platforms
- exact callback/event model
- how platform-specific output surfaces should be represented in public APIs

## 21. Milestones

### M1: Replace Test Scaffolding With Real Player Skeleton

- remove smoke-test exports
- introduce `SemiPlayerHandle`
- add controller/state/command skeleton

### M2: Media Open and Timeline Basics

- open media file
- detect streams
- expose duration and current state
- add seek/reset plumbing

### M3: Core/Backend Separation

- separate portable playback logic from backend-specific implementation points
- establish render/audio backend boundaries

### M4: First Verified Windows Backend

- build Rust Windows render backend
- export a Windows-compatible output surface
- present through WPF `D3DImage`

### M5: Audio Output + Sync + Speed

- add audio output
- add master clock
- add speed control

### M6: Subtitle Pipeline

- integrate libass
- subtitle selection and visibility
- composite subtitles through backend rendering

### M7: Avalonia Adapter

- keep Rust core unchanged
- build Avalonia presentation adapter on the same player core

## 22. Summary

SemiPlayer should be treated as:

- one cross-platform Rust playback core
- one or more platform-specific backends
- one or more UI presentation adapters

Windows is the first verified implementation target.

It is not the architectural definition of the player.
