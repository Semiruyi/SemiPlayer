use crate::api::types::PlayerState;
use crate::scheduler::snapshot::SchedulerSnapshot;
use crate::scheduler::types::{ResourceKey, SchedulerDecision, SchedulerEvent, StageId};

pub fn evaluate_scheduler_decision(
    snapshot: &SchedulerSnapshot,
    event: &SchedulerEvent,
) -> SchedulerDecision {
    let mut decision = SchedulerDecision {
        next_deadline_us: snapshot.playback_demand.next_deadline_us,
        ..SchedulerDecision::default()
    };

    if !snapshot.media_loaded || snapshot.player_state == PlayerState::Idle {
        return decision;
    }

    if snapshot.playback_demand.needs_audio_now {
        request_resource(snapshot, event, ResourceKey::PresentationAudio, &mut decision);
    }

    if snapshot.playback_demand.needs_video_now {
        request_resource(snapshot, event, ResourceKey::PresentationVideo, &mut decision);
    }

    decision.wake_playback = should_wake_playback(snapshot, event);
    decision
}

fn should_wake_playback(snapshot: &SchedulerSnapshot, event: &SchedulerEvent) -> bool {
    if !playback_can_progress(snapshot) {
        return false;
    }

    match event {
        SchedulerEvent::PlaybackDemandChanged
        | SchedulerEvent::SeekCompleted
        | SchedulerEvent::MediaLoaded
        | SchedulerEvent::PlayerStateChanged(PlayerState::Playing) => true,
        SchedulerEvent::StageProgress { produced, .. } => produced.iter().any(|resource| {
            matches!(
                resource,
                ResourceKey::PresentationAudio | ResourceKey::PresentationVideo
            )
        }),
        _ => false,
    }
}

fn playback_can_progress(snapshot: &SchedulerSnapshot) -> bool {
    (!snapshot.playback_demand.needs_audio_now
        || snapshot.resources[ResourceKey::PresentationAudio].is_satisfied())
        && (!snapshot.playback_demand.needs_video_now
            || snapshot.resources[ResourceKey::PresentationVideo].is_satisfied())
}

fn request_resource(
    snapshot: &SchedulerSnapshot,
    event: &SchedulerEvent,
    resource: ResourceKey,
    decision: &mut SchedulerDecision,
) {
    if snapshot.resources[resource].is_satisfied() {
        return;
    }

    let Some(stage) = StageId::producer_for(resource) else {
        return;
    };

    request_stage(snapshot, event, stage, decision);
}

fn request_stage(
    snapshot: &SchedulerSnapshot,
    event: &SchedulerEvent,
    stage: StageId,
    decision: &mut SchedulerDecision,
) {
    let stage_state = snapshot.stages[stage];
    if stage_state.requested || stage_state.in_flight {
        return;
    }

    let topology = stage.topology();
    let mut missing_inputs = Vec::new();
    for resource in topology.consumes {
        if !snapshot.resources[*resource].is_satisfied() {
            missing_inputs.push(*resource);
        }
    }

    if missing_inputs.is_empty() {
        if stage_state.blocked && matches!(event, SchedulerEvent::StageBlocked(blocked) if *blocked == stage) {
            return;
        }
        if !decision.wake_stages.contains(&stage) {
            decision.wake_stages.push(stage);
        }
        return;
    }

    for resource in missing_inputs {
        request_resource(snapshot, event, resource, decision);
    }
}

#[cfg(test)]
mod tests {
    use super::evaluate_scheduler_decision;
    use crate::api::types::PlayerState;
    use crate::scheduler::snapshot::{ResourceMap, SchedulerSnapshot, StageMap};
    use crate::scheduler::types::{
        PlaybackDemand, ResourceKey, ResourceState, SchedulerDecision, SchedulerEvent, StageId,
    };

    fn base_snapshot() -> SchedulerSnapshot {
        SchedulerSnapshot {
            player_state: PlayerState::Playing,
            media_loaded: true,
            generation: 1,
            playback_demand: PlaybackDemand {
                needs_audio_now: false,
                needs_video_now: false,
                next_deadline_us: Some(12_345),
            },
            resources: ResourceMap::new(),
            stages: StageMap::new(),
        }
    }

    #[test]
    fn presentation_video_shortage_wakes_video_render_when_decoded_exists() {
        let mut snapshot = base_snapshot();
        snapshot.playback_demand.needs_video_now = true;
        snapshot.resources[ResourceKey::PresentationVideo] = ResourceState {
            available_units: 0,
            low_watermark: 1,
            high_watermark: 3,
            end_of_stream: false,
            blocked: false,
        };
        snapshot.resources[ResourceKey::DecodedVideo] = ResourceState {
            available_units: 2,
            low_watermark: 1,
            high_watermark: 3,
            end_of_stream: false,
            blocked: false,
        };

        let decision =
            evaluate_scheduler_decision(&snapshot, &SchedulerEvent::PlaybackDemandChanged);

        assert_eq!(
            decision,
            SchedulerDecision {
                wake_playback: false,
                wake_stages: vec![StageId::VideoRender],
                next_deadline_us: Some(12_345),
            }
        );
    }

    #[test]
    fn presentation_video_shortage_wakes_video_decode_when_upstream_empty() {
        let mut snapshot = base_snapshot();
        snapshot.playback_demand.needs_video_now = true;
        snapshot.resources[ResourceKey::PresentationVideo] = ResourceState {
            available_units: 0,
            low_watermark: 1,
            high_watermark: 3,
            end_of_stream: false,
            blocked: false,
        };
        snapshot.resources[ResourceKey::DecodedVideo] = ResourceState {
            available_units: 0,
            low_watermark: 1,
            high_watermark: 3,
            end_of_stream: false,
            blocked: false,
        };

        let decision =
            evaluate_scheduler_decision(&snapshot, &SchedulerEvent::PlaybackDemandChanged);

        assert_eq!(decision.wake_stages, vec![StageId::VideoDecode]);
        assert!(!decision.wake_playback);
    }

