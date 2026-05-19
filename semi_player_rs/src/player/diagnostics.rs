use std::sync::atomic::{AtomicI64, AtomicU64, Ordering};
use std::sync::Mutex;
use std::time::Instant;

use crate::player::runtime::AudioDiscardSummary;
use crate::util::time::MediaTimeUs;

#[derive(Clone, Copy, Debug, Default)]
pub struct PlayerDiagnosticsSnapshot {
    pub ffi_lock_wait_last_us: MediaTimeUs,
    pub ffi_lock_wait_max_us: MediaTimeUs,
    pub sync_worker_lock_wait_last_us: MediaTimeUs,
    pub sync_worker_lock_wait_max_us: MediaTimeUs,
    pub decode_worker_lock_wait_last_us: MediaTimeUs,
    pub decode_worker_lock_wait_max_us: MediaTimeUs,
    pub worker_deadline_slip_last_us: MediaTimeUs,
    pub worker_deadline_slip_max_us: MediaTimeUs,
    pub stale_audio_discard_event_count: u64,
    pub stale_audio_discard_frame_count: u64,
    pub stale_audio_discard_last_frame_count: u64,
    pub stale_audio_discard_last_lag_us: MediaTimeUs,
    pub stale_audio_discard_max_lag_us: MediaTimeUs,
    pub render_frames_total: u64,
    pub render_passthrough_frames_total: u64,
    pub render_passthrough_with_subtitle_intent_frames_total: u64,
    pub render_requires_transform_frames_total: u64,
    pub render_fallback_passthrough_frames_total: u64,
    pub seek_event_count: u64,
    pub seek_active: bool,
    pub last_seek_target_us: MediaTimeUs,
    pub seek_api_duration_us: MediaTimeUs,
    pub seek_lock_wait_us: MediaTimeUs,
    pub seek_ffmpeg_seek_us: MediaTimeUs,
    pub seek_reset_us: MediaTimeUs,
    pub seek_first_video_decoded_us: MediaTimeUs,
    pub seek_first_video_pts_us: MediaTimeUs,
    pub seek_first_post_target_video_decoded_us: MediaTimeUs,
    pub seek_first_post_target_video_pts_us: MediaTimeUs,
    pub seek_audio_position_at_first_post_target_video_decoded_us: MediaTimeUs,
    pub seek_first_audio_decoder_output_us: MediaTimeUs,
    pub seek_first_audio_decoded_us: MediaTimeUs,
    pub seek_first_current_video_ready_us: MediaTimeUs,
    pub seek_first_current_video_pts_us: MediaTimeUs,
    pub seek_audio_position_at_first_current_video_us: MediaTimeUs,
    pub seek_audio_advanced_between_post_target_decode_and_current_us: MediaTimeUs,
    pub seek_post_target_video_dropped_before_current_count: u64,
    pub seek_audio_output_started_before_current: bool,
    pub seek_audio_output_start_us: MediaTimeUs,
    pub seek_target_video_ready_us: MediaTimeUs,
    pub seek_target_video_pts_us: MediaTimeUs,
    pub seek_target_audio_ready_us: MediaTimeUs,
    pub seek_stable_us: MediaTimeUs,
    pub seek_pre_target_video_decoded_count: u64,
    pub seek_pre_target_current_video_count: u64,
    pub seek_first_video_packet_pts_us: MediaTimeUs,
    pub seek_first_video_packet_dts_us: MediaTimeUs,
    pub seek_first_video_packet_is_key: bool,
    pub seek_first_video_packet_pos: i64,
    pub seek_video_packets_read: u64,
    pub seek_audio_packets_read: u64,
}

#[derive(Clone, Copy, Debug)]
pub(crate) enum LockOwner {
    Ffi,
    #[allow(dead_code)]
    SyncWorker,
    #[allow(dead_code)]
    DecodeWorker,
}

