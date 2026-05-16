# Seek Design

This document describes the current seek path in `semi_player_rs` and the target seek strategy for the next implementation stage.

For the broader playback pipeline, see [pipeline.md](pipeline.md).
For synchronization rules, see [sync.md](sync.md).

## 1. Scope

This seek design is optimized for the actual high-frequency user actions in the current product shape:

- keyboard seek
- mouse click seek on the progress bar

It is not primarily designed for:

- drag-preview scrubbing
- continuous thumbnail-preview seek

Those already have a separate thumbnail path and should not dictate the playback-core seek model.

## 2. Current Seek Path

Current entrypoint:

- [`semi_player_rs/src/lib.rs`](../../semi_player_rs/src/lib.rs)
- `semi_player_seek(...)`

Current behavior:

```text
seek(target)
  -> take playback coordination lock
  -> call FFmpeg input.seek(target)
  -> flush audio/video decoder state
  -> bump media generation
  -> clear runtime queues
  -> clear audio backend buffer
  -> move audio clock to target
  -> reset video scheduler + video sync
  -> wake workers
  -> decode worker refills
  -> sync worker rebuilds playback state
```

Important properties of the current path:

- correctness-first
- no explicit buffered seek path
- no explicit keyframe-anchored policy owned by the player
- no dedicated seek-recovery mode
- no audio trim to the exact target point
- no seek-specific optimization to skip expensive work on pre-target frames

This makes the current behavior simple and safe, but it is still closer to "reset and refill" than to an intentionally optimized player seek pipeline.

## 3. Design Goals

The next-stage seek design should optimize for:

1. fast user-visible response
2. stable recovery into normal playback state
3. predictable behavior under worker ownership
4. correctness preserved under concurrent decode activity

In concrete terms, the player should aim for:

- fast first-frame response after seek
- quick audio restart at the target point
- short settle time back into stable A/V sync
- no stale decode contamination after seek

## 4. Core Strategy

The default strategy should be:

```text
performance-first, keyframe-anchored, playback-oriented seek
```

That means:

1. use a local buffered reposition path only when it is obviously cheap and safe
2. otherwise seek to a recoverable point near the target, usually the nearest previous keyframe
3. forward-decode from that point to the target
4. rebuild audio/video playback state from the target, not from the anchor point

The player should treat seek as:

- a playback recovery operation
- not as a frame-preview scrub operation

## 5. Target Seek Modes

### 5.1 Buffered Seek

Use only when:

- the target lies inside a currently useful decoded window
- the player can rebuild state without a real FFmpeg seek

This is an optimization path, not the main design center.

### 5.2 Keyframe-Anchored Seek

This should be the main path for local playback.

Behavior:

- seek FFmpeg backward to a recoverable point
- prefer keyframe-aligned recovery
- flush decoder state
- enter seek recovery
- decode forward until the player reaches the target point

This should be the default because it is:

- fast enough
- stable
- predictable across common H.264/H.265 local media

## 6. Seek Recovery Model

Seek should gain an explicit recovery phase instead of behaving like an ordinary full refill.

Target model:

```text
seek request
  -> anchor seek
  -> recovery decode
  -> establish post-seek audio/video start points
  -> transition back to normal worker-owned playback
```

The recovery phase should make different decisions than normal steady-state decode.

### 6.1 Video during recovery

Before the target point:

- decode frames as needed to advance the decoder
- avoid expensive final processing for frames that will never be displayed
- avoid promoting pre-target frames into the normal visible playback path unless needed for fallback

This is especially important in the current CPU path because the player still converts displayed video into BGRA.

### 6.2 Audio during recovery

Before the target point:

- do not treat pre-target audio as normal playback-ready audio
- identify the frame that contains the target point
- trim audio samples so playback resumes from the target point, not from the beginning of the anchor-region audio frame

This matters because the project is building a real player, not just a frame-stepper. Audio restart quality is part of the seek feel.

## 7. State and Concurrency Rules

Seek already depends on:

- playback coordination lock
- player operation lock
- media generation guard

That should remain the correctness baseline.

The next step is not to remove correctness barriers first.
The next step is to reduce work performed while those barriers are held.

Near-term rules:

- seek must still invalidate stale decode work through media generation
- seek should clear only the state that must be invalidated immediately
- seek recovery should move more rebuilding work out of the hottest lock-held section over time

## 8. Measurement

Seek work should be driven by explicit metrics, not only by feel.

The first measurement set should include:

- API seek call duration
- first video frame after seek latency
- first audible audio after seek latency
- stable post-seek A/V settle time

The project should track both:

- seek correctness
- seek speed

so that a faster implementation does not silently become sloppier.

## 9. Non-Goals For This Stage

This stage should not try to solve everything at once.

Explicit non-goals:

- drag-preview scrub pipeline
- hardware decode integration as a prerequisite for better seek
- fully generic host-side preview semantics
- broad lock refactors unrelated to seek hot paths

Hardware decode may help later, especially for high-resolution content, but the immediate seek gains should come from a better recovery pipeline first.

## 10. Near-Term Implementation Plan

Recommended order:

1. document the current seek path and target recovery model
2. add seek latency and settle metrics
3. introduce explicit seek recovery state
4. implement keyframe-anchored recovery as the default real-seek path
5. trim audio to the target point during recovery
6. avoid expensive video post-processing on pre-target frames
7. add a lightweight buffered-seek fast path where it is clearly worthwhile

## 11. Summary

The current seek path is safe but blunt.

The next seek architecture should become:

- keyframe-anchored
- recovery-oriented
- performance-first
- still correctness-guarded by generation and worker coordination

That is the best fit for the current product shape:

- real playback
- worker-owned timing
- keyboard/progress-bar seek
- no drag-preview requirement
