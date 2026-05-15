# FFmpeg Usage in SemiPlayer

This document summarizes how `semi_player_rs` calls FFmpeg through the `ffmpeg-next` crate.

For the overall decoding flow, see [pipeline.md](pipeline.md).

## Dependency

[`semi_player_rs/Cargo.toml`](../../semi_player_rs/Cargo.toml):

```toml
ffmpeg-next = { version = "8.1", features = ["codec", "format", "software-resampling", "software-scaling"] }
```

The local FFmpeg package lives in `third_party/ffmpeg/current/ffmpeg` and is discovered via `FFMPEG_DIR` in `.cargo/config.toml`.

## Core Operations

### 1. Initialization

```rust
ffmpeg::init()
```

Called once before any other FFmpeg operation. Maps to `avformat_network_init` and related global setup.

### 2. Open Input (Demux)

```rust
let input = ffmpeg::format::input(&path)?;
```

Maps to `avformat_open_input` + `avformat_find_stream_info`. Returns an `Input` context that owns the `AVFormatContext`.

### 3. Stream Probing

```rust
let best_video = context.streams().best(ffmpeg::media::Type::Video);
```

Maps to `av_find_best_stream`. The project also iterates all streams to collect metadata (width, height, sample rate, channels) via `stream.parameters()`.

### 4. Decoder Setup

```rust
let context = ffmpeg::codec::context::Context::from_parameters(stream.parameters())?;
let mut decoder = context.decoder();
decoder.set_packet_time_base(stream.time_base());
let decoder = decoder.video()?; // or .audio()?
```

Maps to `avcodec_parameters_to_context` and `avcodec_open2`. The time base must be explicitly copied from the stream so that frame timestamps are interpreted correctly.

### 5. Packet Reading

```rust
let mut packet = Packet::empty();
packet.read(&mut input)?;
```

Maps to `av_read_frame`. Returns `Err(Eof)` when the file is exhausted.

### 6. Decode: Send / Receive

The project uses the modern push-pull API:

```rust
decoder.send_packet(&packet)?;
// loop:
decoder.receive_frame(&mut frame)?;
```

Maps to `avcodec_send_packet` / `avcodec_receive_frame`. A single `send_packet` can require multiple `receive_frame` calls. The project now handles this correctly via bulk collection (see [pipeline.md](pipeline.md)).

### 7. Seek

```rust
self.input.seek(position, ..)?;
```

Maps to `avformat_seek_file` or `av_seek_frame`. After seeking, decoders must be flushed with `decoder.flush()`.

### 8. End-of-File Flush

When the input is exhausted, the project sends an EOF packet to each decoder:

```rust
decoder.send_eof()?;
```

This signals the decoder to emit any internally buffered frames. The project then loops `receive_frame` until both decoders return EOF.

### 9. Pixel Format Conversion

```rust
ffmpeg_next::software::scaling::Context::get(
    input_format, input_width, input_height,
    output_format, output_width, output_height,
    ScalingFlags::BILINEAR,
)?;
```

Maps to `sws_getContext`. Currently used to convert decoded video frames to BGRA for host consumption.

### 10. Time Conversion

All internal time is in microseconds. FFmpeg uses `AV_TIME_BASE` (1/1_000_000) for container-level duration and stream-specific `time_base` for packet/frame timestamps. The project uses `Rational::rescale` to convert between these bases.

## Format Mapping

Decoded pixel formats and sample formats are mapped to internal enums (`PixelFormatCategory`, `AudioSampleFormatCategory`) so that upper layers do not depend directly on FFmpeg types.