#[derive(Default)]
pub(crate) struct PlayerDiagnostics {
    ffi_lock_wait_last_us: AtomicI64,
    ffi_lock_wait_max_us: AtomicI64,
    sync_worker_lock_wait_last_us: AtomicI64,
    sync_worker_lock_wait_max_us: AtomicI64,
    decode_worker_lock_wait_last_us: AtomicI64,
    decode_worker_lock_wait_max_us: AtomicI64,
    worker_deadline_slip_last_us: AtomicI64,
    worker_deadline_slip_max_us: AtomicI64,
    stale_audio_discard_event_count: AtomicU64,
    stale_audio_discard_frame_count: AtomicU64,
    stale_audio_discard_last_frame_count: AtomicU64,
    stale_audio_discard_last_lag_us: AtomicI64,
    stale_audio_discard_max_lag_us: AtomicI64,
    render_frames_total: AtomicU64,
    render_passthrough_frames_total: AtomicU64,
    render_passthrough_with_subtitle_intent_frames_total: AtomicU64,
    render_requires_transform_frames_total: AtomicU64,
    render_fallback_passthrough_frames_total: AtomicU64,
    seek: Mutex<SeekDiagnosticsState>,
}

#[derive(Clone, Copy, Debug, Default)]
struct SeekDiagnosticsSnapshot {
    seek_event_count: u64,
    seek_active: bool,
    last_seek_target_us: MediaTimeUs,
    seek_api_duration_us: MediaTimeUs,
    seek_lock_wait_us: MediaTimeUs,
    seek_ffmpeg_seek_us: MediaTimeUs,
    seek_reset_us: MediaTimeUs,
    seek_first_video_decoded_us: MediaTimeUs,
    seek_first_video_pts_us: MediaTimeUs,
    seek_first_post_target_video_decoded_us: MediaTimeUs,
    seek_first_post_target_video_pts_us: MediaTimeUs,
    seek_audio_position_at_first_post_target_video_decoded_us: MediaTimeUs,
    seek_first_audio_decoder_output_us: MediaTimeUs,
    seek_first_audio_decoded_us: MediaTimeUs,
    seek_first_current_video_ready_us: MediaTimeUs,
    seek_first_current_video_pts_us: MediaTimeUs,
    seek_audio_position_at_first_current_video_us: MediaTimeUs,
    seek_audio_advanced_between_post_target_decode_and_current_us: MediaTimeUs,
    seek_post_target_video_dropped_before_current_count: u64,
    seek_audio_output_started_before_current: bool,
    seek_audio_output_start_us: MediaTimeUs,
    seek_target_video_ready_us: MediaTimeUs,
    seek_target_video_pts_us: MediaTimeUs,
    seek_target_audio_ready_us: MediaTimeUs,
    seek_stable_us: MediaTimeUs,
    seek_pre_target_video_decoded_count: u64,
    seek_pre_target_current_video_count: u64,
}

#[derive(Debug)]
struct SeekObservation {
    requested_at: Instant,
    target_us: MediaTimeUs,
    seek_api_duration_us: Option<MediaTimeUs>,
    seek_lock_wait_us: Option<MediaTimeUs>,
    seek_ffmpeg_seek_us: Option<MediaTimeUs>,
    seek_reset_us: Option<MediaTimeUs>,
    seek_first_video_decoded_us: Option<MediaTimeUs>,
    seek_first_video_pts_us: Option<MediaTimeUs>,
    seek_first_post_target_video_decoded_us: Option<MediaTimeUs>,
    seek_first_post_target_video_pts_us: Option<MediaTimeUs>,
    seek_audio_position_at_first_post_target_video_decoded_us: Option<MediaTimeUs>,
    seek_first_audio_decoder_output_us: Option<MediaTimeUs>,
    seek_first_audio_decoded_us: Option<MediaTimeUs>,
    seek_first_current_video_ready_us: Option<MediaTimeUs>,
    seek_first_current_video_pts_us: Option<MediaTimeUs>,
    seek_audio_position_at_first_current_video_us: Option<MediaTimeUs>,
    seek_audio_advanced_between_post_target_decode_and_current_us: Option<MediaTimeUs>,
    seek_post_target_video_dropped_before_current_count: u64,
    seek_audio_output_started_before_current: bool,
    seek_audio_output_start_us: Option<MediaTimeUs>,
    seek_target_video_ready_us: Option<MediaTimeUs>,
    seek_target_video_pts_us: Option<MediaTimeUs>,
    seek_target_audio_ready_us: Option<MediaTimeUs>,
    seek_stable_us: Option<MediaTimeUs>,
    seek_pre_target_video_decoded_count: u64,
    seek_pre_target_current_video_count: u64,
    last_observed_current_video_pts_us: Option<MediaTimeUs>,
}

