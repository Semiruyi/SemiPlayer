use crate::api::types::PlayerState;
use crate::util::time::MediaTimeUs;

#[derive(Clone, Copy, Debug, Eq, PartialEq, Hash)]
pub enum ResourceKey {
    DecodedAudio,
    DecodedVideo,
    PresentationAudio,
    PresentationVideo,
}

impl ResourceKey {
    pub const ALL: [Self; 4] = [
        Self::DecodedAudio,
        Self::DecodedVideo,
        Self::PresentationAudio,
        Self::PresentationVideo,
    ];

    pub const fn index(self) -> usize {
        match self {
            Self::DecodedAudio => 0,
            Self::DecodedVideo => 1,
            Self::PresentationAudio => 2,
            Self::PresentationVideo => 3,
        }
    }
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct ResourceState {
    pub available_units: usize,
    pub low_watermark: usize,
    pub high_watermark: usize,
    pub end_of_stream: bool,
    pub blocked: bool,
}

impl ResourceState {
    pub fn is_below_low_watermark(self) -> bool {
        self.available_units < self.low_watermark
    }

    pub fn is_satisfied(self) -> bool {
        self.end_of_stream || !self.is_below_low_watermark()
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Hash)]
pub enum StageId {
    AudioDecode,
    VideoDecode,
    AudioRender,
    VideoRender,
}

impl StageId {
    pub const ALL: [Self; 4] = [
        Self::AudioDecode,
        Self::VideoDecode,
        Self::AudioRender,
        Self::VideoRender,
    ];

    pub const fn index(self) -> usize {
        match self {
            Self::AudioDecode => 0,
            Self::VideoDecode => 1,
            Self::AudioRender => 2,
            Self::VideoRender => 3,
        }
    }

    pub const fn topology(self) -> StageTopology {
        match self {
            Self::AudioDecode => StageTopology {
                consumes: &[],
                produces: &[ResourceKey::DecodedAudio],
            },
            Self::VideoDecode => StageTopology {
                consumes: &[],
                produces: &[ResourceKey::DecodedVideo],
            },
            Self::AudioRender => StageTopology {
                consumes: &[ResourceKey::DecodedAudio],
                produces: &[ResourceKey::PresentationAudio],
            },
            Self::VideoRender => StageTopology {
                consumes: &[ResourceKey::DecodedVideo],
                produces: &[ResourceKey::PresentationVideo],
            },
        }
    }

    pub const fn producer_for(resource: ResourceKey) -> Option<Self> {
        match resource {
            ResourceKey::DecodedAudio => Some(Self::AudioDecode),
            ResourceKey::DecodedVideo => Some(Self::VideoDecode),
            ResourceKey::PresentationAudio => Some(Self::AudioRender),
            ResourceKey::PresentationVideo => Some(Self::VideoRender),
        }
    }
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct StageState {
    pub requested: bool,
    pub in_flight: bool,
    pub blocked: bool,
    pub last_progress_generation: u64,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct StageTopology {
    pub consumes: &'static [ResourceKey],
    pub produces: &'static [ResourceKey],
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct PlaybackDemand {
    pub needs_audio_now: bool,
    pub needs_video_now: bool,
    pub next_deadline_us: Option<MediaTimeUs>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum SchedulerEvent {
    PlaybackDemandChanged,
    PlaybackAdvanced,
    StageRequested(StageId),
    StageStarted(StageId),
    StageProgress {
        stage: StageId,
        produced: Vec<ResourceKey>,
    },
    StageBlocked(StageId),
    StageIdle(StageId),
    SeekStarted,
    SeekCompleted,
    MediaLoaded,
    MediaUnloaded,
    PlayerStateChanged(PlayerState),
    ShutdownRequested,
}

#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct SchedulerDecision {
    pub wake_playback: bool,
    pub wake_stages: Vec<StageId>,
    pub next_deadline_us: Option<MediaTimeUs>,
}
