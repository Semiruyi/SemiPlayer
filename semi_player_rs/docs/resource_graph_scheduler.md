# Semi Player Resource-Graph Scheduler Design

## Goal

Replace the current point-to-point worker wake flow with a single scheduling model that can scale to:

1. multi-stage video preparation
2. multi-stage audio preparation
3. subtitle rendering
4. composition
5. future multi-threaded decode and render workers

The new model should make one thing explicit:

- playback consumes resources
- stages produce resources
- the scheduler decides which stage should run next based on resource shortage

This document is the implementation target for the next refactor.

## Why Change

The current worker chain is already showing structural limits:

1. `sync` observes playback-side shortage
2. `render` only understands part of presentation supply
3. `decode` only reacts to decode-local demand
4. workers wake each other directly

That creates hidden ownership gaps. A typical failure mode is:

- playback audio is empty
- video presentation still has frames
- `sync` asks `render` for help
- `render` sees no video work and waits
- nobody requests upstream work for audio

The problem is not a single conditional. The problem is that the system has no single place that owns resource-shortage resolution.

## Design Principles

1. Workers do execution, not global reasoning.
2. The scheduler owns the global state machine.
3. Scheduling is driven by resource shortage, not by hardcoded worker-to-worker chains.
4. Audio and video should be modeled symmetrically where possible.
5. The first implementation should use fixed enums and explicit Rust logic, not a generic runtime DAG engine.
6. Logging must be rich enough to explain why a stage was or was not scheduled.

## Core Model

### Resource

A resource is a class of playback-relevant supply.

Initial resource set:

- `DecodedAudio`
- `DecodedVideo`
- `PresentationAudio`
- `PresentationVideo`

Likely later additions:

- `SubtitlePresentation`
- `CompositeVideo`
- `AudioDeviceBuffer`
- `VideoPresentQueue`

Suggested shape:

```rust
#[derive(Clone, Copy, Debug, Eq, PartialEq, Hash)]
pub enum ResourceKey {
    DecodedAudio,
    DecodedVideo,
    PresentationAudio,
    PresentationVideo,
}

#[derive(Clone, Copy, Debug, Default)]
pub struct ResourceState {
    pub available_units: usize,
    pub low_watermark: usize,
    pub high_watermark: usize,
    pub end_of_stream: bool,
    pub blocked: bool,
}
```

`available_units` does not need to be physically identical across all resources. It only needs to be meaningful inside each resource family:

- frame count for video queues
- sample frames or chunks for audio queues

### Stage

A stage consumes upstream resources and produces downstream resources.

Initial stage set:

- `AudioDecode`
- `VideoDecode`
- `AudioRender`
- `VideoRender`

Likely later additions:

- `SubtitleRender`
- `Composite`

Suggested shape:

```rust
#[derive(Clone, Copy, Debug, Eq, PartialEq, Hash)]
pub enum StageId {
    AudioDecode,
    VideoDecode,
    AudioRender,
    VideoRender,
}

#[derive(Clone, Copy, Debug, Default)]
pub struct StageState {
    pub in_flight: bool,
    pub blocked: bool,
    pub last_progress_generation: u64,
}
```

Each stage also has static topology metadata:

```rust
pub struct StageTopology {
    pub consumes: &'static [ResourceKey],
    pub produces: &'static [ResourceKey],
}
```

The topology is fixed in code for now. Do not build a dynamic graph loader in the first pass.

### Playback Demand

Playback does not ask for a worker. It asks for resources.

Suggested shape:

```rust
#[derive(Clone, Copy, Debug, Default)]
pub struct PlaybackDemand {
    pub needs_audio_now: bool,
    pub needs_video_now: bool,
    pub next_deadline_us: Option<i64>,
}
```

This is the scheduler-facing form of what `sync` currently discovers from:

- playback clock
- audio output headroom
- presentation video readiness
- player state

## Scheduler Responsibilities

The scheduler is the only place allowed to decide:

- which stage should be woken
- whether playback work should be woken
- whether the system should wait

The scheduler should not:

- perform decode
- perform render
- mutate runtime queues directly while deciding
- hold broad locks while doing execution

The scheduler is control plane only.

## Event Model

All workers and control flows should talk to the scheduler through events.

Suggested first event set:

