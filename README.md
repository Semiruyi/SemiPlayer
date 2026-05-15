# SemiPlayer

A cross-platform media playback core written in Rust, exposing a C ABI for UI hosts such as WPF and Avalonia.

## Quick Links

| Document | What it covers |
|---|---|
| [ARCHITECTURE.md](ARCHITECTURE.md) | High-level goals, system boundaries, platform strategy |
| [docs/dev/pipeline.md](docs/dev/pipeline.md) | Current decoding pipeline and frame output flow |
| [docs/adr/001-decoder-drain-queue.md](docs/adr/001-decoder-drain-queue.md) | Why the decoder uses an internal drain queue |
| [docs/env/windows.md](docs/env/windows.md) | Windows dev environment setup |
| [TODO.md](TODO.md) | Current progress and milestones |

## Repository Layout

```text
Semi/
├── semi_player_rs/              # Rust playback core (cdylib + staticlib + rlib)
│   └── src/
│       ├── lib.rs               # C ABI exports
│       ├── api/                 # C-compatible types and error codes
│       ├── core/                # Platform-neutral player logic
│       ├── render/              # Video frame scheduling and backends
│       ├── audio/               # Audio clock and backends
│       ├── subtitle/            # Subtitle processing (planned)
│       ├── platform/            # OS-specific glue (planned)
│       └── util/                # Time helpers
├── docs/                        # Developer and environment docs
├── third_party/ffmpeg/          # Local FFmpeg package
└── tools/smoke/                 # .NET interop smoke test
```

## Verified Development Baseline

- **OS**: Windows 11
- **FFmpeg**: 7.1 shared build
- **Rust**: `ffmpeg-next = 8.1`, `x86_64-pc-windows-msvc`
- **UI host**: .NET  smoke test

See [docs/env/windows.md](docs/env/windows.md) for full setup instructions.

## Build

```powershell
cd semi_player_rs
cargo build
```

## Smoke Test

```powershell
cd tools/smoke/SemiPlayer.HelloTest
dotnet run
```

## Architecture Principle

The Rust core owns demux, decode, sync, and frame output contracts. Platform-specific rendering and presentation live behind backend and adapter boundaries. The core does not know whether the consumer is WPF, Avalonia, or a future host.
