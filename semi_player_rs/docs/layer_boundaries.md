# Semi Player Layer Boundaries

## Goal

This document defines the intended boundaries for the main layers in `semi_player_rs`.

The project goals are:

1. high performance
2. cross-platform portability
3. long-term extensibility
4. maintainable ownership boundaries

The central rule is:

- each layer should own one kind of reasoning
- each layer should expose capabilities at the highest useful abstraction level
- concrete backend details should stay as low as possible

This is not a class diagram. It is a boundary contract for future refactors.

## Design Principles

### 1. Keep policy above mechanism

Upper layers should express:

- intent
- preference
- quality/performance tradeoff
- scheduling demand

Lower layers should decide:

- which concrete backend to use
- how to allocate resources
- how to execute platform-specific steps

Example:

- good: `prefer_zero_copy`, `prefer_compatibility`
- less good at the upper layer: `use_d3d11va`

### 2. Keep execution below orchestration

Worker threads and service modules should execute bounded work units.

They should not own:

- global scheduling policy
- cross-layer fallback policy
- player-wide state transitions

### 3. Keep data contracts stable, backend contracts local

Cross-layer data shapes should be stable and portable:

- decoded audio frame
- decoded video surface
- presentation frame
- playback demand

Backend-specific contracts should stay local:

- ffmpeg hardware context pointers
- D3D11 texture ownership details
- platform device handles

### 4. Abstract capabilities, not vendor names

When a layer depends on another layer, it should usually ask for a capability:

- can produce GPU-backed decoded frames
- can render to BGRA CPU output
- can preserve zero-copy surfaces
- can provide a monotonic media clock

It should avoid depending on concrete names unless the concrete name is the real domain concept.

### 5. Cross-platform means platform detail sinks downward

If Windows-specific, FFmpeg-specific, or device-specific code leaks upward, portability gets more expensive over time.

Portable layers may know that a capability exists.
Portable layers should not know the detailed setup ritual for that capability.

## Current Layer Stack

The current source tree already points to a workable layer stack:

1. `api`
2. `player`
3. `scheduler`
4. `sync`
5. `demux`
6. `decode`
7. `render`
8. `audio`
9. `subtitle`
10. `platform`
11. `util`

Not every layer is equally mature yet, but this is a reasonable architectural direction.

## Layer Table

| Layer | Main files today | Should care about | Should not care about | Prefer abstract capability or concrete capability? |
| --- | --- | --- | --- | --- |
| API / FFI | `src/lib.rs`, `src/api/*` | FFI safety, C ABI, stable exported structs/enums, result codes | decode internals, render backend choice, scheduler rules, device setup | Mostly concrete API contract; avoid leaking internal backend names unless part of external product contract |
| Player Orchestration | `src/player/*` | lifecycle, open/play/pause/seek/reset, worker ownership, runtime ownership, user-facing preferences | packet decode mechanics, device-specific GPU interop, pixel conversion details | Abstract capabilities and policies |
| Scheduler | `src/scheduler/*` | resource shortage reasoning, stage wake decisions, stage topology, execution ordering | how decode works, how render works, FFmpeg or D3D specifics | Abstract resource/stage capabilities |
| AV Sync | `src/sync/*` | clocking, deadlines, starvation detection, playback timing demand | decode backend choice, render backend choice, media open lifecycle | Abstract timing and demand capabilities |
| Demux | `src/demux/*` | stream discovery, packet access, seek positioning, demux diagnostics | frame mapping, presentation formats, render targets | Mostly concrete media/container mechanics behind a narrow session contract |
| Decode | `src/decode/*` | packet-to-frame conversion, decode policy, fallback planning, decoded surface production | player lifecycle policy, scheduler ownership, presenter policy | Mixed: upper decode APIs should expose abstract decode capabilities; backend modules can stay concrete |
| Render | `src/render/*` | presentation target negotiation, transform planning, GPU/CPU frame conversion, presentation frame production | demux/seek policy, player state machine, scheduler global logic | Upper render APIs should expose abstract presentation capabilities; backend modules stay concrete |
| Audio | `src/audio/*` | audio frame model, resampling, output buffering, device feed | video decode/render policy, scheduler global reasoning | Abstract audio output capability; concrete backend details low in stack |
| Subtitle | `src/subtitle/*` | subtitle parsing, subtitle render inputs, later composition inputs | decode backend selection, audio output control, player lifecycle | Abstract subtitle/composition capability |
| Platform | `src/platform/*` | OS-specific primitives, platform probing, native integration helpers | playback policy, scheduler logic, cross-domain orchestration | Concrete platform capability providers behind narrow traits/structs |
| Util | `src/util/*` | time math, logging helpers, generic support code | domain ownership, policy decisions | Concrete helper code only; no domain policy |

