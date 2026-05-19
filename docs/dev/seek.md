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
  -> take playback coordination gate
  -> take player operation lock for seek prepare
  -> release player operation lock
  -> call FFmpeg input.seek(target)
  -> retake player operation lock for seek commit
  -> clear runtime and audio state
  -> bump media generation
  -> move audio clock to target and reset sync state
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

The current implementation baseline now also includes:

- explicit seek timing metrics
- target-aware seek video recovery metrics
- a first video fast path during seek recovery that skips BGRA conversion for pure pre-target pass-through frames
- anchor diagnostics that compare the actual FFmpeg recovery point against the expected nearest-left video keyframe
- direct recovery work accounting for:
  - video vs audio decode-side cost
  - packet read cost
  - poll-loop cost and call counts

Current diagnostic findings:

- FFmpeg seek placement is currently good enough for the main local-playback path; observed actual anchor packets match the expected nearest-left main-video keyframe in tested samples
- pure pre-target video frames already avoid the most expensive CPU playback work because they skip BGRA conversion, frame copy, runtime video queue insertion, and sync dirties
- pre-target audio trimming was moved early enough that audio recovery no longer appears to be a meaningful seek bottleneck in current smoke measurements
- direct seek-cost measurements now show that the dominant steady seek cost is forward video recovery from the correct left keyframe, not audio recovery, not reset, and not demux packet read
- the next optimization stage should therefore focus less on "did FFmpeg pick the right keyframe?" and more on "how expensive is forward video recovery from the correct anchor?"
- the first lock refactor stage now keeps FFmpeg seek itself outside the player-wide operation lock while still preserving the outer playback coordination gate

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

Current implementation direction:

- seek enters a player-owned recovery state
- decode polling receives a seek-recovery policy derived from that state
- if a decoded video's display interval ends before the seek target, it is treated as decoder-advance-only work
- those pure pre-target frames are not converted to BGRA and are not enqueued into the normal runtime video queue

This is the first practical fast path because the expensive work currently happens in the FFmpeg-facing media layer, before runtime queue logic ever sees the frame.

Current assessment:

- the video side is already close to the intended recovery shape
- remaining pre-target video cost is now mostly codec forward recovery itself
- packet read cost is small in current measurements
- the dominant measured cost sits in the FFmpeg video decode feed path during recovery
- video may still have smaller framework costs such as frame mapping/copy and `SkippedVideo` bookkeeping, but they are not the first-order bottleneck

### 6.2 Audio during recovery

Before the target point:

- do not treat pre-target audio as normal playback-ready audio
- identify the frame that contains the target point
- trim audio samples so playback resumes from the target point, not from the beginning of the anchor-region audio frame

This matters because the project is building a real player, not just a frame-stepper. Audio restart quality is part of the seek feel.

Current implementation direction:

- audio frames fully before the seek target should not enter normal post-seek playback
- the first audio frame that overlaps the seek target should be trimmed at a sample boundary before entering the runtime queue
- this is currently implemented as a runtime-apply trim step, which is enough to correct audible restart position before deeper seek-pipeline work lands

Current assessment:

- the audio side no longer appears to be the limiting path for playing seek
- current smoke diagnostics show audio decode-side cost is tiny compared with video decode-side cost during the same seek
- audio restart and gating are still correctness-sensitive, but audio-specific seek optimization is no longer the primary performance track

## 7. State and Concurrency Rules

Seek already depends on:

- playback coordination lock
- player operation lock
- media generation guard

That should remain the correctness baseline.

The next step is not to remove correctness barriers first.
The next step is to reduce work performed while those barriers are held.

Current implementation note:

- the outer playback coordination gate is still held across the full seek operation
- the player operation lock is now split into prepare and commit sections, with FFmpeg seek running outside that lock
- keyframe-relative seek entrypoints now follow the same prepare / execute / commit structure as direct seek

Near-term rules:

- seek must still invalidate stale decode work through media generation
- seek should clear only the state that must be invalidated immediately
- seek recovery should move more rebuilding work out of the hottest lock-held section over time

## 8. Measurement

Seek work should be driven by explicit metrics, not only by feel.

The measurement model should be split into two layers:

### 8.1 Result Metrics

These are the user-visible or player-visible outcome metrics that answer:

```text
how good did the seek feel?
```

