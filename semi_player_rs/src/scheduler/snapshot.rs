use std::ops::{Index, IndexMut};

use crate::api::types::PlayerState;
use crate::player::runtime::{DecodeDemandStatus, PlaybackSupplyStatus, RenderSupplyStatus};
use crate::scheduler::types::{PlaybackDemand, ResourceKey, ResourceState, StageId, StageState};

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct ResourceMap {
    entries: [ResourceState; ResourceKey::ALL.len()],
}

impl ResourceMap {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn iter(self) -> impl Iterator<Item = (ResourceKey, ResourceState)> {
        ResourceKey::ALL.into_iter().map(move |key| (key, self[key]))
    }
}

impl Index<ResourceKey> for ResourceMap {
    type Output = ResourceState;

    fn index(&self, index: ResourceKey) -> &Self::Output {
        &self.entries[index.index()]
    }
}

impl IndexMut<ResourceKey> for ResourceMap {
    fn index_mut(&mut self, index: ResourceKey) -> &mut Self::Output {
        &mut self.entries[index.index()]
    }
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct StageMap {
    entries: [StageState; StageId::ALL.len()],
}

impl StageMap {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn iter(self) -> impl Iterator<Item = (StageId, StageState)> {
        StageId::ALL.into_iter().map(move |stage| (stage, self[stage]))
    }
}

impl Index<StageId> for StageMap {
    type Output = StageState;