#[derive(Debug, Default)]
struct SeekDiagnosticsState {
    seek_event_count: u64,
    active: Option<SeekObservation>,
    last_completed: Option<SeekDiagnosticsSnapshot>,
}

impl PlayerDiagnostics {
    pub(crate) fn observe_lock_wait(&self, owner: LockOwner, wait_us: MediaTimeUs) {
        match owner {
            LockOwner::Ffi => {
                self.ffi_lock_wait_last_us.store(wait_us, Ordering::Relaxed);
                update_atomic_max(&self.ffi_lock_wait_max_us, wait_us);
            }
            LockOwner::SyncWorker => {
                self.sync_worker_lock_wait_last_us
                    .store(wait_us, Ordering::Relaxed);
                update_atomic_max(&self.sync_worker_lock_wait_max_us, wait_us);
            }
            LockOwner::DecodeWorker => {
                self.decode_worker_lock_wait_last_us
                    .store(wait_us, Ordering::Relaxed);
                update_atomic_max(&self.decode_worker_lock_wait_max_us, wait_us);
            }
        }
    }

    pub(crate) fn observe_worker_deadline_slip(&self, slip_us: MediaTimeUs) {
        self.worker_deadline_slip_last_us
            .store(slip_us, Ordering::Relaxed);
        update_atomic_max(&self.worker_deadline_slip_max_us, slip_us);
    }

    pub(crate) fn observe_stale_audio_discard(&self, discard: AudioDiscardSummary) {
        if discard.removed_frames == 0 {
            return;
        }

        self.stale_audio_discard_event_count
            .fetch_add(1, Ordering::Relaxed);
        self.stale_audio_discard_frame_count.fetch_add(
            u64::try_from(discard.removed_frames).unwrap_or(u64::MAX),
            Ordering::Relaxed,
        );
        self.stale_audio_discard_last_frame_count.store(
            u64::try_from(discard.removed_frames).unwrap_or(u64::MAX),
            Ordering::Relaxed,
        );
        self.stale_audio_discard_last_lag_us
            .store(discard.max_lag_us, Ordering::Relaxed);
        update_atomic_max(&self.stale_audio_discard_max_lag_us, discard.max_lag_us);
    }

    pub(crate) fn observe_render_stats(
        &self,
        rendered_frames: usize,
        passthrough_frames: usize,
        passthrough_with_subtitle_intent_frames: usize,
        requires_transform_frames: usize,
        fallback_passthrough_frames: usize,
    ) {
        self.render_frames_total.fetch_add(
            u64::try_from(rendered_frames).unwrap_or(u64::MAX),
            Ordering::Relaxed,
        );
        self.render_passthrough_frames_total.fetch_add(
            u64::try_from(passthrough_frames).unwrap_or(u64::MAX),
            Ordering::Relaxed,
        );
        self.render_passthrough_with_subtitle_intent_frames_total
            .fetch_add(
                u64::try_from(passthrough_with_subtitle_intent_frames).unwrap_or(u64::MAX),
                Ordering::Relaxed,
            );
        self.render_requires_transform_frames_total.fetch_add(
            u64::try_from(requires_transform_frames).unwrap_or(u64::MAX),
            Ordering::Relaxed,
        );
        self.render_fallback_passthrough_frames_total.fetch_add(
            u64::try_from(fallback_passthrough_frames).unwrap_or(u64::MAX),
            Ordering::Relaxed,
        );
    }

    pub(crate) fn observe_seek_requested(&self, target_us: MediaTimeUs) {
        let mut seek = self.seek.lock().unwrap();
        seek.seek_event_count = seek.seek_event_count.saturating_add(1);
        seek.active = Some(SeekObservation::new(target_us));
    }

    pub(crate) fn observe_seek_lock_acquired(&self) {
        self.with_active_seek(|seek| {
            seek.seek_lock_wait_us = Some(seek.elapsed_us());
        });
    }

    pub(crate) fn observe_seek_ffmpeg_seek_started(&self) {
        self.with_active_seek(|seek| {
            if seek.seek_lock_wait_us.is_none() {
                seek.seek_lock_wait_us = Some(seek.elapsed_us());
            }
        });
    }

