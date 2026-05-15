# SemiPlayer Documentation

This directory contains detailed developer and environment documentation for the SemiPlayer project.

For high-level architecture goals, system boundaries, and design philosophy, see the root-level [ARCHITECTURE.md](../ARCHITECTURE.md).

## Directory Layout

```text
docs/
├── env/          # Development environment setup guides
├── dev/          # Implementation details and internal mechanics
└── adr/          # Architecture Decision Records
```

## Documents

### Environment

| Document | Purpose |
|---|---|
| [env/windows.md](env/windows.md) | Windows development baseline: FFmpeg package, Rust build, .NET smoke test verification |

### Developer Guides

| Document | Purpose |
|---|---|
| [dev/pipeline.md](dev/pipeline.md) | Current decoding pipeline and frame output flow (decoder drain queue, BGRA conversion, pump loop, FFI output) |
| [dev/sync.md](dev/sync.md) | Audio/video synchronization model (planned — currently audio clock is software-based) |
| [dev/ffmpeg-usage.md](dev/ffmpeg-usage.md) | How the project calls FFmpeg: init, demux, decode, seek, format mapping (planned) |
| [dev/abi.md](dev/abi.md) | C ABI reference for host integration: handle lifecycle, state queries, frame copy (planned) |

### Architecture Decision Records

| ADR | Topic |
|---|---|
| [adr/001-decoder-drain-queue.md](adr/001-decoder-drain-queue.md) | Why `OpenedMedia` uses an internal `pending_outputs` queue and explicit decoder draining |

## Maintenance Rules

- **Code changes must update docs**: If you modify the decoding pipeline, update `dev/pipeline.md`. If you change the public C ABI, update `dev/abi.md`.
- **ADRs are append-only**: Once an ADR is accepted, do not rewrite its decision. Add a new ADR if the decision changes.
- **Environment docs are platform-scoped**: Add `docs/env/linux.md` or `docs/env/macos.md` when those baselines are verified. Do not put cross-platform speculation in `windows.md`.