The first result set should include:

- API seek call duration
- first video frame after seek latency
- first audible audio after seek latency
- stable post-seek A/V settle time

The project should track both:

- seek correctness
- seek speed

so that a faster implementation does not silently become sloppier.

### 8.2 Stage Timing Metrics

Result metrics alone are not enough.

They can show that seek is slow, but they cannot explain which stage is slow.

The player should therefore also record internal stage timestamps and compute per-stage durations.

Recommended timestamp points:

- `seek_requested_at`
- `seek_lock_acquired_at`
- `ffmpeg_seek_started_at`
- `ffmpeg_seek_finished_at`
- `seek_reset_finished_at`
- `first_post_seek_video_decoded_at`
- `first_post_seek_audio_decoded_at`
- `target_video_ready_at`
- `target_audio_ready_at`
- `seek_stable_at`

Recommended derived stage durations:

- lock wait duration
- FFmpeg seek duration
- immediate reset duration
- decode-to-first-video duration
- decode-to-first-audio duration
- target-video-ready duration
- target-audio-ready duration
- stable-settle duration

These metrics should answer:

- is the cost dominated by lock wait?
- is the FFmpeg seek itself expensive?
- is forward decode to the target point expensive?
- is video post-processing the problem?
- is audio recovery the problem?
- is refill-to-stable-state the problem?

The seek diagnostics should also retain recovery-shape data such as:

- first decoded video PTS after seek
- first current-video PTS after seek
- target-video PTS when the seek first becomes video-ready
- pre-target decoded video count
- pre-target current-frame promotion count

The next measurement pass should add explicit "work-accounting" metrics for recovery, especially:

- pre-target video decoded count vs queued/current/promoted count
- pre-target audio decoded count vs queued/submitted/discarded count
- how many pre-target audio frames were fully resampled and copied before being discarded
- reset sub-stage timings such as runtime clear, audio backend clear, clock move, and sync/scheduler reset
- the delay from first post-target decoded video to first post-target current frame
- the delay from seek reset completion to first submitted post-target audio chunk

Those metrics should answer two concrete questions before code changes land:

- is pre-target audio still doing playback work instead of only recovery work?
- is reset itself, or the rebuild work caused by reset, a primary blocker in end-to-end seek latency?

### 8.3 Core-Internal vs End-to-End

The first implementation stage should focus on:

- core-internal metrics

That means the Rust player should first measure:

- when internal milestones are reached
- not when the host actually displays or audibly renders them

Later, the host may add end-to-end timestamps such as:

- first displayed frame after seek
- first heard audio after seek

But those should be a second-stage validation layer, not a prerequisite for improving the core seek path.

## 9. Non-Goals For This Stage

This stage should not try to solve everything at once.

Explicit non-goals:

- drag-preview scrub pipeline
- hardware decode integration as a prerequisite for understanding current recovery costs
- fully generic host-side preview semantics
- broad lock refactors unrelated to seek hot paths

Hardware decode may help later, especially for high-resolution content, but the immediate seek gains should come from a better recovery pipeline first.

## 10. Near-Term Implementation Plan

Recommended order:

1. document the current seek path and target recovery model
2. add seek result metrics and internal stage timing metrics
3. introduce explicit seek recovery state
4. implement keyframe-anchored recovery as the default real-seek path
5. avoid expensive video post-processing on pure pre-target frames
6. add recovery work-accounting metrics for pre-target video, pre-target audio, and reset sub-stages
7. move seek-audio target gating earlier so fully pre-target audio can be discarded before full playback-grade conversion
8. review reset granularity and remove obviously unnecessary rebuild work from the hot seek path
9. add a lightweight buffered-seek fast path only if later measurements show a clear payoff

## 11. Summary

The current seek path is safe but blunt.

The next seek architecture should become:

- keyframe-anchored
- recovery-oriented
- performance-first
- still correctness-guarded by generation and worker coordination

Near-term optimization work should assume:

- the current FFmpeg anchor choice is not the main local seek problem
- pure pre-target video is already on a mostly-correct fast path
- pre-target audio and reset/rebuild costs are the most suspicious remaining recovery blockers

That is the best fit for the current product shape:

- real playback
- worker-owned timing
- keyboard/progress-bar seek
- no drag-preview requirement
