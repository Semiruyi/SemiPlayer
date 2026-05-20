# Semi Player Target Architecture

## Goal

This document turns the layer-boundary principles into a practical target architecture.

It answers four questions:

1. what stable modules should exist
2. what each module should own
3. how dependencies should flow
4. what refactor order gives the best return

This is the intended medium-term shape for a player that optimizes for:

- high performance
- cross-platform support
- backend growth
- debuggable behavior
- low long-term coupling

## Architectural Thesis

The project should converge on this rule:

- upper layers express intent
- middle layers negotiate capability and build plans
- lower layers execute concrete mechanisms
- diagnostics expose the concrete outcome without turning it into the main control contract

That means the architecture should explicitly separate:

1. preferences
2. capabilities
3. plans
4. execution
5. diagnostics

## Target Top-Level Shape

```text
api
  -> player
      -> scheduler
      -> sync
      -> media
           -> demux
           -> decode
      -> presentation
           -> render
           -> subtitle
      -> audio
      -> platform capabilities
```

The current tree does not need to rename everything immediately, but the ownership should move in this direction.

## Core Concepts

There are a few concepts that should become first-class and reused consistently.

### 1. Preference

What the upper layer wants.

Examples:

- prefer compatibility
- prefer performance
- prefer zero-copy
- prefer CPU-compatible presentation
- subtitles visible

### 2. Capability

What the runtime/backend environment can do.

Examples:

- hardware decode available
- GPU-presentable textures available
- zero-copy decode-to-render interop available
- CPU BGRA fallback available

### 3. Plan

The resolved decision between preference and capability.

Examples:

- decode with hardware surface output and fallback to software
- present as GPU BGRA texture
- present as CPU-packed BGRA

### 4. Execution

The bounded unit of real work.

Examples:

- decode packets into one or more decoded outputs
- render one batch of decoded frames
- submit audio frames
- advance playback state

### 5. Diagnostics

What actually happened.

Examples:

- selected backend
- fallback reason
- transform path usage
- render fallback count
- stage blocked reason

## Target Module Map

## API

### Target role

Stable FFI and host integration boundary.

### Owns

- exported functions
- API-facing structs/enums
- result-code mapping
- ABI conversion

### Should delegate to

- player control interface
- player snapshot interface

### Should not own

- media-open backend wiring
- scheduler dispatch logic
- direct hardware context creation

## Player

### Target role

Application control plane and aggregate root.

### Owns

- player lifecycle
- user-facing control state
- worker lifecycle
- runtime domain ownership
- orchestration of scheduler, sync, decode, render, audio

### Suggested target submodules

```text
player/
  control.rs
  orchestrator.rs
  runtime.rs
  diagnostics.rs
  execution/
  worker/
  access.rs
  view.rs
```

### Should expose

- preferences
- runtime snapshots
- stage execution triggers
- state transitions

### Should not expose

- FFmpeg device pointers
- D3D11 interop details
- decode backend-specific open rituals

## Scheduler

### Target role

Central control-plane decision engine.

### Owns

- resource model
- stage topology
- event queue/state
- pure scheduling decisions

### Suggested target submodules

```text
scheduler/
  types.rs
  state.rs
  snapshot.rs
  decision.rs
  trace.rs
```

### Should receive

- playback demand facts
- stage progress/blocked/idle events
- resource snapshots

### Should output

- stage wake decisions
- playback wake decisions

### Should not know

- frame surface shape
- backend type names
- platform device details

## Sync

### Target role

Playback timing and demand detection.

### Owns

- audio clock collaboration
- video readiness timing
- startup buffering rules
- starvation detection

### Suggested target submodules

```text
sync/
  clock.rs
  demand.rs
  video_sync.rs
  video_scheduler.rs
```

### Should output

- playback demand
- playback deadlines
- ready/not-ready decisions

### Should not output

- direct backend commands
- render/decode implementation instructions

## Media

This is not a current folder, but it is a useful conceptual grouping for `demux + decode`.

### Target role

Packet-to-decoded-data path.

### Owns

- media session model
- demux state
- decode planning
- decode execution

### Should not own

- presentation policy
- UI-facing render profile policy
- player lifecycle

## Demux

