# D3D11 `libplacebo` Integration Plan

This document turns the render design in
[d3d11-libplacebo-render.md](d3d11-libplacebo-render.md) into a concrete first-phase execution
plan.

The main question it answers is:

```text
What should be done before and during the first `libplacebo` integration?
```

## 1. Short Answer

Do not start by dropping `libplacebo` binaries into the repo and writing FFI calls blindly.

Do this first:

1. stabilize the Rust-side render contract
2. define how backend state and texture ownership will live
3. choose the Windows dependency and build strategy
4. only then implement the first real `NV12 -> BGRA` GPU render path

This keeps the first integration focused on one narrow milestone instead of mixing architecture
changes, dependency plumbing, and rendering bugs in the same pass.

## 2. First Milestone

The first milestone should prove exactly this:

```text
FFmpeg D3D11VA decode
  ->
DecodedVideoFrame backed by D3D11 NV12
  ->
player-owned D3D11 backend using `libplacebo`
  ->
PresentationFrame backed by D3D11 BGRA
```

It does not need to solve subtitles, HDR polish, or a new host ABI on the first landing.

## 3. Work Order

Recommended order:

1. frame and metadata prep
2. backend lifetime and ownership prep
3. dependency/build prep
4. minimal D3D11 `libplacebo` execution
5. validation and fallback checks

## 4. Phase 1: Frame And Metadata Prep

Before integrating `libplacebo`, the frame contract should carry enough meaning for the backend to
render correctly.

### 4.1 Required decisions

Decide and document:

- what a decoder-native D3D11 frame means in `VideoSurface`
- whether output BGRA textures are backend-owned or caller-owned
- how long returned D3D11 textures stay valid
- whether `PresentationFrame` may share a texture with decode input in passthrough cases

### 4.2 Recommended code prep

Touch these files first:

- [`semi_player_rs/src/render/core/frame.rs`](../../semi_player_rs/src/render/core/frame.rs)
- [`semi_player_rs/src/render/gpu/d3d11/renderer.rs`](../../semi_player_rs/src/render/gpu/d3d11/renderer.rs)
- [`semi_player_rs/src/render/gpu/d3d11/interop.rs`](../../semi_player_rs/src/render/gpu/d3d11/interop.rs)
- [`semi_player_rs/src/decode/session/mod.rs`](../../semi_player_rs/src/decode/session/mod.rs)

Recommended prep items:

- keep `VideoSurfaceStorage::D3d11Texture2D`
- leave room for future frame metadata such as:
  - color range
  - primaries
  - transfer
  - matrix
- make it explicit that `texture_ptr` is an `ID3D11Texture2D*`
- make it explicit that `array_slice` refers to a subresource view of the texture-backed frame

### 4.3 Acceptance criteria

This phase is done when:

- the team can describe the lifetime of decode textures and render textures without guessing
- future color metadata has an obvious home in the frame model
- the D3D11 backend contract does not depend on hidden assumptions from FFmpeg internals

## 5. Phase 2: Backend Lifetime And Ownership Prep

The current backend skeleton is useful, but the real renderer should not be recreated every frame.

### 5.1 Goal

Turn the D3D11 backend into a persistent service with stable state.

### 5.2 Recommended structure

Preferred long-lived state:

- D3D11 device
- D3D11 immediate context or the required rendering context handle
- `libplacebo` context bound to that device
- reusable output texture pool
- backend error/reporting state if useful

### 5.3 Files to shape

- [`semi_player_rs/src/render/gpu/d3d11/renderer.rs`](../../semi_player_rs/src/render/gpu/d3d11/renderer.rs)
- [`semi_player_rs/src/player/execution/render_supply.rs`](../../semi_player_rs/src/player/execution/render_supply.rs)

### 5.4 Acceptance criteria

This phase is done when:

- `D3d11Renderer::new()` is no longer treated as a per-frame scratch helper
- render execution has a place to reuse textures and backend state
- the code has a clear home for initializing and tearing down `libplacebo`

## 6. Phase 3: Dependency And Build Prep