```rust
#[derive(Clone, Debug)]
pub enum SchedulerEvent {
    PlaybackDemandChanged,
    PlaybackAdvanced,
    StageRequested(StageId),
    StageStarted(StageId),
    StageProgress {
        stage: StageId,
        produced: &'static [ResourceKey],
    },
    StageBlocked(StageId),
    StageIdle(StageId),
    SeekStarted,
    SeekCompleted,
    MediaLoaded,
    MediaUnloaded,
    PlayerStateChanged,
    ShutdownRequested,
}
```

Notes:

- `PlaybackDemandChanged` is the main signal from the current sync side.
- `StageProgress` means the stage made real forward progress and runtime supply changed.
- `StageBlocked` means the stage ran but cannot continue under current upstream conditions.
- `StageIdle` means the stage has no immediate work and is not blocked on execution.

The first version can keep this smaller if needed. The important thing is that workers stop directly coordinating with one another.

## Snapshot Model

The scheduler must decide from snapshots, not from cross-domain live mutation.

Suggested scheduler input snapshot:

```rust
pub struct SchedulerSnapshot {
    pub player_state: PlayerState,
    pub playback_demand: PlaybackDemand,
    pub resources: ResourceMap,
    pub stages: StageMap,
    pub media_loaded: bool,
    pub generation: u64,
}
```

The snapshot is assembled from existing domains:

- `runtime`
- `audio_output`
- `video_sync`
- `control`
- stage-local state if needed

Important rule:

- snapshot assembly may briefly lock several domains
- scheduler evaluation must happen after those locks are released

## Decision Model

The scheduler returns commands, not side effects.

Suggested shape:

```rust
#[derive(Clone, Debug, Default)]
pub struct SchedulerDecision {
    pub wake_playback: bool,
    pub wake_stages: Vec<StageId>,
    pub next_deadline_us: Option<i64>,
}
```

The first implementation can keep `Vec<StageId>`. If this becomes hot later, it can be replaced with a small fixed set or bitflags.

## Scheduling Strategy

### Reverse Demand Propagation

The scheduler should work backward from playback shortage.

Example:

1. playback needs `PresentationAudio`
2. producer stage is `AudioRender`
3. if `AudioRender` can run, wake it
4. if `AudioRender` cannot run because `DecodedAudio` is empty, wake `AudioDecode`

Likewise:

1. playback needs `PresentationVideo`
2. producer stage is `VideoRender`
3. if `VideoRender` can run, wake it
4. if `VideoRender` cannot run because `DecodedVideo` is empty, wake `VideoDecode`

This generalizes naturally when subtitle and composition stages appear.

### First-Pass Scheduling Rules

For the first implementation, use explicit rules in Rust:

1. If playback needs `PresentationAudio`:
   - wake `AudioRender` when decoded audio exists
   - otherwise wake `AudioDecode`
2. If playback needs `PresentationVideo`:
   - wake `VideoRender` when decoded video exists
   - otherwise wake `VideoDecode`
3. Never wake a stage already marked `in_flight`, unless we later add explicit multi-instance capacity.
4. If a stage reports progress that may satisfy playback demand, also wake playback.
5. If a stage reports blocked and upstream is empty, escalate to the producer stage.

This is intentionally explicit. The model is graph-shaped, but the first code should stay concrete and easy to debug.

## Worker Roles After Refactor

### Scheduler Worker

Owns:

- scheduler event queue
- scheduler state machine
- scheduler decisions

Does not own:

- queue mutation
- decode execution
- render execution
- audio output submission

### Playback Worker

Owns:

- playback clock advancement
- consuming presentation resources
- observing playback deadlines

Sends events:

- `PlaybackDemandChanged`
- `PlaybackAdvanced`
- `StageBlocked`-like signals if playback cannot proceed due to shortage

### Stage Workers

Each stage worker owns one execution concern:

- `AudioDecode`
- `VideoDecode`
- `AudioRender`
- `VideoRender`

Each stage worker:

1. waits for scheduler request
2. executes one bounded unit of work
3. commits results to its owned runtime area
4. reports event back to scheduler

Stage workers should not directly notify other stage workers.

## Locking Rules

This refactor will fail if lock boundaries are sloppy. These rules are mandatory.

1. The scheduler has its own state lock.
   - Example: `scheduler_state: Mutex<SchedulerState>`