### Target role

Container and packet access boundary.

### Suggested target submodules

```text
demux/
  probe.rs
  packet_source.rs
  seek.rs
  diagnostics.rs
```

### Owns

- media probing
- stream metadata
- packet reads
- seek positioning

### Stable outward contract

- media info
- stream selection
- packet source behavior
- seek result/diagnostics

## Decode

### Target role

Decoded-frame production with explicit planning.

### This is the most important module to reshape

Today `src/decode/decoder.rs` is doing too much. The target shape should separate policy, planning, backend open, and execution.

### Suggested target submodules

```text
decode/
  mod.rs
  error.rs
  output.rs
  policy.rs
  diagnostics.rs
  planner.rs
  backend/
    mod.rs
    ffmpeg_software.rs
    ffmpeg_d3d11va.rs
  executor/
    mod.rs
    open.rs
    pump.rs
    map_video.rs
    map_audio.rs
  session/
    mod.rs
    lifecycle.rs
    decode.rs
    shared.rs
```

### Ownership split

#### `policy.rs`

Owns upper-layer decode intent:

- preference
- fallback allowed
- surface requirements

Example shape:

```rust
pub enum DecodePreference {
    PreferCompatibility,
    PreferPerformance,
    PreferZeroCopy,
}

pub struct DecodeRequirements {
    pub preference: DecodePreference,
    pub allow_fallback: bool,
    pub require_gpu_output: bool,
}
```

#### `planner.rs`

Owns capability negotiation and selected decode plan.

Example outputs:

```rust
pub enum DecodeBackendKind {
    Software,
    D3d11va,
}

pub struct DecodePlan {
    pub backend: DecodeBackendKind,
    pub output_kind: DecodedSurfaceKind,
    pub fallback_chain: Vec<DecodeBackendKind>,
}
```

Important:

- planner may know concrete backend kinds
- upper layers should not usually need to

#### `backend/*`

Owns concrete backend-specific opening rules and quirks.

Examples:

- software FFmpeg decoder setup
- D3D11VA setup
- platform/backend-specific failure mapping

#### `executor/open.rs`

Owns opening an active decoder instance from a selected plan.

#### `executor/pump.rs`

Owns packet send/receive and drain behavior.

#### `executor/map_video.rs`

Owns mapping backend-native decoded frames into stable decoded-surface contracts.

#### `executor/map_audio.rs`

Owns audio frame normalization and resample output mapping.

### Stable outward contract for decode

Decode should export stable concepts like:

- `DecodedOutput`
- `DecodeRequirements`
- `DecodeDiagnosticsSnapshot`
- `DecodedVideoFrame`

It should not require upper layers to coordinate concrete FFmpeg hardware setup directly.

## Presentation

This is the conceptual home for `render + subtitle`.

### Target role

Turn decoded media into host-consumable presentation resources.

## Render

### Target role

Decoded-video to presentation-video path.

### Suggested target submodules

```text
render/
  mod.rs
  service.rs
  core/
    frame.rs
    request.rs
    plan.rs
    pipeline.rs
  backend/
    mod.rs
    gpu/
      mod.rs
      d3d11/
        device.rs
        renderer.rs
        interop.rs
    cpu/
      bgra.rs
```

### Why split this way

Right now `render/gpu/d3d11.rs` mixes three distinct jobs:

1. create/manage D3D11 device
2. bridge FFmpeg/D3D11 interop details
3. perform rendering/copy/convert operations

That should become separate files or submodules even if they still live under one backend folder.

### Suggested ownership split

#### `core/request.rs`

Owns presentation intent:

- preserve input
- CPU-compatible output
- GPU-presentable output
- subtitle composition intent

Your current `VideoRenderRequest` is already close to this.

#### `core/plan.rs`

Owns transform planning between input surfaces and target surfaces.

#### `backend/gpu/*/device.rs`

Owns native device creation and lifetime.

#### `backend/gpu/*/interop.rs`

Owns backend-specific decoded-surface interop, especially with decode backends.

#### `backend/gpu/*/renderer.rs`

Owns actual presentation transform or copy behavior.

### Stable outward contract for render

- `VideoRenderRequest`
- `PresentationFrame`
- render stats/diagnostics