## Layer Details

## API / FFI

### Should care about

- pointer validation
- ABI-safe structs and enums
- conversion between C-facing and Rust-facing types
- stable error mapping
- minimal, explicit external contract

### Should not care about

- how a decoder is selected
- how render fallback works
- how many workers exist
- how scheduling decisions are made
- how a platform device is initialized

### Capability guidance

This layer is one of the few places where concrete enums are fine, because the API contract itself is concrete.

But the API should only expose concrete backend names when they are product-relevant diagnostics, not because the internals happen to use them.

Good examples:

- playback state
- media info
- presentation profile
- diagnostics snapshot

Use with care:

- `D3d11va`
- `SoftwareBgra`

Those are acceptable as diagnostics, but they should not become the main control contract of the API.

## Player Orchestration

### Should care about

- player lifecycle
- media load/unload
- ownership of runtime state
- worker startup and shutdown
- user-facing preferences
- bridging between control plane and execution plane

### Should not care about

- ffmpeg packet send/receive loops
- D3D11 texture copy details
- swscale conversion mechanics
- exact scheduler scoring logic

### Capability guidance

The player layer should mostly talk in policies and preferences:

- preferred presentation profile
- preferred decode strategy
- subtitle visibility
- seek mode

It should not need to know:

- which hardware device type was used
- how a GPU texture is imported
- whether a particular decoder required fallback because of hardware config probing

Those details can be observed through diagnostics, but should not drive most player control flow.

## Scheduler

### Should care about

- what resources are missing
- which stage can produce them
- whether a stage is blocked or in flight
- when playback should wake

### Should not care about

- frame pixel format details
- hardware device binding
- audio backend startup ritual
- per-backend failure mapping

### Capability guidance

This layer should be almost entirely abstract.

Good scheduler concepts:

- `DecodedVideo`
- `PresentationVideo`
- `AudioDecode`
- `VideoRender`
- `needs_video_now`

Bad scheduler concepts:

- `D3d11Texture`
- `AVHWDeviceContext`
- `NV12ToBGRA`

The scheduler owns resource reasoning, not media mechanics.

## AV Sync

### Should care about

- playback position
- audio clock
- frame readiness windows
- video presentation deadlines
- startup buffering and starvation signals

### Should not care about

- how frames were decoded
- which render backend is active
- concrete subtitle rasterization path

### Capability guidance

`sync` should consume timing-facing facts:

- current audio clock
- queued presentation frames
- frame timestamps and durations
- audio output headroom

It should not be a covert orchestrator of decode and render policy.

Its output should be demand signals, not backend instructions.

## Demux

### Should care about

- container open/probe
- stream selection
- packet read order
- seek positioning
- packet-level diagnostics

### Should not care about

- decode backend choice
- render surface shape
- presentation profile
- scheduler global demand

### Capability guidance

Demux is allowed to be concretely tied to FFmpeg internally, because packet IO and seek semantics are inherently mechanism-heavy.

But its outward contract should stay narrow:

- stream metadata
- packets
- seek/reset behavior
- diagnostics

Demux should not expose presentation-oriented concepts.

## Decode

### Should care about

