# SemiPlayer Documentation

This directory contains detailed developer and environment documentation for the SemiPlayer project.

For high-level architecture goals, system boundaries, and design philosophy, see the root-level [ARCHITECTURE.md](../ARCHITECTURE.md).

## Directory Layout

```text
docs/
  env/          Development environment setup guides
  dev/          Implementation details and internal mechanics
  adr/          Architecture Decision Records
```

## Documents

### Environment

| Document | Purpose |
|---|---|
| [env/windows.md](env/windows.md) | Windows development baseline: FFmpeg package, Rust build, and .NET smoke verification |

### Developer Guides

| Document | Purpose |
|---|---|
| [dev/pipeline.md](dev/pipeline.md) | Current decoding pipeline and frame output flow |
| [dev/sync.md](dev/sync.md) | Current audio/video synchronization model |
| [dev/internal-video-sync.md](dev/internal-video-sync.md) | Planned player-owned internal video sync system that reduces timing dependence on external `pump` calls |
| [dev/ffmpeg-usage.md](dev/ffmpeg-usage.md) | How the project calls FFmpeg: init, demux, decode, seek, and format mapping |
| [dev/abi.md](dev/abi.md) | C ABI reference for host integration: handle lifecycle, state queries, and frame copy |
| [dev/d3d11-libplacebo-render.md](dev/d3d11-libplacebo-render.md) | Planned Windows hardware render path from FFmpeg D3D11 NV12 decode surfaces to D3D11 BGRA presentation surfaces |

### Architecture Decision Records

| ADR | Topic |
|---|---|
| [adr/001-decoder-drain-queue.md](adr/001-decoder-drain-queue.md) | Why `OpenedMedia` uses an internal `pending_outputs` queue and explicit decoder draining |

## Maintenance Rules

- **Code changes must update docs**: If you modify the decoding pipeline, update `dev/pipeline.md`. If you change the public C ABI, update `dev/abi.md`. If you change synchronization ownership or timing semantics, update `dev/sync.md` and `dev/internal-video-sync.md` as needed.
- **ADRs are append-only**: Once an ADR is accepted, do not rewrite its decision. Add a new ADR if the decision changes.
- **Environment docs are platform-scoped**: Add `docs/env/linux.md` or `docs/env/macos.md` when those baselines are verified. Do not put cross-platform speculation in `windows.md`.
