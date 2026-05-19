# C ABI Reference

This document describes the public C ABI exposed by `semi_player_rs` for host integration.

The ABI is handle-based and command-oriented. All strings are UTF-8. Functions return explicit error codes.

## Error Codes

File: [`semi_player_rs/src/api/error.rs`](../../semi_player_rs/src/api/error.rs)

| Code | Value | Meaning |
|---|---|---|
| `SEMI_OK` | `0` | Success |
| `SEMI_E_INVALID_ARG` | `-1` | Null pointer or bad argument |
| `SEMI_E_INVALID_STATE` | `-2` | Operation not valid in current player state |
| `SEMI_E_MEDIA_OPEN_FAILED` | `-3` | Failed to open input file |
| `SEMI_E_MEDIA_PROBE_FAILED` | `-4` | Failed to probe stream info |
| `SEMI_E_DECODER_OPEN_FAILED` | `-5` | Failed to open decoder |
| `SEMI_E_SEEK_FAILED` | `-6` | Seek operation failed |

## Lifecycle

```c
int semi_player_create(SemiPlayerHandle** out_player);
void semi_player_destroy(SemiPlayerHandle* player);
```

`SemiPlayerHandle` is an opaque pointer. The host must not dereference or modify it.

## Playback Control

```c
int semi_player_open(SemiPlayerHandle* player, const char* path_utf8);
int semi_player_play(SemiPlayerHandle* player);
int semi_player_pause(SemiPlayerHandle* player);
int semi_player_seek(SemiPlayerHandle* player, int64_t position_ms, int exact);
int semi_player_reset(SemiPlayerHandle* player);
```

## Configuration

```c
int semi_player_set_speed(SemiPlayerHandle* player, double speed);
int semi_player_set_video_presentation_bias_ms(SemiPlayerHandle* player, int32_t bias_ms);
int semi_player_set_subtitle_visible(SemiPlayerHandle* player, int visible);
```

`presentation_bias_ms` allows the host to compensate for its own display pipeline latency.

## State Queries

```c
int semi_player_get_state(SemiPlayerHandle* player, uint32_t* out_state);
int semi_player_get_position_ms(SemiPlayerHandle* player, int64_t* out_position_ms);
int semi_player_get_duration_ms(SemiPlayerHandle* player, int64_t* out_duration_ms);
int semi_player_get_media_info(SemiPlayerHandle* player, SemiMediaInfo* out_media_info);
```

## Playback Snapshot

```c
int semi_player_get_playback_snapshot(SemiPlayerHandle* player, SemiPlaybackSnapshot* out_snapshot);
```

## Video Frame Output

The host can query the currently selected video frame at any time:

```c
int semi_player_get_current_video_frame_info(
    SemiPlayerHandle* player,
    SemiVideoFrameInfo* out_frame_info
);

int semi_player_copy_current_video_frame_bgra(
    SemiPlayerHandle* player,
    uint8_t* destination,
    uint32_t destination_len
);
```

Typical usage:

1. Ensure the player is opened and playing.
2. Call `semi_player_get_current_video_frame_info` to read metadata (size, stride, byte length).
3. Allocate a buffer of at least `byte_len` bytes.
4. Call `semi_player_copy_current_video_frame_bgra` to copy BGRA pixels into the buffer.
5. Present the buffer using the host's rendering pipeline.

### SemiVideoFrameInfo

| Field | Type | Description |
|---|---|---|
| `pts_ms` | `int64_t` | Presentation timestamp in milliseconds |
| `duration_ms` | `int64_t` | Frame duration in milliseconds |
| `width` | `uint32_t` | Frame width in pixels |
| `height` | `uint32_t` | Frame height in pixels |
| `stride` | `uint32_t` | Bytes per scanline (may include padding) |
| `pixel_format` | `uint32_t` | Currently always `4` (`Bgra8`) |
| `byte_len` | `uint32_t` | Total size of pixel data |
| `flags` | `uint32_t` | `1` if key frame |

## Utility

```c
void semi_free_string(char* s);
char* semi_ffmpeg_version_string(void);
```

Strings returned by the library (e.g., `semi_ffmpeg_version_string`) must be freed with `semi_free_string`.

## Threading Notes

The player now owns an internal sync worker thread for playback progression.

Current threading rules:

- FFI calls are still processed synchronously on the calling thread
- internal playback progression is fully player-owned
- mutable access is serialized inside the player handle
- hosts should still avoid issuing concurrent control calls against the same handle unless they intentionally want serialized behavior