    pub(crate) fn observe_seek_ffmpeg_seek_finished(&self) {
        self.with_active_seek(|seek| {
            seek.seek_ffmpeg_seek_us = Some(seek.elapsed_us());
        });
    }

    pub(crate) fn observe_seek_reset_finished(&self) {
        self.with_active_seek(|seek| {
            seek.seek_reset_us = Some(seek.elapsed_us());
        });
    }

    pub(crate) fn observe_seek_api_completed(&self) {
        self.with_active_seek(|seek| {
            seek.seek_api_duration_us = Some(seek.elapsed_us());
        });
    }

    pub(crate) fn observe_seek_aborted(&self) {
        let mut seek = self.seek.lock().unwrap();
        let Some(mut active) = seek.active.take() else {
            return;
        };

        if active.seek_api_duration_us.is_none() {
            active.seek_api_duration_us = Some(active.elapsed_us());
        }
        seek.last_completed = Some(active.snapshot(seek.seek_event_count, false));
    }

    pub(crate) fn observe_seek_video_decoded(
        &self,
        frame_pts_us: MediaTimeUs,
        playback_time_us: MediaTimeUs,
    ) {
        self.with_active_seek(|seek| {
            if seek.seek_first_video_decoded_us.is_none() {
                seek.seek_first_video_decoded_us = Some(seek.elapsed_us());
            }
            if seek.seek_first_video_pts_us.is_none() {
                seek.seek_first_video_pts_us = Some(frame_pts_us);
            }
            if frame_pts_us >= seek.target_us
                && seek.seek_first_post_target_video_decoded_us.is_none()
            {
                seek.seek_first_post_target_video_decoded_us = Some(seek.elapsed_us());
                seek.seek_first_post_target_video_pts_us = Some(frame_pts_us);
                seek.seek_audio_position_at_first_post_target_video_decoded_us =
                    Some(playback_time_us);
            }
            if frame_pts_us < seek.target_us {
                seek.seek_pre_target_video_decoded_count =
                    seek.seek_pre_target_video_decoded_count.saturating_add(1);
            }
        });
    }

    pub(crate) fn observe_seek_first_audio_decoded(&self) {
        self.with_active_seek(|seek| {
            if seek.seek_first_audio_decoded_us.is_none() {
                seek.seek_first_audio_decoded_us = Some(seek.elapsed_us());
            }
        });
    }

    pub(crate) fn observe_seek_first_audio_decoder_output(&self) {
        self.with_active_seek(|seek| {
            if seek.seek_first_audio_decoder_output_us.is_none() {
                seek.seek_first_audio_decoder_output_us = Some(seek.elapsed_us());
            }
        });
    }

    pub(crate) fn observe_seek_current_video(
        &self,
        current_pts_us: MediaTimeUs,
        current_effective_end_us: Option<MediaTimeUs>,
        playback_time_us: MediaTimeUs,
    ) {
        self.with_active_seek(|seek| {
            let is_new_current = seek.last_observed_current_video_pts_us != Some(current_pts_us);

            if is_new_current {
                seek.last_observed_current_video_pts_us = Some(current_pts_us);

                if seek.seek_first_current_video_ready_us.is_none() {
                    seek.seek_first_current_video_ready_us = Some(seek.elapsed_us());
                    seek.seek_first_current_video_pts_us = Some(current_pts_us);
                    seek.seek_audio_position_at_first_current_video_us = Some(playback_time_us);
                    if let Some(post_target_audio_position_us) =
                        seek.seek_audio_position_at_first_post_target_video_decoded_us
                    {
                        seek.seek_audio_advanced_between_post_target_decode_and_current_us =
                            Some(playback_time_us.saturating_sub(post_target_audio_position_us));
                    }
                }

                if current_pts_us < seek.target_us {
                    seek.seek_pre_target_current_video_count =
                        seek.seek_pre_target_current_video_count.saturating_add(1);
                } else if seek.seek_target_video_ready_us.is_none() {
                    seek.seek_target_video_ready_us = Some(seek.elapsed_us());
                    seek.seek_target_video_pts_us = Some(current_pts_us);
                }
            }

            if seek.seek_target_video_ready_us.is_none()
                && is_seek_target_ready(seek.target_us, current_pts_us, current_effective_end_us)
            {
                seek.seek_target_video_ready_us = Some(seek.elapsed_us());
                seek.seek_target_video_pts_us = Some(current_pts_us);
            }
        });
    }