Only after the interface and lifetime model are settled should `libplacebo` be brought in.

### 6.1 Decide the Windows dependency strategy

Make an explicit choice for the first implementation:

- local vendored binaries in `third_party/`
- developer-installed shared library discovered by build config
- another reproducible local strategy

The key rule is reproducibility. A new machine should be able to answer:

```text
Where does `libplacebo` come from, and how does the Rust crate find it?
```

### 6.2 Decide the Rust binding strategy

First implementation recommendation:

- use a narrow internal FFI layer
- bind only the `libplacebo` pieces needed for:
  - context creation
  - D3D11 wrapping
  - image import
  - render into BGRA target

Avoid trying to expose a giant general-purpose safe wrapper before the first frame is rendering.

### 6.3 Suggested repository work

Likely touch points:

- `third_party/` layout
- build notes in [docs/env/windows.md](../env/windows.md)
- `semi_player_rs/Cargo.toml`
- a new FFI module in `semi_player_rs/src/render/backends/`

### 6.4 Acceptance criteria

This phase is done when:

- a clean Windows dev machine has a documented way to obtain `libplacebo`
- the crate has a deterministic way to link or load it
- backend code can compile without hand-edited local hacks

## 7. Phase 4: Minimal `NV12 -> BGRA` Execution

This is the first real integration step.

### 7.1 Scope

Only implement the narrowest useful path:

- input surface kind: `D3d11Texture2D`
- input pixel format: `Nv12`
- output surface kind: `D3d11Texture2D`
- output pixel format: `Bgra8`

Keep other paths unchanged:

- CPU BGRA compatibility stays as fallback
- unsupported D3D11 formats can still report unavailable or unsupported

### 7.2 Files most likely to change

- [`semi_player_rs/src/render/gpu/d3d11/renderer.rs`](../../semi_player_rs/src/render/gpu/d3d11/renderer.rs)
- [`semi_player_rs/src/render/core/pipeline.rs`](../../semi_player_rs/src/render/core/pipeline.rs)
- [`semi_player_rs/src/player/execution/render_supply.rs`](../../semi_player_rs/src/player/execution/render_supply.rs)

### 7.3 Success behavior

Success means:

- render-core still plans `Nv12ToBgraTexture`
- D3D11 backend executes the transform instead of returning `BackendUnavailable`
- returned frame is a real `PresentationFrame` with `PixelFormatCategory::Bgra8`
- fallback diagnostics remain correct when the backend cannot execute

## 8. Phase 5: Validation

The first `libplacebo` landing needs focused validation, not broad feature expansion.

### 8.1 Validate these cases

- D3D11 NV12 input successfully produces D3D11 BGRA output
- CPU BGRA compatibility path still works
- unsupported or missing backend state still falls back cleanly
- render diagnostics still distinguish:
  - passthrough
  - transform required
  - fallback passthrough

### 8.2 Useful validation hooks

- unit tests around render planning
- backend-level tests where possible
- smoke-path logging for:
  - selected presentation profile
  - backend availability
  - transform success/fallback counts

## 9. What Not To Do First

Do not start phase 1 with these:

- full subtitle composition
- P010 support
- HDR tuning
- cross-platform backend abstractions beyond what current code already needs
- a fully polished safe Rust wrapper over all `libplacebo`

Those are good later tasks, but they are not the first door to unlock.

## 10. Immediate Next Tasks

If work starts now, the next concrete tasks should be:

1. document the D3D11 texture lifetime and ownership rule in code-facing docs
2. define where future color metadata will live in `VideoFrame` or `VideoSurface`
3. refactor the D3D11 backend skeleton toward persistent renderer state
4. decide and document the Windows `libplacebo` acquisition/link strategy
5. only then write the first `NV12 -> BGRA` backend implementation

## 11. Summary

The next step is not simply "download `libplacebo` and start wiring."

The next step is:

```text
lock the contract
  ->
lock the backend lifetime model
  ->
lock the dependency strategy
  ->
implement one narrow render path
```

That sequence gives the project a better chance of landing a clean first GPU render path without
thrashing the frame model at the same time.
