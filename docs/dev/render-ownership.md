# Render Ownership Model

This document defines the intended ownership and responsibility split for the video-render stage in
`semi_player_rs`.

It captures the architectural idea:

```text
player owns render
render owns pipeline(s)
pipeline(s) use backend(s)
```

Related documents:

- [pipeline.md](pipeline.md)
- [d3d11-libplacebo-render.md](d3d11-libplacebo-render.md)
- [ARCHITECTURE.md](../../ARCHITECTURE.md)

## 1. Goal

The render subsystem should be a real player-owned stage between decode and presentation.

Its purpose is:

- accept decoded frames in decoder-native formats
- transform them into presentation-ready frames
- own color conversion, scaling, subtitle composition, and related video processing over time

The render subsystem is not:

- part of the decoder
- part of the host presenter
- a global process-wide singleton service

## 2. Ownership Rule

The preferred ownership chain is:

```text
SemiPlayerHandle
  ->
RenderService
  ->
RenderPipeline selection / orchestration
  ->
backend execution
```

More concretely:

- the player owns one render service instance
- the render service owns render state and render pipeline selection
- render pipelines own transformation policy
- render backends own platform execution details

This also means the render service is the owner of future render orchestration:

- which render steps are needed for a given frame
- which steps may run in parallel
- which steps must wait for others
- how intermediate render products are composed into the final presentation frame

## 3. Why This Split

This split keeps three responsibilities from collapsing into one another:

### 3.1 Decode is not render

Decode should produce:

- timed decoded frames
- decoder-native surfaces

Decode should not decide:

- the final presentation pixel format
- subtitle composition behavior
- host-facing render surfaces

### 3.2 Render is not host presentation

Render should produce:

- presentation-ready frames
- using player-owned timing and composition rules

The host should consume those results, not reinvent video processing policy.

### 3.3 Backend is not subsystem ownership

`D3D11`, `libplacebo`, and later other backends are implementation tools of the render subsystem.

They should not define the top-level render architecture by themselves.

## 4. Responsibilities By Layer

### 4.1 Player

The player should own:

- playback state
- decode scheduling
- render scheduling
- sync behavior
- host handoff timing

The player decides:

- when decoded frames should be sent to render
- which presentation target profile is active
- when presentation-ready frames are ready for scheduling/presentation

### 4.2 Render Service

The render service should own:

- decoded-frame to presentation-frame transformation
- long-lived render context state
- pipeline selection
- render-step orchestration
- render-oriented diagnostics

The render service is the subsystem boundary for:

- color conversion
- scaling
- subtitle composition
- future overlays and OSD

The render service should be treated as the coordinator of the whole render workflow, not merely as
a thin wrapper around one pipeline call.

For future complex rendering, it should decide:

- whether video rendering and subtitle rendering can proceed independently
- when composition must wait for upstream steps
- whether cached subtitle/layout data can be reused
- what the final presentation target of the frame should be

### 4.3 Pipeline

A pipeline should describe a concrete transformation strategy.

Examples:

- passthrough pipeline
- CPU BGRA compatibility pipeline
- D3D11 `libplacebo` video pipeline
- subtitle composition pipeline

Pipeline responsibilities:

- decide how a requested transformation should be performed
- express what backend execution path is required
- preserve a stable input/output contract for the render service

Pipelines should not become the top-level scheduler of the whole frame render graph.

That orchestration belongs to the render service.

### 4.4 Backend

A backend should own platform-specific execution details.

For example, a D3D11 backend may own:

- D3D11 device/context bindings
- `libplacebo` D3D11 context
- resource wrapping
- reusable output textures
- backend-local error state

The backend should not become the architectural owner of render scheduling or player state.

## 5. Why Not A Global Singleton Renderer

A process-wide singleton renderer is not the preferred end-state.

Reasons:

- renderer state is naturally tied to a player instance or render context
- future multi-player or multi-device scenarios should not be forced through one global backend
- `libplacebo` context, texture pools, and D3D11 resources fit instance ownership better than
  process-wide ownership
- lifecycle, reset, and testing are cleaner when render is owned by the player

Temporary scaffolding may still exist during refactoring, but the intended architecture is:

- player-owned render service
- render-owned renderer/backend state

not:

- process-global renderer singleton

## 6. Data Flow

The preferred flow is:

```text
decode
  ->
DecodedVideoFrame
  ->
RenderService
  ->
selected pipeline
  ->
backend execution
  ->
PresentationFrame
  ->
video scheduler / host presenter
```

This makes render the stage that turns raw decode output into display-usable output.

For a more complex future frame, the intended flow is closer to:

```text
decoded video frame
  + active subtitle state
  + render request
    ->
RenderService
    ->
video pipeline
    + subtitle pipeline
    ->
composition pipeline
    ->
PresentationFrame
```

In this shape:

- the player/sync layer decides which playback time should be rendered
- the render service decides how that playback time becomes a final frame

## 7. Relationship To `libplacebo` And `libass`

This ownership model fits the planned libraries naturally.

### 7.1 `libplacebo`

`libplacebo` belongs in a render backend or backend-backed pipeline.

It should be used by the render subsystem for:

- YUV to RGB conversion
- scaling
- color management
- future HDR work

### 7.2 `libass`

`libass` should feed the render subsystem, not the decoder and not the host shell.

It should provide:

- subtitle parsing/layout
- bitmap/raster output for subtitle composition

The render subsystem should then decide how those subtitle results are blended onto presentation
frames.

That means subtitle rendering should be treated as a sibling render workflow to video rendering,
with composition owned above both by the render service.

## 8. Recommended Module Direction

The intended module direction is roughly:

```text
core/player/
  player owns render service

render/
  service.rs          render subsystem entry point
  core/
    frame.rs
    pipeline.rs
  pipelines/
    passthrough.rs
    cpu_bgra.rs
    d3d11_placebo.rs
    subtitle_compose.rs
  backends/
    d3d11.rs
```

This exact file layout can evolve, but the ownership rule should stay stable.

## 9. Immediate Design Consequences

Near-term design implications:

- render should become an explicit player-owned service, not just a free function path
- pipeline should be treated as a child concept of render
- backend state should move toward render-instance ownership
- D3D11 renderer lifetime should eventually be bound to the render service instance
- future subtitle, overlay, and composition work should be orchestrated by the render service
  rather than hidden inside a single video pipeline or backend

## 10. Summary

The architectural rule is:

```text
player owns render
render owns pipeline(s)
pipeline(s) use backend(s)
```

This gives the project a clean place for:

- decoded-surface to presentation-surface transformation
- `libplacebo`-based video processing
- `libass`-based subtitle composition
- future platform-specific execution without letting backend details leak upward into player or
  host responsibilities
