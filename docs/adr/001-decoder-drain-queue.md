# ADR 001: Decoder Drain Queue and Bulk Frame Collection

- **Status**: Accepted
- **Date**: 2026-05-15
- **Context**: Working-tree changes to `semi_player_rs/src/core/media/opened.rs`

## Context

The initial decoder implementation in `OpenedMedia::next_decoded_output` used a one-to-one model:

1. Read one packet.
2. Send it to the video or audio decoder.
3. Receive at most one frame.
4. Return that frame immediately.

This model had two problems:

1. **Dropped frames**: FFmpeg's `avcodec_send_packet` / `avcodec_receive_frame` API is push-pull. A single packet can produce zero, one, or multiple frames. Receiving only once per packet silently dropped subsequent frames, which caused visible frame skipping and A/V desync on files with B-frames.
2. **Incorrect EAGAIN handling**: When `send_packet` returned `EAGAIN`, the code ignored it and moved to the next packet. This meant the packet was never submitted to the decoder, causing a decode gap.

Additionally, end-of-file handling was naive: as soon as `av_read_frame` returned EOF, the function emitted `EndOfStream`. This left decoded frames buffered inside the decoder that were never retrieved.

## Decision

We changed `next_decoded_output` to use an internal `VecDeque<DecodedOutput>` called `pending_outputs` and introduced explicit **drain** semantics.

### 1. Internal Queue

`OpenedMedia` now owns `pending_outputs`. On every call to `next_decoded_output`:

- If the queue has items, pop one and return it.
- If the queue is empty, read/decode until at least one item is produced.

This decouples the "one output per call" API contract from FFmpeg's variable packet-to-frame ratio.

### 2. Bulk Collection

Two new helpers, `collect_video_frames` and `collect_audio_frames`, loop `receive_frame` until `EAGAIN`:

```rust
fn collect_video_frames(
    decoder: &mut OpenedVideoDecoder,
    outputs: &mut VecDeque<DecodedOutput>,
    draining: bool,
) -> Result<bool, MediaOpenError>
```

They push every successfully received frame into `outputs`. The boolean return indicates whether the decoder has fully drained (relevant only when `draining = true`).

### 3. EAGAIN Retry

When `send_packet` returns `EAGAIN`:

1. Call the corresponding `collect_*_frames` to drain already-decoded frames.
2. If no frames were produced, propagate the error.
3. Otherwise retry `send_packet`.

### 4. Draining Mode

When packet reading reaches EOF:

1. `enter_draining_mode` sends `send_eof` to both decoders.
2. `collect_drained_outputs` repeatedly calls the bulk collectors with `draining = true`.
3. When both decoders report fully drained, emit exactly one `EndOfStream`.

This ensures all internally buffered frames are surfaced before playback ends.

## Consequences

### Positive

- **No more dropped frames**: All frames produced by a packet are captured.
- **Correct EOF behavior**: Decoder-internal buffers are fully flushed.
- **Clean API boundary**: `next_decoded_output` still returns one item per call, but the internal complexity of FFmpeg's decode model is hidden from the pump loop.

### Negative / Trade-offs

- **Memory spike potential**: A single packet can now enqueue many frames. For normal content this is bounded by FFmpeg's decoder latency (a few frames). For pathological content it could spike memory.
- **Slightly more state**: `OpenedMedia` now carries `DecoderDrainingState` and `pending_outputs`, increasing struct size.
- **No backpressure yet**: `pending_outputs` can grow without an explicit upper bound. The pump loop's `max_iterations` provides some indirect limit, but the queue itself is unbounded.

## Alternatives Considered

- **Return `Vec<DecodedOutput>` from decode functions**: Rejected because it would force the pump loop to handle variable-sized batches, complicating the state machine.
- **Use a fixed-size ring buffer**: Rejected for now because frame sizes vary and we want to keep the initial implementation simple. A bounded queue can be added later if memory profiling shows it is needed.

## Related Documents

- [docs/dev/pipeline.md](../dev/pipeline.md) — current decoding and frame output flow
- [`semi_player_rs/src/core/media/opened.rs`](../../semi_player_rs/src/core/media/opened.rs) — implementation