    fn index(&self, index: StageId) -> &Self::Output {
        &self.entries[index.index()]
    }
}

impl IndexMut<StageId> for StageMap {
    fn index_mut(&mut self, index: StageId) -> &mut Self::Output {
        &mut self.entries[index.index()]
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct SchedulerSnapshot {
    pub player_state: PlayerState,
    pub playback_demand: PlaybackDemand,
    pub resources: ResourceMap,
    pub stages: StageMap,
    pub media_loaded: bool,
    pub generation: u64,
}

impl Default for SchedulerSnapshot {
    fn default() -> Self {
        Self {
            player_state: PlayerState::Idle,
            playback_demand: PlaybackDemand::default(),
            resources: ResourceMap::default(),
            stages: StageMap::default(),
            media_loaded: false,
            generation: 0,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct LegacySchedulerInputs {
    pub player_state: PlayerState,
    pub media_loaded: bool,
    pub generation: u64,
    pub playback_supply: PlaybackSupplyStatus,
    pub render_supply: RenderSupplyStatus,
    pub decode_demand: DecodeDemandStatus,
    pub audio_presentation_demand: bool,
    pub video_presentation_demand: bool,
    pub next_deadline_us: Option<i64>,
}

impl SchedulerSnapshot {
    pub fn from_legacy_inputs(inputs: LegacySchedulerInputs) -> Self {
        let mut resources = ResourceMap::new();
        resources[ResourceKey::PresentationAudio] = ResourceState {
            available_units: inputs.playback_supply.audio_queue_len,
            low_watermark: inputs.playback_supply.target_audio_queue_len,
            high_watermark: inputs.playback_supply.target_audio_queue_len,
            end_of_stream: inputs.playback_supply.end_of_stream,
            blocked: false,
        };
        resources[ResourceKey::PresentationVideo] = ResourceState {
            available_units: inputs.playback_supply.ready_video_frame_count,
            low_watermark: inputs.playback_supply.target_ready_video_frame_count,
            high_watermark: inputs.playback_supply.target_ready_video_frame_count,
            end_of_stream: inputs.playback_supply.end_of_stream,
            blocked: false,
        };

        // Compatibility shim:
        // audio currently reaches playback without a distinct render-stage queue.
        // Until audio render exists as a separate stage, decoded audio mirrors the
        // playback-ready audio backlog closely enough for scheduler scaffolding.
        resources[ResourceKey::DecodedAudio] = ResourceState {
            available_units: inputs.decode_demand.audio_queue_len,
            low_watermark: inputs.decode_demand.target_audio_queue_len,
            high_watermark: inputs.decode_demand.target_audio_queue_len,
            end_of_stream: inputs.decode_demand.end_of_stream,
            blocked: false,
        };
        resources[ResourceKey::DecodedVideo] = ResourceState {
            available_units: inputs.render_supply.decoded_video_queue_len
                + inputs.render_supply.in_flight_decoded_video_queue_len,
            low_watermark: 1,
            high_watermark: inputs.decode_demand.target_total_video_frames,
            end_of_stream: inputs.render_supply.end_of_stream,
            blocked: false,
        };

        let mut stages = StageMap::new();
        stages[StageId::AudioDecode] = StageState {
            requested: false,
            in_flight: false,
            blocked: inputs.decode_demand.end_of_stream,
            last_progress_generation: inputs.generation,
        };
        stages[StageId::VideoDecode] = StageState {
            requested: false,
            in_flight: false,
            blocked: inputs.decode_demand.end_of_stream,
            last_progress_generation: inputs.generation,
        };
        stages[StageId::AudioRender] = StageState {
            requested: false,
            in_flight: false,
            blocked: false,
            last_progress_generation: inputs.generation,
        };
        stages[StageId::VideoRender] = StageState {
            requested: false,
            in_flight: inputs.render_supply.has_in_flight_batch,
            blocked: false,
            last_progress_generation: inputs.generation,
        };

        Self {
            player_state: inputs.player_state,
            playback_demand: PlaybackDemand {
                needs_audio_now: inputs.audio_presentation_demand,
                needs_video_now: inputs.video_presentation_demand,
                next_deadline_us: inputs.next_deadline_us,
            },
            resources,
            stages,
            media_loaded: inputs.media_loaded,
            generation: inputs.generation,
        }
    }
}

#[cfg(test)]
mod tests {
    use crate::api::types::PlayerState;
    use crate::player::runtime::{DecodeDemandStatus, PlaybackSupplyStatus, RenderSupplyStatus};
    use crate::scheduler::decision::evaluate_scheduler_decision;
    use crate::scheduler::types::{SchedulerEvent, StageId};

    use super::{LegacySchedulerInputs, SchedulerSnapshot};

    #[test]
    fn legacy_audio_shortage_routes_to_audio_decode_stage() {
        let snapshot = SchedulerSnapshot::from_legacy_inputs(LegacySchedulerInputs {
            player_state: PlayerState::Playing,
            media_loaded: true,
            generation: 7,
            playback_supply: PlaybackSupplyStatus {
                audio_queue_len: 0,
                ready_video_frame_count: 3,
                target_audio_queue_len: 8,
                target_ready_video_frame_count: 3,
                has_sufficient_audio: false,
                has_sufficient_video: true,
                has_sufficient_presentation_buffer: false,
                end_of_stream: false,
                needs_presentation_supply: true,
            },
            render_supply: RenderSupplyStatus {
                ready_video_frame_count: 3,
                target_ready_video_frame_count: 3,
                ..RenderSupplyStatus::default()
            },
            decode_demand: DecodeDemandStatus {
                audio_queue_len: 0,
                decoded_video_queue_len: 0,
                buffered_video_frame_count: 3,
                target_audio_queue_len: 8,
                target_total_video_frames: 3,
                has_sufficient_audio: false,
                has_sufficient_total_video_buffer: true,
                needs_audio_decode: true,
                needs_video_decode: false,
                end_of_stream: false,
                should_decode: true,
            },
            audio_presentation_demand: true,
            video_presentation_demand: false,
            next_deadline_us: Some(42_000),
        });

        let decision =
            evaluate_scheduler_decision(&snapshot, &SchedulerEvent::PlaybackDemandChanged);

        assert_eq!(decision.wake_stages, vec![StageId::AudioDecode]);
        assert!(!decision.wake_playback);
    }
}