    pub(crate) fn observe_seek_video_dropped(&self, frame_pts_us: MediaTimeUs) {
        self.with_active_seek(|seek| {
            if frame_pts_us >= seek.target_us
                && seek.seek_first_current_video_ready_us.is_none()
                && seek.last_observed_current_video_pts_us != Some(frame_pts_us)
            {
                seek.seek_post_target_video_dropped_before_current_count = seek
                    .seek_post_target_video_dropped_before_current_count
                    .saturating_add(1);
            }
        });
    }

    pub(crate) fn observe_seek_audio_output_started(&self) {
        self.with_active_seek(|seek| {
            if !seek.seek_audio_output_started_before_current
                && seek.seek_first_current_video_ready_us.is_none()
            {
                seek.seek_audio_output_started_before_current = true;
            }

            if seek.seek_audio_output_start_us.is_none() {
                seek.seek_audio_output_start_us = Some(seek.elapsed_us());
            }
        });
    }

    pub(crate) fn observe_seek_target_audio_ready(&self) {
        self.with_active_seek(|seek| {
            if seek.seek_target_audio_ready_us.is_none() {
                seek.seek_target_audio_ready_us = Some(seek.elapsed_us());
            }
        });
    }

    pub(crate) fn observe_seek_stable(&self) {
        let mut seek = self.seek.lock().unwrap();
        let Some(mut active) = seek.active.take() else {
            return;
        };

        if active.seek_stable_us.is_none() {
            active.seek_stable_us = Some(active.elapsed_us());
        }

        seek.last_completed = Some(active.snapshot(seek.seek_event_count, false));
    }

    fn with_active_seek(&self, f: impl FnOnce(&mut SeekObservation)) {
        let mut seek = self.seek.lock().unwrap();
        if let Some(active) = seek.active.as_mut() {
            f(active);
        }
    }