- turning packets into audio/video frames
- decode-local fallback
- seek-recovery frame suppression
- decoded surface ownership
- decoder diagnostics

### Should not care about

- player-wide lifecycle policy
- render scheduling
- final presenter shape
- UI-facing presentation profile names

### Capability guidance

This layer is where abstraction quality matters most right now.

The decode layer should expose abstract decode intent and output capability, such as:

- prefer compatibility
- prefer performance
- prefer zero-copy
- allow fallback
- can output GPU-backed decoded surfaces

It should avoid making upper layers choose directly among concrete backends.

Recommended split of concerns:

1. decode policy
2. backend selection / plan building
3. concrete backend open
4. packet/frame execution
5. frame mapping into decoded-surface contracts

Recommended shape direction:

```rust
pub enum DecodePreference {
    PreferCompatibility,
    PreferPerformance,
    PreferZeroCopy,
}

pub struct DecodeRequirements {
    pub preference: DecodePreference,
    pub allow_fallback: bool,
    pub require_gpu_surface: bool,
}
```

Then a lower layer can resolve this into a concrete backend plan.

### Current hotspot

`src/decode/decoder.rs` currently mixes:

- decoder opening
- hardware context preparation
- backend fallback
- frame mapping
- color metadata mapping

That is a strong signal that the decode layer needs an internal split even if the external API stays stable.

## Render

### Should care about

- taking decoded video frames and producing presentation frames
- presentation target negotiation
- CPU/GPU transform planning
- renderer fallback
- preserving efficient surface paths when possible

### Should not care about

- demux seek strategy
- packet budgets
- player state machine transitions
- global scheduler ownership

### Capability guidance

Render should expose target capabilities and preferences, not just backend names.

Good render-facing concepts:

- preserve input
- CPU BGRA compatibility
- GPU-presentable BGRA
- preserve GPU texture when possible
- subtitle composition required

Your current `VideoRenderRequest` already points in a healthy direction, because it is closer to capability intent than to implementation detail.

For example, this is good:

- `PresentationTargetProfile::CpuBgraCompatibility`

because it describes an output need rather than a low-level mechanism.

Long term, render should own the mapping from:

- decoded surface kind
- presentation target
- transform path

and upper layers should not need to know the transform mechanics.

### Current hotspot

`src/render/gpu/d3d11.rs` currently owns both:

- D3D11 device/backend logic
- ffmpeg hardware interop glue

Those are related, but not identical responsibilities. Splitting backend device ownership from decoder interop would improve extensibility.

## Audio

### Should care about

- audio frame format normalization
- resampling
- output device feed
- output timing snapshots
- output startup/underrun behavior

### Should not care about

- video backend choice
- subtitle policy
- scheduler graph rules

### Capability guidance

Expose audio needs as stable output capabilities:

- target sample rate/channels
- output readiness
- buffered duration
- backend started state

Do not let upper layers depend on backend-specific output rituals.

Audio should expose timing and buffering facts, not leak device implementation details upward.

## Subtitle

### Should care about

- subtitle data model
- parse/load
- timing windows
- future composition inputs

### Should not care about

- decode backend selection
- render worker ownership
- player control transitions

### Capability guidance

Subtitle should eventually integrate through composition-oriented contracts:

- subtitle cue stream
- renderable subtitle primitives
- composition intent

Avoid baking subtitle policy directly into decode or render backend modules.

## Platform

### Should care about

- OS/platform feature probing
- native device helpers
- platform-specific interop glue

### Should not care about

- scheduler decisions
- player state transitions
- cross-domain runtime ownership

### Capability guidance

Platform modules should be concrete, but they should provide narrow capability providers upward.

Good example direction:

- a provider that can create a hardware decode context
- a provider that can create a GPU presentation device
- a provider that can report supported interop paths

Upper layers should ask what is possible, not how Windows, macOS, or Linux each wire it up.

## Util

### Should care about

- domain-agnostic helpers
- small reusable primitives

### Should not care about

