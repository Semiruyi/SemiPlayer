# D3D11 NV12 Render Plan

This document describes the planned render path for turning FFmpeg-decoded D3D11 NV12 frames into
D3D11 BGRA presentation frames in `semi_player_rs`.

It is a design document for the next real render-backend milestone.

Related documents:

- [pipeline.md](pipeline.md)
- [ffmpeg-usage.md](ffmpeg-usage.md)
- [ARCHITECTURE.md](../../ARCHITECTURE.md)

## 1. Goal

The near-term Windows video goal is:

```text
FFmpeg D3D11VA decode
  ->
decoder-native D3D11 NV12 surface
  ->
player-owned GPU render
  ->
presentation-friendly D3D11 BGRA surface
  ->
host presenter
```

This is intentionally different from the older software path:

```text
decode
  ->
swscale to CPU BGRA bytes
  ->
host copy/present
```

The hardware path should preserve GPU-native surfaces for as long as possible and only fall back to
CPU BGRA when compatibility requires it.

## 2. Current Code State

Relevant current files:

- [`semi_player_rs/src/core/media/opened.rs`](../../semi_player_rs/src/core/media/opened.rs)
- [`semi_player_rs/src/render/core/frame.rs`](../../semi_player_rs/src/render/core/frame.rs)
- [`semi_player_rs/src/render/core/pipeline.rs`](../../semi_player_rs/src/render/core/pipeline.rs)
- [`semi_player_rs/src/render/backends/d3d11.rs`](../../semi_player_rs/src/render/backends/d3d11.rs)
- [`semi_player_rs/src/core/player/execution/render_supply.rs`](../../semi_player_rs/src/core/player/execution/render_supply.rs)

Important current implementation facts:

- FFmpeg D3D11VA decode probing is already present
- decoded hardware frames can already be classified as `PixelFormatCategory::Nv12`
- the core frame model already supports `VideoSurfaceStorage::D3d11Texture2D`
- render requests already express a `D3d11BgraPresenter` target profile
- render-core planning already distinguishes:
  - passthrough
  - passthrough with subtitle intent
  - requires transform
- the D3D11 backend already has an explicit `Nv12ToBgraTexture` render-plan kind
- that D3D11 transform path is still a skeleton and currently reports backend unavailable

So the architecture seam for a real GPU render stage already exists in code.

## 3. Why `libplacebo`

`libplacebo` is a strong fit for this render stage because it already solves the class of work we
need between decode and presentation:

- YUV to RGB conversion
- limited/full range handling
- color primaries and transfer handling
- scaling
- tone mapping for future HDR work
- GPU-oriented render graph execution

For this project, the most important value is not just "convert NV12 to BGRA", but:

- keep color conversion out of the host
- avoid reimplementing video color rules in WPF and later Avalonia
- establish one player-owned render stage that can later absorb subtitle composition and OSD work

## 4. Why Not Keep This In Decode

The decode layer should output decoder-native surfaces, not presentation-ready RGB frames.

Decode should own:

- packet to frame decoding
- hardware device setup
- decoder surface acquisition
- frame timing metadata

Decode should not own:

- final presentation pixel format
- scaling policy
- subtitle composition
- host-specific presenter behavior

Keeping color conversion in the render stage preserves the intended split:

```text
decode
  ->
render
  ->
present
```

## 5. Planned Render Ownership

The render stage should accept `DecodedVideoFrame` values and produce `PresentationFrame` values.

The preferred internal meaning is:

- `DecodedVideoFrame`
  - decoder-native surface
  - examples: `D3D11 NV12`, later `D3D11 P010`, software YUV fallback
- `PresentationFrame`
  - presentation-oriented render surface
  - examples: `D3D11 BGRA`, CPU BGRA compatibility fallback

The current type aliases already point in this direction, even though both roles still use the same
underlying frame struct today.

## 6. Proposed `libplacebo` Placement

`libplacebo` should live behind the D3D11 render backend, not in render-core planning code.

Recommended ownership:

```text
player-owned render service
  ->
pipeline selection
  ->
render/core/
  portable render request, frame, and scheduling contracts

render/backends/d3d11/
  D3D11 resource wrapping
  libplacebo integration
  backend execution
```

This keeps `render/core/pipeline.rs` responsible for deciding that a transform is needed, while the
backend decides how to execute that transform on Windows.

Preferred ownership rule:

- player owns render
- render owns pipeline selection and render context state
- pipelines use backend execution

This is preferred over a process-global renderer singleton.

## 7. Proposed End-to-End Windows Path

Preferred steady-state path:

```text
FFmpeg packet
  ->
D3D11VA decode
  ->
AVFrame backed by D3D11 texture
  ->
DecodedVideoFrame { surface = D3d11Texture2D, format = Nv12 }
  ->
render_supply()
  ->
VideoRenderPipeline
  ->
D3D11 backend
  ->
libplacebo render from NV12 planes into BGRA render target
  ->
PresentationFrame { surface = D3d11Texture2D, format = Bgra8 }
  ->
host presenter
```

Compatibility fallback path:

```text
software decode or unsupported backend state
  ->
CPU BGRA transform path
  ->
existing copy-out ABI
```

## 8. Output Contract