    pub(crate) fn snapshot(&self) -> PlayerDiagnosticsSnapshot {
        let seek_snapshot = self.seek.lock().unwrap().snapshot();

        PlayerDiagnosticsSnapshot {
            ffi_lock_wait_last_us: self.ffi_lock_wait_last_us.load(Ordering::Relaxed),
            ffi_lock_wait_max_us: self.ffi_lock_wait_max_us.load(Ordering::Relaxed),
            sync_worker_lock_wait_last_us: self
                .sync_worker_lock_wait_last_us
                .load(Ordering::Relaxed),
            sync_worker_lock_wait_max_us: self.sync_worker_lock_wait_max_us.load(Ordering::Relaxed),
            decode_worker_lock_wait_last_us: self
                .decode_worker_lock_wait_last_us
                .load(Ordering::Relaxed),
            decode_worker_lock_wait_max_us: self
                .decode_worker_lock_wait_max_us
                .load(Ordering::Relaxed),
            worker_deadline_slip_last_us: self.worker_deadline_slip_last_us.load(Ordering::Relaxed),
            worker_deadline_slip_max_us: self.worker_deadline_slip_max_us.load(Ordering::Relaxed),
            stale_audio_discard_event_count: self
                .stale_audio_discard_event_count
                .load(Ordering::Relaxed),
            stale_audio_discard_frame_count: self
                .stale_audio_discard_frame_count
                .load(Ordering::Relaxed),
            stale_audio_discard_last_frame_count: self
                .stale_audio_discard_last_frame_count
                .load(Ordering::Relaxed),
            stale_audio_discard_last_lag_us: self
                .stale_audio_discard_last_lag_us
                .load(Ordering::Relaxed),
            stale_audio_discard_max_lag_us: self
                .stale_audio_discard_max_lag_us
                .load(Ordering::Relaxed),
            render_frames_total: self.render_frames_total.load(Ordering::Relaxed),
            render_passthrough_frames_total: self
                .render_passthrough_frames_total
                .load(Ordering::Relaxed),
            render_passthrough_with_subtitle_intent_frames_total: self
                .render_passthrough_with_subtitle_intent_frames_total
                .load(Ordering::Relaxed),
            render_requires_transform_frames_total: self
                .render_requires_transform_frames_total
                .load(Ordering::Relaxed),
            render_fallback_passthrough_frames_total: self
                .render_fallback_passthrough_frames_total
                .load(Ordering::Relaxed),
            seek_event_count: seek_snapshot.seek_event_count,
            seek_active: seek_snapshot.seek_active,
            last_seek_target_us: seek_snapshot.last_seek_target_us,
            seek_api_duration_us: seek_snapshot.seek_api_duration_us,
            seek_lock_wait_us: seek_snapshot.seek_lock_wait_us,
            seek_ffmpeg_seek_us: seek_snapshot.seek_ffmpeg_seek_us,
            seek_reset_us: seek_snapshot.seek_reset_us,
            seek_first_video_decoded_us: seek_snapshot.seek_first_video_decoded_us,
            seek_first_video_pts_us: seek_snapshot.seek_first_video_pts_us,
            seek_first_post_target_video_decoded_us: seek_snapshot
                .seek_first_post_target_video_decoded_us,
            seek_first_post_target_video_pts_us: seek_snapshot.seek_first_post_target_video_pts_us,
            seek_audio_position_at_first_post_target_video_decoded_us: seek_snapshot
                .seek_audio_position_at_first_post_target_video_decoded_us,
            seek_first_audio_decoder_output_us: seek_snapshot.seek_first_audio_decoder_output_us,
            seek_first_audio_decoded_us: seek_snapshot.seek_first_audio_decoded_us,
            seek_first_current_video_ready_us: seek_snapshot.seek_first_current_video_ready_us,
            seek_first_current_video_pts_us: seek_snapshot.seek_first_current_video_pts_us,
            seek_audio_position_at_first_current_video_us: seek_snapshot
                .seek_audio_position_at_first_current_video_us,
            seek_audio_advanced_between_post_target_decode_and_current_us: seek_snapshot
                .seek_audio_advanced_between_post_target_decode_and_current_us,
            seek_post_target_video_dropped_before_current_count: seek_snapshot
                .seek_post_target_video_dropped_before_current_count,
            seek_audio_output_started_before_current: seek_snapshot
                .seek_audio_output_started_before_current,
            seek_audio_output_start_us: seek_snapshot.seek_audio_output_start_us,
            seek_target_video_ready_us: seek_snapshot.seek_target_video_ready_us,
            seek_target_video_pts_us: seek_snapshot.seek_target_video_pts_us,
            seek_target_audio_ready_us: seek_snapshot.seek_target_audio_ready_us,
            seek_stable_us: seek_snapshot.seek_stable_us,
            seek_pre_target_video_decoded_count: seek_snapshot.seek_pre_target_video_decoded_count,
            seek_pre_target_current_video_count: seek_snapshot.seek_pre_target_current_video_count,
            seek_first_video_packet_pts_us: -1,
            seek_first_video_packet_dts_us: -1,
            seek_first_video_packet_is_key: false,
            seek_first_video_packet_pos: -1,
            seek_video_packets_read: 0,
            seek_audio_packets_read: 0,
        }
    }
}

impl SeekObservation {
    fn new(target_us: MediaTimeUs) -> Self {
        Self {
            requested_at: Instant::now(),
            target_us,
            seek_api_duration_us: None,
            seek_lock_wait_us: None,
            seek_ffmpeg_seek_us: None,
            seek_reset_us: None,
            seek_first_video_decoded_us: None,
            seek_first_video_pts_us: None,
            seek_first_post_target_video_decoded_us: None,
            seek_first_post_target_video_pts_us: None,
            seek_audio_position_at_first_post_target_video_decoded_us: None,
            seek_first_audio_decoder_output_us: None,
            seek_first_audio_decoded_us: None,
            seek_first_current_video_ready_us: None,
            seek_first_current_video_pts_us: None,
            seek_audio_position_at_first_current_video_us: None,
            seek_audio_advanced_between_post_target_decode_and_current_us: None,
            seek_post_target_video_dropped_before_current_count: 0,
            seek_audio_output_started_before_current: false,
            seek_audio_output_start_us: None,
            seek_target_video_ready_us: None,
            seek_target_video_pts_us: None,
            seek_target_audio_ready_us: None,
            seek_stable_us: None,
            seek_pre_target_video_decoded_count: 0,
            seek_pre_target_current_video_count: 0,
            last_observed_current_video_pts_us: None,
        }
    }

