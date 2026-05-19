use ffmpeg_next::Packet;
use ffmpeg_next::{Rational, Rescale};

use crate::util::time::MediaTimeUs;

use super::probe::StreamKind;

#[derive(Clone, Copy, Debug, Default)]
pub struct SeekDemuxDiagnosticsSnapshot {
    pub first_video_packet_pts_us: MediaTimeUs,
    pub first_video_packet_dts_us: MediaTimeUs,
    pub first_video_packet_is_key: bool,
    pub first_video_packet_pos: i64,
    pub first_video_packet_stream_index: i64,
    pub first_video_packet_stream_kind: StreamKind,
    pub video_packets_read: u64,
    pub audio_packets_read: u64,
    pub video_frames_output: u64,
    pub video_frames_skipped: u64,
    pub audio_frames_output: u64,
    pub audio_frames_skipped: u64,
    pub expected_left_keyframe_pts_us: MediaTimeUs,
    pub expected_left_keyframe_dts_us: MediaTimeUs,
}

#[derive(Debug, Default)]
pub(crate) struct SeekDemuxDiagnostics {
    active: bool,
    first_video_packet_pts_us: Option<MediaTimeUs>,
    first_video_packet_dts_us: Option<MediaTimeUs>,
    first_video_packet_is_key: bool,
    first_video_packet_pos: Option<i64>,
    first_video_packet_stream_index: Option<i64>,
    first_video_packet_stream_kind: StreamKind,
    video_packets_read: u64,
    audio_packets_read: u64,
    video_frames_output: u64,
    video_frames_skipped: u64,
    audio_frames_output: u64,
    audio_frames_skipped: u64,
    expected_left_keyframe_pts_us: Option<MediaTimeUs>,
    expected_left_keyframe_dts_us: Option<MediaTimeUs>,
    last_completed: SeekDemuxDiagnosticsSnapshot,
}

impl SeekDemuxDiagnostics {
    pub(crate) fn begin(&mut self) {
        self.active = true;
        self.first_video_packet_pts_us = None;
        self.first_video_packet_dts_us = None;
        self.first_video_packet_is_key = false;
        self.first_video_packet_pos = None;
        self.first_video_packet_stream_index = None;
        self.first_video_packet_stream_kind = StreamKind::Unknown;
        self.video_packets_read = 0;
        self.audio_packets_read = 0;
        self.video_frames_output = 0;
        self.video_frames_skipped = 0;
        self.audio_frames_output = 0;
        self.audio_frames_skipped = 0;
        self.expected_left_keyframe_pts_us = None;
        self.expected_left_keyframe_dts_us = None;
        self.last_completed = SeekDemuxDiagnosticsSnapshot::default();
    }

    pub(crate) fn observe_packet(
        &mut self,
        stream_index: usize,
        packet: &Packet,
        best_video_stream_index: Option<usize>,
        best_audio_stream_index: Option<usize>,
        time_base: Option<Rational>,
    ) {
        if !self.active {
            return;
        }

        if Some(stream_index) == best_video_stream_index {
            self.video_packets_read = self.video_packets_read.saturating_add(1);
            if self.first_video_packet_pts_us.is_none() {
                self.first_video_packet_pts_us = Some(packet_timestamp_us(packet.pts(), time_base));
                self.first_video_packet_dts_us = Some(packet_timestamp_us(packet.dts(), time_base));
                self.first_video_packet_is_key = packet.is_key();
                self.first_video_packet_pos = i64::try_from(packet.position()).ok();
                self.first_video_packet_stream_index = i64::try_from(stream_index).ok();
                self.first_video_packet_stream_kind = StreamKind::Video;
            }
        } else if Some(stream_index) == best_audio_stream_index {
            self.audio_packets_read = self.audio_packets_read.saturating_add(1);
        }
    }

    pub(crate) fn observe_expected_left_keyframe(
        &mut self,
        expected_left_keyframe: Option<(MediaTimeUs, MediaTimeUs)>,
    ) {
        if let Some((pts_us, dts_us)) = expected_left_keyframe {
            self.expected_left_keyframe_pts_us = Some(pts_us);
            self.expected_left_keyframe_dts_us = Some(dts_us);
        }
    }

    pub(crate) fn observe_video_frame(&mut self, skipped: bool) {
        if !self.active {
            return;
        }

        if skipped {
            self.video_frames_skipped = self.video_frames_skipped.saturating_add(1);
        } else {
            self.video_frames_output = self.video_frames_output.saturating_add(1);
        }
    }

    pub(crate) fn observe_audio_frame(&mut self, skipped: bool) {
        if !self.active {
            return;
        }

        if skipped {
            self.audio_frames_skipped = self.audio_frames_skipped.saturating_add(1);
        } else {
            self.audio_frames_output = self.audio_frames_output.saturating_add(1);
        }
    }

    pub(crate) fn finish(&mut self) {
        if !self.active {
            return;
        }

        self.last_completed = self.snapshot();
        self.active = false;
    }

    #[allow(clippy::similar_names)]
    pub(crate) fn snapshot(&self) -> SeekDemuxDiagnosticsSnapshot {
        let first_pts = self.first_video_packet_pts_us.unwrap_or(-1);
        let first_dts = self.first_video_packet_dts_us.unwrap_or(-1);
        let first_pos = self.first_video_packet_pos.unwrap_or(-1);
        let first_stream_index = self.first_video_packet_stream_index.unwrap_or(-1);
        let video_packets_read = self.video_packets_read;
        let audio_packets_read = self.audio_packets_read;
        let video_frames_output = self.video_frames_output;
        let video_frames_skipped = self.video_frames_skipped;
        let audio_frames_output = self.audio_frames_output;
        let audio_frames_skipped = self.audio_frames_skipped;
        let expected_keyframe_pts = self.expected_left_keyframe_pts_us.unwrap_or(-1);
        let expected_keyframe_dts = self.expected_left_keyframe_dts_us.unwrap_or(-1);

        if self.active {
            SeekDemuxDiagnosticsSnapshot {
                first_video_packet_pts_us: first_pts,
                first_video_packet_dts_us: first_dts,
                first_video_packet_is_key: self.first_video_packet_is_key,
                first_video_packet_pos: first_pos,
                first_video_packet_stream_index: first_stream_index,
                first_video_packet_stream_kind: self.first_video_packet_stream_kind,
                video_packets_read,
                audio_packets_read,
                video_frames_output,
                video_frames_skipped,
                audio_frames_output,
                audio_frames_skipped,
                expected_left_keyframe_pts_us: expected_keyframe_pts,
                expected_left_keyframe_dts_us: expected_keyframe_dts,
            }
        } else {
            self.last_completed
        }
    }
}

fn packet_timestamp_us(timestamp: Option<i64>, time_base: Option<Rational>) -> MediaTimeUs {
    match (timestamp, time_base) {
        (Some(value), Some(time_base)) => value.rescale(time_base, (1, 1_000_000)),
        _ => -1,
    }
}