- owning playback behavior
- hiding architecture decisions

### Capability guidance

Keep `util` boring.

If a helper starts embedding domain policy, it probably belongs in a real layer instead.

## Abstract vs Concrete Capability Rules

The simplest rule set for this project is:

| Situation | Prefer |
| --- | --- |
| User-facing control | Abstract capability or policy |
| Cross-layer dependency | Abstract capability |
| Diagnostics and telemetry | Concrete backend detail is acceptable |
| Low-level backend module | Concrete implementation |
| Internal backend selection result | Concrete plan hidden behind abstract request |
| FFI snapshot meant for debugging | Abstract plus selective concrete detail |

Examples:

| Bad upper-layer control | Better control |
| --- | --- |
| `use_d3d11va: bool` | `preference: PreferZeroCopy` |
| `force_sw_bgra: bool` | `target_profile: CpuBgraCompatibility` |
| `decode_with_hw_device_ctx(ptr)` from player | `decode_requirements + context_provider` |

## Proposed Dependency Direction

The intended main dependency direction should look like this:

- `api` -> `player`
- `player` -> `scheduler`, `sync`, `decode`, `render`, `audio`, `subtitle`
- `scheduler` -> shared scheduler/resource models only
- `sync` -> runtime snapshots and timing-facing contracts
- `decode` -> `demux`, `audio`, `render::core`-level frame contracts, platform/backend adapters
- `render` -> render core models, gpu backends, subtitle composition inputs
- `audio` -> backend/device adapters
- `platform` -> native support code only
- `util` -> leaf support

Important negative rules:

- `scheduler` should not depend on backend modules
- `player` should not depend on FFmpeg interop details
- `decode` should not depend on player orchestration logic
- `render` should not depend on demux logic

## Current Cross-Layer Smells

These are the main places where the current codebase still mixes layers.

| File | Smell | Why it matters |
| --- | --- | --- |
| `src/lib.rs` | FFI layer reaches into GPU-backed media open setup | API layer is still participating in backend wiring |
| `src/decode/decoder.rs` | decode policy, backend selection, hardware context glue, and frame mapping are fused | makes cross-platform decode growth harder |
| `src/render/gpu/d3d11.rs` | render backend and ffmpeg interop are fused | backend reuse and future backend additions get noisier |
| `src/player/execution/decode_supply.rs` | player execution owns GPU texture materialization fallback | ownership between decode/runtime/render is still blurry |
| `src/player/worker/decode.rs` and `src/player/worker/render.rs` | workers still carry some local scheduling flavor | execution and scheduling remain partially mixed |

## Recommended Refactor Priorities

1. Keep `api` thin and move backend-specific open wiring behind player/decode-facing capability providers.
2. Split decode internals into:
   - requirements/policy
   - backend plan
   - backend open
   - packet/frame execution
   - frame mapping
3. Split render backend code into:
   - backend device
   - decoder interop adapter
   - presentation renderer
4. Keep growing scheduler as the only owner of cross-stage wake policy.
5. Move player-facing preferences toward abstract capability requests, while keeping concrete backend diagnostics available for visibility.

## Practical Guidance For New Code

When adding a new feature, ask these questions first:

1. Is this layer expressing intent, or accidentally choosing a backend?
2. Is this layer executing work, or accidentally scheduling other layers?
3. Is this contract portable, or did a platform detail leak upward?
4. Is this enum naming a capability, or just exposing today's implementation?
5. If a new backend is added next month, would this API remain stable?

If the answer to the last question is no, the abstraction is probably too concrete.

## Bottom Line

For this project, a good abstraction is not the most generic abstraction.

A good abstraction:

- preserves fast paths
- keeps backend details low
- exposes preferences and capabilities upward
- keeps scheduling separate from execution
- makes adding new decode and render backends predictable

The most important architectural move is:

- upper layers speak in intent
- lower layers resolve intent into concrete plans
- diagnostics report the concrete result without turning it into the main control contract