Upper layers should ask for target characteristics, not for a specific implementation sequence.

## Audio

### Target role

Decoded-audio to audio-device path.

### Suggested target submodules

```text
audio/
  core/
    frame.rs
    format.rs
    resampler.rs
    output_state.rs
    output_controller.rs
  backend/
    mod.rs
```

### Owns

- normalized audio frame shape
- resampling
- buffering
- backend feed state
- timing snapshots needed by sync

### Should export

- output readiness facts
- buffered duration/headroom
- started/not-started state

### Should not export

- backend quirks as orchestration policy

## Subtitle

### Target role

Subtitle data and eventual composition support.

### Suggested target submodules

```text
subtitle/
  model.rs
  parser.rs
  layout.rs
  composition.rs
```

The implementation can stay small for now, but the architecture should leave room for subtitle-to-presentation composition as a real stage.

## Platform

### Target role

Native capability providers and OS-specific helpers.

### Suggested target submodules

```text
platform/
  mod.rs
  capabilities.rs
  windows/
  macos/
  linux/
```

### Key idea

This layer should increasingly become the place that answers:

- what acceleration paths exist here
- what presentation devices can be created here
- what interop paths are possible here

instead of making `player` or `api` manually wire backend-specific details.

## Runtime Contracts

To keep modules decoupled, a few contracts should be treated as especially stable.

### 1. Decoded video surface contract

This is already emerging in `render/core/frame.rs`.

The long-term idea should be:

- decode produces a decoded surface
- render consumes a decoded surface
- both sides agree on shape and ownership

Important fields:

- pixel format category
- surface kind
- color metadata
- ownership/lifetime model

### 2. Presentation frame contract

This is what playback/sync/FFI should mainly consume.

### 3. Playback demand contract

This is what sync emits and scheduler consumes.

### 4. Stage progress contract

This is what execution workers emit back to scheduler.

## Dependency Rules

## Allowed dependency direction

```text
api -> player
player -> scheduler, sync, decode, render, audio, subtitle
decode -> demux, audio(core), render(core frame contract), platform capability provider
render -> subtitle(composition input), platform capability provider
sync -> player runtime snapshot + audio timing facts + presentation facts
scheduler -> scheduler types only
platform -> native APIs
util -> nobody special; leaf helper
```

## Forbidden or discouraged directions

- `scheduler` -> decode/render backend modules
- `api` -> decode/render backend modules
- `sync` -> backend-specific interop modules
- `render` -> demux
- `decode` -> player orchestration

## What To Keep Concrete

Not everything should be abstracted.

These are good places to stay concrete:

- FFmpeg packet/codec glue
- D3D11 backend implementation
- exact AVBufferRef ownership rules
- CPU pixel conversion routines
- backend-specific diagnostics

Concrete code is healthy when it is:

- low in the stack
- local in scope
- hidden behind a stable contract

## What To Keep Abstract

These are the places where abstraction buys the most:

- user-facing preferences
- scheduler resource/stage model
- decode requirements
- presentation requirements
- platform capability queries
- diagnostics snapshots that aggregate behavior across backends

## Immediate Refactor Targets

These are the highest-value moves based on the current codebase.

## 1. Remove backend wiring from API-facing open path

Current smell:

- `src/lib.rs` reaches into GPU device creation flow to obtain FFmpeg hardware context inputs

Target:

- `player` or a lower capability provider asks platform/render/decode capability layers for media-open requirements
- API just says "open this media"

## 2. Split decode into requirements, planner, executor, mapper

Current smell:

- `src/decode/decoder.rs` is a mixed ownership hotspot

Target:

- policy and selection become explicit
- concrete backends become local
- frame mapping becomes separate from open/pump logic

## 3. Split D3D11 backend into device, interop, renderer

Current smell:

- one file owns too many backend roles

Target:

- easier future backend growth
- clearer decode/render boundary

## 4. Reduce backend detail in player aggregate

Current smell:

- `SemiPlayerHandle` still directly owns some backend-aware wiring concerns

Target:

- `player` owns preferences and services
- lower layers own backend resolution

## 5. Keep scheduler as the only global wake-policy owner

Current smell:

- some worker-side logic still has local scheduling flavor

Target:

- workers execute bounded stage work
- scheduler owns wake reasoning

## Refactor Sequence

This is the recommended order.

## Current Progress Snapshot

The codebase has already completed a meaningful part of the target shape.

### Largely completed

- Phase 1 model cleanup:
  - decode preference / requirements types are explicit
  - render intent is expressed as `PresentationIntent`
  - diagnostics snapshots expose concrete outcomes without turning them into main control APIs
- Phase 2 decode internal split:
  - decode policy, planner, open, pump, and frame mapping are no longer fused into one implementation body
  - session code now has a real directory boundary
- Phase 3 render backend split:
  - D3D11 device, interop, and renderer responsibilities now live in separate files
  - top-level render code talks in `RenderBackend`

### In progress

- Phase 4 open-path cleanup:
  - FFI no longer assembles media-open backend wiring directly
  - player-side open request assembly exists
  - decode session open now has an explicit `MediaOpenRequest`

### Still meaningfully open

- Phase 4 is not fully done until more of the remaining open-path assembly and compatibility wrappers collapse onto the request-shaped path.
- Phase 5 preference unification is still a cleanup and naming pass, not yet a finished sweep.

## Phase 1: Model cleanup without behavior changes

Add or reshape:

- decode requirements model
- decode diagnostics model
- render request/plan naming cleanup
- platform capability provider interfaces

Goal:

- clearer types before moving behavior

## Phase 2: Decode internal split

Split `decode/decoder.rs` into internal modules, even if public APIs stay almost the same.

Goal:

- isolate backend logic
- isolate mapping logic
- make future backend additions predictable

## Phase 3: Render backend split

Split D3D11 backend into device / interop / renderer pieces.

Goal:

- isolate native device ownership from media interop and rendering behavior

Status:

- Mostly completed in the current tree.

## Phase 4: Open-path cleanup

Move media-open capability negotiation below API.

Goal:

- API and player stop carrying concrete backend setup knowledge

Status:

- Started and partially completed.
- `src/lib.rs` no longer assembles backend-specific media-open wiring.
- `player/access` now assembles a player-facing media-open context.
- `decode/session` now has `MediaOpenRequest`, and `session/lifecycle` treats request-shaped open as its internal primary path.

Remaining work:

- Continue collapsing compatibility wrappers toward the request-shaped open path.
- Decide whether `MediaSession::from_input*` should also converge on request-shaped or plan-shaped open input.
- Continue reducing backend-aware assembly that still lives on the player aggregate root.

## Phase 5: Preference unification

Normalize naming across decode/render/player:

- `Preference`
- `Requirements`
- `Plan`
- `Diagnostics`

Goal:

- consistent mental model across modules

Status:

- Partially completed.
- Decode preference, render intent, render backend, and diagnostics language are much cleaner than before.
- A final consistency pass is still needed for open/request/plan naming across session and player code.

## Naming Guidance

To keep the architecture readable, prefer these naming patterns:

| Purpose | Recommended naming |
| --- | --- |
| User-facing intent | `Preference`, `Requirements`, `Profile` |
| Selected mechanism | `Plan`, `Selection` |
| Concrete backend implementation | `Backend`, `Device`, `Renderer`, `Interop` |
| Output observation | `Snapshot`, `Diagnostics`, `Stats` |
| Bounded work execution | `execute_*`, `poll_*`, `commit_*`, `stage_*` |

Avoid vague names like:

- `manager`
- `helper`
- `utils` for domain logic
- `context` when it really means `plan`, `requirements`, or `session`

## Success Criteria

You will know the architecture is improving when:

1. adding a new decode backend mostly touches decode backend/planner code
2. adding a new presentation path mostly touches render core/backend code
3. `player` mostly changes for user-facing behavior, not backend mechanics
4. `scheduler` tests do not care about media backend details
5. diagnostics become richer without forcing upper-layer API churn

## Bottom Line

The best next version of this project is not a giant trait hierarchy.

It is a system where:

- policies are explicit
- capabilities are queryable
- plans are first-class
- execution is bounded
- backend detail stays low

If that shape holds, performance work, cross-platform work, and feature growth stop fighting each other as much.
