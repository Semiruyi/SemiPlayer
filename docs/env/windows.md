# Windows Development Baseline

This document defines the current Windows development baseline used by this repository.

It is intended to be the source of truth for the currently verified Windows setup:

- the FFmpeg package selection
- the Rust build environment
- the .NET runtime loading path used by local verification

For overall system design, see:

- [ARCHITECTURE.md](../../ARCHITECTURE.md)

This document is not a cross-platform environment specification.

It describes only the first verified development environment.

## 1. Scope

This baseline covers the current Windows development environment for:

1. command-line `ffmpeg`
2. Rust crate `semi_player_rs`
3. .NET verification host `tools/smoke/SemiPlayer.HelloTest`

It does not define the final cross-platform packaging model.

## 2. Selected FFmpeg Distribution

The repository currently uses a local shared FFmpeg package located at:

```text
C:\y-s\project\Semi\third_party\ffmpeg\current\ffmpeg
```

Important directories:

```text
bin\        ffmpeg.exe and runtime DLLs
include\    FFmpeg headers
lib\        import libraries used by Rust linking
```

## 3. Compatibility Baseline

The currently verified combination is:

- FFmpeg shared build: `7.1`
- Rust wrapper crate: `ffmpeg-next = 8.1`
- Rust toolchain family: `x86_64-pc-windows-msvc`

This combination has been verified to:

- compile the Rust crate successfully
- load the Rust DLL from .NET
- load the FFmpeg runtime DLLs from .NET
- execute `ffmpeg -version` from PowerShell

## 4. Why This Baseline Was Chosen

Earlier attempts uncovered compatibility problems with other combinations:

- newer `ffmpeg-master-latest` packages were too far ahead of the original binding setup
- some MinGW-oriented builds caused bindgen/layout mismatches with the MSVC Rust toolchain

The current baseline is not described as universally optimal. It is described only as the version set currently verified for Windows development in this repository.

## 5. Rust Build Configuration

Rust-side FFmpeg discovery is configured in:

- [`.cargo/config.toml`](../../.cargo/config.toml)

Current contents:

```toml
[env]
FFMPEG_DIR = { value = "C:/y-s/project/Semi/third_party/ffmpeg/current/ffmpeg", force = true }
LIBCLANG_PATH = { value = "C:/Program Files/LLVM/bin", force = true }
```

Meaning:

- `FFMPEG_DIR` points `ffmpeg-sys-next` at the selected FFmpeg installation
- `LIBCLANG_PATH` allows bindgen to locate `libclang`

## 6. Rust Dependency Baseline

The current Rust dependency direction is defined in:

- [`semi_player_rs/Cargo.toml`](../../semi_player_rs/Cargo.toml)

Current FFmpeg-related dependency:

```toml
ffmpeg-next = { version = "8.1", features = ["codec", "format", "software-resampling", "software-scaling"] }
```

If this version changes, this document should be updated together with:

- `ARCHITECTURE.md`
- any related interop verification expectations

## 7. .NET Verification Host Behavior

The current .NET verification host is:

- [`tools/smoke/SemiPlayer.HelloTest`](../../tools/smoke/SemiPlayer.HelloTest)

Its project file currently copies:

- `semi_player_rs.dll`
- FFmpeg runtime DLLs from the selected `bin` directory

Reference:

- [`tools/smoke/SemiPlayer.HelloTest/SemiPlayer.HelloTest.csproj`](../../tools/smoke/SemiPlayer.HelloTest/SemiPlayer.HelloTest.csproj)

This arrangement exists for local Windows verification only. It is not yet the final deployment strategy.

## 8. User PATH Requirement

To make `ffmpeg` available from a new terminal, the user PATH should contain:

```text
C:\y-s\project\Semi\third_party\ffmpeg\current\ffmpeg\bin
```

Verification command:

```powershell
ffmpeg -version
```

If PowerShell does not recognize `ffmpeg`, reopen the terminal and test again.

## 9. Verification Commands

### 9.1 Command-line FFmpeg

```powershell
cd C:\y-s\project\Semi
ffmpeg -version
```

### 9.2 Rust Build

```powershell
cd C:\y-s\project\Semi\semi_player_rs
cargo build
```

### 9.3 .NET Verification Host

```powershell
cd C:\y-s\project\Semi\tools\smoke\SemiPlayer.HelloTest
dotnet run
```

## 10. Repository Files That Define This Baseline

- [`.cargo/config.toml`](../../.cargo/config.toml)
- [`semi_player_rs/Cargo.toml`](../../semi_player_rs/Cargo.toml)
- [`tools/smoke/SemiPlayer.HelloTest/SemiPlayer.HelloTest.csproj`](../../tools/smoke/SemiPlayer.HelloTest/SemiPlayer.HelloTest.csproj)
- [ARCHITECTURE.md](../../ARCHITECTURE.md)
- [docs/env/windows.md](windows.md)

## 11. Change Management Rules

If any of the following change:

- FFmpeg package path
- FFmpeg major/minor version family
- Rust FFmpeg wrapper version
- runtime DLL copy strategy

then update this document in the same change.

This helps keep the Windows development baseline aligned with architecture notes and local verification steps.

## 12. Common Failure Modes

### `ffmpeg` is not recognized

Likely cause:

- the current terminal was opened before PATH was updated

Action:

1. close the current terminal
2. open a new PowerShell window
3. run `ffmpeg -version`

### `cargo build` fails with FFmpeg header or bindgen errors

Check:

1. `FFMPEG_DIR` still points to the selected 7.1 shared package
2. `C:\Program Files\LLVM\bin` still exists
3. the project was not switched to a different FFmpeg distribution without updating configuration

Useful commands:

```powershell
cd C:\y-s\project\Semi
Get-Content .\.cargo\config.toml
Get-ChildItem .\third_party\ffmpeg\current\ffmpeg
```

### `.NET` host fails because `semi_player_rs.dll` is missing

Likely cause:

- Rust build has not completed successfully

Action:

```powershell
cd C:\y-s\project\Semi\semi_player_rs
cargo build

cd ..\tools\smoke\SemiPlayer.HelloTest
dotnet run
```

### `.NET` host fails because FFmpeg DLLs are missing

Check whether the `.csproj` still copies FFmpeg runtime DLLs from the selected `bin` folder.

## 13. Notes

- The current verification host is transitional and will evolve as the real player API replaces the smoke-test scaffolding.
- This document should stay focused on the verified Windows development baseline, not on future packaging or cross-platform release assumptions.