The preferred result of the D3D11 render path is not CPU bytes.

The preferred result is:

- `VideoSurfaceStorage::D3d11Texture2D`
- `PixelFormatCategory::Bgra8`

This matters because the render stage is not merely a format converter. It is the owner of the
presentation-friendly GPU surface that later composition stages should target.

CPU BGRA should remain available as:

- fallback
- compatibility contract
- diagnostics path

It should not define the main hardware-render architecture.

## 9. Subtitle Plan With `libass`

The subtitle plan should pair naturally with the render-stage design.

Recommended ownership split:

- `libass`
  - parse ASS/SSA
  - shape/layout subtitle events
  - rasterize subtitle output into bitmap regions
- player render stage
  - upload subtitle bitmaps to GPU resources
  - blend subtitle overlays onto the same presentation target as video

That makes the long-term render path:

```text
video decode surface
  ->
libplacebo video render
  + subtitle overlays from libass
  ->
final presentation BGRA surface
```

Important design rule:

- subtitles should not be burned into decoded surfaces

They belong to render-time composition, where visibility changes, style changes, and host reuse are
all cheaper and cleaner.

## 10. Transitional Subtitle Strategy

The first subtitle milestone does not need full GPU composition on day one.

Reasonable phase order:

1. keep subtitle timing independent from video decode
2. integrate `libass` for event parsing and layout
3. allow a transitional overlay path if needed
4. move subtitle composition fully into the player-owned render stage

This keeps the timing model stable while the GPU path matures.

## 11. Resource and Lifetime Rules

The D3D11 backend should become a long-lived backend service, not a per-frame scratch object.

Recommended direction:

- one render-owned D3D11 renderer instance
- one render-owned D3D11 render context
- one render-owned `libplacebo` context bound to that D3D11 device
- pooled or reusable BGRA render targets owned by the render subsystem
- frame objects carry references or handles to backend-owned textures

Avoid this shape for the real implementation:

```text
create renderer every frame
  ->
wrap input
  ->
destroy renderer
```

The current `D3d11Renderer::new()` skeleton is acceptable as a placeholder, but the real
implementation should hold persistent backend state owned by the render service instance.

## 12. Data Needed From Decode

For `libplacebo` to do correct video rendering, decode-side metadata should eventually carry more
than width, height, and `NV12`.

Important future metadata:

- color range
- color primaries
- transfer characteristics
- matrix coefficients
- chroma location when relevant
- HDR metadata when relevant

The first implementation can begin conservatively if some of this metadata is unavailable, but the
render contract should leave room for it instead of hard-coding SDR assumptions forever.

## 13. ABI Direction

Normal host-facing ABI should continue to trend toward presentation-oriented surfaces, not
decoder-native ones.

Preferred contract direction:

- host asks for current presentation frame
- host receives a surface descriptor for a presentation-friendly texture
- host does not need to understand `NV12` decode semantics

Low-level decoder-surface exposure may still exist for diagnostics, but it should not become the
main integration path.

## 14. Implementation Phases

Recommended implementation order:

1. keep the current render-core planning boundary
2. make the D3D11 backend stateful and long-lived
3. integrate `libplacebo` in `render/backends/d3d11.rs`
4. implement real `Nv12ToBgraTexture` execution
5. return a new BGRA D3D11 presentation surface instead of fallback passthrough
6. preserve CPU BGRA path as compatibility fallback
7. integrate subtitle timing and `libass`
8. move subtitle bitmap composition onto the same GPU render target

## 15. Immediate Repository Implications

The next code changes should likely touch:

- `semi_player_rs/src/render/backends/d3d11.rs`
  - real backend state
  - `libplacebo` binding/integration
  - NV12 to BGRA execution
- `semi_player_rs/src/render/core/frame.rs`
  - optional future color metadata extension
- `semi_player_rs/src/core/media/opened.rs`
  - preserve decode-native D3D11 outputs
  - expose enough metadata for render
- `semi_player_rs/src/core/player/execution/render_supply.rs`
  - continue owning decoded-to-presentation promotion

The current synchronous `render_supply()` step is still a good place to land the first real render
implementation before deciding whether the render stage needs its own worker.

## 16. Non-Goals For The First Milestone

The first `libplacebo` milestone does not need to solve everything.

Explicit non-goals for the first landing:

- HDR polish
- complex subtitle animation optimization
- cross-platform render backend parity
- host-independent screenshot/export pipeline
- asynchronous render worker execution

The first milestone only needs to prove:

- FFmpeg D3D11 NV12 input can remain GPU-native
- the player can render it to a D3D11 BGRA presentation surface
- the render stage boundary is real
- the design leaves room for later subtitle composition with `libass`

## 17. Summary

The intended Windows hardware video pipeline is:

```text
FFmpeg D3D11VA decode
  ->
decoder-native D3D11 NV12 frame
  ->
player-owned `libplacebo` render
  ->
D3D11 BGRA presentation frame
  ->
host presenter
```

`libplacebo` should be the video-render engine for color conversion and future scaling/HDR work.
`libass` should provide subtitle layout and bitmap generation.
The player-owned render stage should compose those results into a presentation-friendly BGRA surface
without pushing decoder-native details up into the host.