2. The scheduler lock must never be held while doing decode or render work.
3. Worker execution must not hold multiple unrelated domain locks longer than necessary.
4. Snapshot collection may touch multiple locks, but only for read/copy.
5. Decision computation runs after snapshot locks are released.
6. Stage result commit should mutate its own target domain first, then emit a scheduler event.
7. No worker should call another worker directly after the refactor boundary is in place.

Target mental model:

- locks protect data domains
- scheduler events connect domains
- scheduler decisions wake executors

## Logging Requirements

This design is only worth doing if the logs explain system decisions.

### Scheduler Tick

Every scheduler evaluation should produce one high-value summary line:

```text
scheduler:tick event=PlaybackDemandChanged
  demand={audio_now=true video_now=false deadline=123456}
  resources={pa=0 pv=2 da=0 dv=3}
  stages={ar=idle vr=idle ad=idle vd=in_flight}
  shortages=[PresentationAudio]
  plan=[Wake(AudioDecode)]
```

The exact formatting can be flattened to one line, but the information density should stay.

### Stage Completion

```text
stage:complete stage=VideoDecode produced=DecodedVideo count=1 eos=false blocked=false
```

### Stage Blocked

```text
stage:block stage=AudioRender reason=MissingDecodedAudio
```

### Playback Starvation

```text
playback:starved missing=PresentationAudio
```

### State Transition

```text
scheduler:state old=Running new=Seeking reason=SeekStarted
```

Also note:

- `runtime-trace.log` currently shows write interleaving
- trace output should later be serialized behind a dedicated trace mutex

## Migration Plan

### Phase 1: Introduce Scheduler Types

Add a new module, for example:

- `src/scheduler/mod.rs`
- `src/scheduler/types.rs`
- `src/scheduler/snapshot.rs`
- `src/scheduler/decision.rs`

First deliverables:

- `ResourceKey`
- `StageId`
- `ResourceState`
- `StageState`
- `PlaybackDemand`
- `SchedulerEvent`
- `SchedulerDecision`
- `SchedulerSnapshot`

No behavioral migration yet.

### Phase 2: Route Existing Signals Through Scheduler

Keep current workers, but stop direct worker-to-worker orchestration.

Transitional goal:

- `sync` no longer calls `render` or `decode`
- `render` no longer calls `decode` or `sync`
- `decode` no longer calls `render` or `sync`
- they all emit scheduler events instead

The scheduler can still wake legacy workers while the execution side remains old.

### Phase 3: Split Stage Identity

Separate the current broad workers into stage-level executors:

- `AudioDecode`
- `VideoDecode`
- `AudioRender`
- `VideoRender`

Even if some share a thread at first, the scheduling identity should be separate.

### Phase 4: Move Playback Timing Into Scheduler Model

Reduce `sync` from a quasi-global coordinator into a playback executor and deadline observer.

At this stage:

- scheduling policy lives in the scheduler
- playback timing facts feed scheduler demand
- `sync` stops owning global wake policy

### Phase 5: Add New Stages

After the model is stable:

- subtitle rendering
- composition
- multi-instance decode capacity
- multi-instance render capacity

## First Implementation Boundary

To keep the first rewrite tractable, the initial scheduler-backed path should only guarantee:

1. separate `AudioDecode` and `VideoDecode` stage identity
2. separate `AudioRender` and `VideoRender` stage identity
3. playback demand expressed as audio/video presentation shortage
4. scheduler decides wakes
5. workers stop direct coordination

Do not try to solve all future graph complexity in the first pass.

## Open Questions

1. Should `PresentationAudio` mean runtime-owned audio chunks, device-buffer headroom, or both?
2. Should `VideoPresentQueue` be modeled separately from generic `PresentationVideo`?
3. Do we want one scheduler thread with an event queue, or scheduler evaluation inline on event submission plus a condvar wake path?
4. How should multi-instance capacity be represented when `VideoDecode` later uses more than one worker?
5. Which current diagnostics should move into scheduler-specific trace state versus staying in player diagnostics?

## Recommended Next Step

Start with code scaffolding, not worker rewrites:

1. add the `scheduler` module
2. define fixed enums and structs
3. add scheduler snapshot assembly from current runtime
4. add a pure decision function with unit tests
5. only then start rerouting worker notifications

That keeps the model visible and testable before execution wiring gets noisy.