    fn elapsed_us(&self) -> MediaTimeUs {
        i64::try_from(self.requested_at.elapsed().as_micros()).unwrap_or(i64::MAX)
    }

    fn snapshot(&self, seek_event_count: u64, seek_active: bool) -> SeekDiagnosticsSnapshot {
        SeekDiagnosticsSnapshot {
            seek_event_count,
            seek_active,
            last_seek_target_us: self.target_us,
            seek_api_duration_us: self.seek_api_duration_us.unwrap_or(-1),
            seek_lock_wait_us: self.seek_lock_wait_us.unwrap_or(-1),
            seek_ffmpeg_seek_us: self.seek_ffmpeg_seek_us.unwrap_or(-1),
            seek_reset_us: self.seek_reset_us.unwrap_or(-1),
            seek_first_video_decoded_us: self.seek_first_video_decoded_us.unwrap_or(-1),
            seek_first_video_pts_us: self.seek_first_video_pts_us.unwrap_or(-1),
            seek_first_post_target_video_decoded_us: self
                .seek_first_post_target_video_decoded_us
                .unwrap_or(-1),
            seek_first_post_target_video_pts_us: self
                .seek_first_post_target_video_pts_us
                .unwrap_or(-1),
            seek_audio_position_at_first_post_target_video_decoded_us: self
                .seek_audio_position_at_first_post_target_video_decoded_us
                .unwrap_or(-1),
            seek_first_audio_decoder_output_us: self
                .seek_first_audio_decoder_output_us
                .unwrap_or(-1),
            seek_first_audio_decoded_us: self.seek_first_audio_decoded_us.unwrap_or(-1),
            seek_first_current_video_ready_us: self.seek_first_current_video_ready_us.unwrap_or(-1),
            seek_first_current_video_pts_us: self.seek_first_current_video_pts_us.unwrap_or(-1),
            seek_audio_position_at_first_current_video_us: self
                .seek_audio_position_at_first_current_video_us
                .unwrap_or(-1),
            seek_audio_advanced_between_post_target_decode_and_current_us: self
                .seek_audio_advanced_between_post_target_decode_and_current_us
                .unwrap_or(-1),
            seek_post_target_video_dropped_before_current_count: self
                .seek_post_target_video_dropped_before_current_count,
            seek_audio_output_started_before_current: self.seek_audio_output_started_before_current,
            seek_audio_output_start_us: self.seek_audio_output_start_us.unwrap_or(-1),
            seek_target_video_ready_us: self.seek_target_video_ready_us.unwrap_or(-1),
            seek_target_video_pts_us: self.seek_target_video_pts_us.unwrap_or(-1),
            seek_target_audio_ready_us: self.seek_target_audio_ready_us.unwrap_or(-1),
            seek_stable_us: self.seek_stable_us.unwrap_or(-1),
            seek_pre_target_video_decoded_count: self.seek_pre_target_video_decoded_count,
            seek_pre_target_current_video_count: self.seek_pre_target_current_video_count,
        }
    }
}

impl SeekDiagnosticsState {
    fn snapshot(&self) -> SeekDiagnosticsSnapshot {
        if let Some(active) = self.active.as_ref() {
            return active.snapshot(self.seek_event_count, true);
        }

        self.last_completed.unwrap_or(SeekDiagnosticsSnapshot {
            seek_event_count: self.seek_event_count,
            ..SeekDiagnosticsSnapshot::default()
        })
    }
}

fn update_atomic_max(target: &AtomicI64, value: MediaTimeUs) {
    let mut current = target.load(Ordering::Relaxed);
    while value > current {
        match target.compare_exchange(current, value, Ordering::Relaxed, Ordering::Relaxed) {
            Ok(_) => break,
            Err(observed) => current = observed,
        }
    }
}

fn is_seek_target_ready(
    target_us: MediaTimeUs,
    current_pts_us: MediaTimeUs,
    current_effective_end_us: Option<MediaTimeUs>,
) -> bool {
    if current_pts_us >= target_us {
        return true;
    }

    current_effective_end_us.is_some_and(|end_us| target_us < end_us)
}