    #[test]
    fn in_flight_stage_is_not_woken_twice() {
        let mut snapshot = base_snapshot();
        snapshot.playback_demand.needs_video_now = true;
        snapshot.resources[ResourceKey::PresentationVideo] = ResourceState {
            available_units: 0,
            low_watermark: 1,
            high_watermark: 3,
            end_of_stream: false,
            blocked: false,
        };
        snapshot.resources[ResourceKey::DecodedVideo] = ResourceState {
            available_units: 2,
            low_watermark: 1,
            high_watermark: 3,
            end_of_stream: false,
            blocked: false,
        };
        snapshot.stages[StageId::VideoRender].in_flight = true;

        let decision =
            evaluate_scheduler_decision(&snapshot, &SchedulerEvent::PlaybackDemandChanged);

        assert!(decision.wake_stages.is_empty());
    }

    #[test]
    fn requested_stage_is_not_woken_twice() {
        let mut snapshot = base_snapshot();
        snapshot.playback_demand.needs_video_now = true;
        snapshot.resources[ResourceKey::PresentationVideo] = ResourceState {
            available_units: 0,
            low_watermark: 1,
            high_watermark: 3,
            end_of_stream: false,
            blocked: false,
        };
        snapshot.resources[ResourceKey::DecodedVideo] = ResourceState {
            available_units: 2,
            low_watermark: 1,
            high_watermark: 3,
            end_of_stream: false,
            blocked: false,
        };
        snapshot.stages[StageId::VideoRender].requested = true;

        let decision =
            evaluate_scheduler_decision(&snapshot, &SchedulerEvent::PlaybackDemandChanged);

        assert!(decision.wake_stages.is_empty());
    }

    #[test]
    fn stage_progress_wakes_playback_when_presentation_supply_is_ready() {
        let mut snapshot = base_snapshot();
        snapshot.playback_demand.needs_audio_now = true;
        snapshot.resources[ResourceKey::PresentationAudio] = ResourceState {
            available_units: 8,
            low_watermark: 4,
            high_watermark: 8,
            end_of_stream: false,
            blocked: false,
        };

        let decision = evaluate_scheduler_decision(
            &snapshot,
            &SchedulerEvent::StageProgress {
                stage: StageId::AudioRender,
                produced: vec![ResourceKey::PresentationAudio],
            },
        );

        assert!(decision.wake_playback);
        assert!(decision.wake_stages.is_empty());
    }

    #[test]
    fn blocked_video_render_escalates_to_video_decode_when_input_is_missing() {
        let mut snapshot = base_snapshot();
        snapshot.playback_demand.needs_video_now = true;
        snapshot.resources[ResourceKey::PresentationVideo] = ResourceState {
            available_units: 0,
            low_watermark: 1,
            high_watermark: 3,
            end_of_stream: false,
            blocked: false,
        };
        snapshot.resources[ResourceKey::DecodedVideo] = ResourceState {
            available_units: 0,
            low_watermark: 1,
            high_watermark: 3,
            end_of_stream: false,
            blocked: false,
        };
        snapshot.stages[StageId::VideoRender].blocked = true;

        let decision = evaluate_scheduler_decision(
            &snapshot,
            &SchedulerEvent::StageBlocked(StageId::VideoRender),
        );

        assert_eq!(decision.wake_stages, vec![StageId::VideoDecode]);
    }

    #[test]
    fn upstream_progress_reactivates_blocked_video_render_once_input_exists() {
        let mut snapshot = base_snapshot();
        snapshot.playback_demand.needs_video_now = true;
        snapshot.resources[ResourceKey::PresentationVideo] = ResourceState {
            available_units: 0,
            low_watermark: 1,
            high_watermark: 3,
            end_of_stream: false,
            blocked: false,
        };
        snapshot.resources[ResourceKey::DecodedVideo] = ResourceState {
            available_units: 2,
            low_watermark: 1,
            high_watermark: 3,
            end_of_stream: false,
            blocked: false,
        };
        snapshot.stages[StageId::VideoRender].blocked = true;

        let decision = evaluate_scheduler_decision(
            &snapshot,
            &SchedulerEvent::StageProgress {
                stage: StageId::VideoDecode,
                produced: vec![ResourceKey::DecodedVideo],
            },
        );

        assert_eq!(decision.wake_stages, vec![StageId::VideoRender]);
    }

    #[test]
    fn stage_started_snapshot_does_not_requeue_same_stage() {
        let mut snapshot = base_snapshot();
        snapshot.playback_demand.needs_video_now = true;
        snapshot.resources[ResourceKey::PresentationVideo] = ResourceState {
            available_units: 0,
            low_watermark: 1,
            high_watermark: 3,
            end_of_stream: false,
            blocked: false,
        };
        snapshot.resources[ResourceKey::DecodedVideo] = ResourceState {
            available_units: 1,
            low_watermark: 1,
            high_watermark: 3,
            end_of_stream: false,
            blocked: false,
        };
        snapshot.stages[StageId::VideoRender].in_flight = true;

        let decision =
            evaluate_scheduler_decision(&snapshot, &SchedulerEvent::StageStarted(StageId::VideoRender));

        assert!(decision.wake_stages.is_empty());
    }
}
