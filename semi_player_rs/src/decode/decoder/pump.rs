use std::collections::VecDeque;

use ffmpeg_next as ffmpeg;
use ffmpeg_next::frame;
use ffmpeg_next::Packet;

use crate::decode::decoder::map::{
    audio_duration_us, frame_duration_us, frame_timestamp_us, map_audio_frame, map_video_frame,
};
use crate::decode::decoder::shared::{OpenedAudioDecoder, OpenedVideoDecoder};
use crate::decode::error::MediaOpenError;
use crate::decode::output::{DecodePolicy, DecodedOutput, SkippedAudioFrame, SkippedVideoFrame};
use crate::demux::demux_impl::SeekDemuxDiagnostics;
use crate::util::time::MediaTimeUs;

pub(crate) fn decode_video_packet(
    decoder: &mut OpenedVideoDecoder,
    packet: &Packet,
    outputs: &mut VecDeque<DecodedOutput>,
    seek_diagnostics: &mut SeekDemuxDiagnostics,
    policy: DecodePolicy,
) -> Result<(), MediaOpenError> {
    loop {
        match decoder.decoder.send_packet(packet) {
            Ok(()) => {
                let _ = collect_video_frames(decoder, outputs, seek_diagnostics, false, policy)?;
                return Ok(());
            }
            Err(ffmpeg::Error::Other { errno }) if errno == ffmpeg::error::EAGAIN => {
                let output_count_before = outputs.len();
                let drained =
                    collect_video_frames(decoder, outputs, seek_diagnostics, false, policy)?;
                if drained || outputs.len() == output_count_before {
                    return Err(MediaOpenError::SendPacket(ffmpeg::Error::Other {
                        errno: ffmpeg::error::EAGAIN,
                    }));
                }
            }
            Err(error) => return Err(MediaOpenError::SendPacket(error)),
        }
    }
}

pub(crate) fn decode_audio_packet(
    decoder: &mut OpenedAudioDecoder,
    packet: &Packet,
    outputs: &mut VecDeque<DecodedOutput>,
    seek_diagnostics: &mut SeekDemuxDiagnostics,
    policy: DecodePolicy,
) -> Result<(), MediaOpenError> {
    loop {
        match decoder.decoder.send_packet(packet) {
            Ok(()) => {
                let _ = collect_audio_frames(decoder, outputs, seek_diagnostics, false, policy)?;
                return Ok(());
            }
            Err(ffmpeg::Error::Other { errno }) if errno == ffmpeg::error::EAGAIN => {
                let output_count_before = outputs.len();
                let drained =
                    collect_audio_frames(decoder, outputs, seek_diagnostics, false, policy)?;
                if drained || outputs.len() == output_count_before {
                    return Err(MediaOpenError::SendPacket(ffmpeg::Error::Other {
                        errno: ffmpeg::error::EAGAIN,
                    }));
                }
            }
            Err(error) => return Err(MediaOpenError::SendPacket(error)),
        }
    }
}

pub(crate) fn collect_video_frames(
    decoder: &mut OpenedVideoDecoder,
    outputs: &mut VecDeque<DecodedOutput>,
    seek_diagnostics: &mut SeekDemuxDiagnostics,
    draining: bool,
    policy: DecodePolicy,
) -> Result<bool, MediaOpenError> {
    let mut reached_decoder_eof = false;

    loop {
        let mut frame = frame::Video::empty();
        match decoder.decoder.receive_frame(&mut frame) {
            Ok(()) => {
                let time_base = decoder.decoder.packet_time_base();
                let pts_us =
                    frame_timestamp_us(frame.pts().or_else(|| frame.timestamp()), time_base);
                let duration_us = frame_duration_us(frame.packet().duration, time_base)
                    .or(decoder.estimated_frame_duration_us);

                if should_skip_video_frame_for_seek_recovery(policy, pts_us, duration_us) {
                    outputs.push_back(DecodedOutput::SkippedVideo(SkippedVideoFrame {
                        pts_us,
                        duration_us,
                    }));
                    seek_diagnostics.observe_video_frame(true);
                } else {
                    outputs.push_back(DecodedOutput::Video(map_video_frame(
                        decoder,
                        &frame,
                        pts_us,
                        duration_us,
                    )?));
                    seek_diagnostics.observe_video_frame(false);
                }
            }
            Err(ffmpeg::Error::Other { errno }) if errno == ffmpeg::error::EAGAIN => break,
            Err(ffmpeg::Error::Eof) => {
                reached_decoder_eof = true;
                break;
            }
            Err(error) => return Err(MediaOpenError::ReceiveFrame(error)),
        }
    }

    Ok(draining && reached_decoder_eof)
}

pub(crate) fn collect_audio_frames(
    decoder: &mut OpenedAudioDecoder,
    outputs: &mut VecDeque<DecodedOutput>,
    seek_diagnostics: &mut SeekDemuxDiagnostics,
    draining: bool,
    policy: DecodePolicy,
) -> Result<bool, MediaOpenError> {
    let mut reached_decoder_eof = false;

    loop {
        let mut frame = frame::Audio::empty();
        match decoder.decoder.receive_frame(&mut frame) {
            Ok(()) => {
                let time_base = decoder.decoder.packet_time_base();
                let pts_us =
                    frame_timestamp_us(frame.pts().or_else(|| frame.timestamp()), time_base);
                let duration_us = audio_duration_us(&frame);

                if should_skip_audio_frame_for_seek_recovery(policy, pts_us, duration_us) {
                    outputs.push_back(DecodedOutput::SkippedAudio(SkippedAudioFrame {
                        pts_us,
                        duration_us,
                    }));
                    seek_diagnostics.observe_audio_frame(true);
                    continue;
                }

                outputs.push_back(DecodedOutput::Audio(map_audio_frame(
                    decoder,
                    &frame,
                    pts_us,
                    duration_us,
                )?));
                seek_diagnostics.observe_audio_frame(false);
            }
            Err(ffmpeg::Error::Other { errno }) if errno == ffmpeg::error::EAGAIN => break,
            Err(ffmpeg::Error::Eof) => {
                reached_decoder_eof = true;
                break;
            }
            Err(error) => return Err(MediaOpenError::ReceiveFrame(error)),
        }
    }

    Ok(draining && reached_decoder_eof)
}

pub(crate) fn send_video_decoder_eof(
    decoder: &mut ffmpeg::decoder::Video,
) -> Result<(), MediaOpenError> {
    match decoder.send_eof() {
        Ok(()) | Err(ffmpeg::Error::Eof) => Ok(()),
        Err(error) => Err(MediaOpenError::SendPacket(error)),
    }
}

pub(crate) fn send_audio_decoder_eof(
    decoder: &mut ffmpeg::decoder::Audio,
) -> Result<(), MediaOpenError> {
    match decoder.send_eof() {
        Ok(()) | Err(ffmpeg::Error::Eof) => Ok(()),
        Err(error) => Err(MediaOpenError::SendPacket(error)),
    }
}

pub(crate) fn should_skip_video_frame_for_seek_recovery(
    policy: DecodePolicy,
    pts_us: MediaTimeUs,
    duration_us: Option<MediaTimeUs>,
) -> bool {
    let Some(seek_recovery) = policy.seek_recovery else {
        return false;
    };

    let Some(end_us) = duration_us.and_then(|duration_us| pts_us.checked_add(duration_us)) else {
        return false;
    };

    end_us <= seek_recovery.target_video_us
}

pub(crate) fn should_skip_audio_frame_for_seek_recovery(
    policy: DecodePolicy,
    pts_us: MediaTimeUs,
    duration_us: Option<MediaTimeUs>,
) -> bool {
    let Some(seek_recovery) = policy.seek_recovery else {
        return false;
    };

    let Some(end_us) = duration_us.and_then(|duration_us| pts_us.checked_add(duration_us)) else {
        return false;
    };

    end_us <= seek_recovery.target_video_us
}

#[cfg(test)]
mod tests {
    use super::{
        should_skip_audio_frame_for_seek_recovery, should_skip_video_frame_for_seek_recovery,
    };
    use crate::decode::{DecodePolicy, SeekRecoveryPolicy};

    #[test]
    fn seek_recovery_skips_frame_that_ends_before_target() {
        let policy = DecodePolicy {
            seek_recovery: Some(SeekRecoveryPolicy {
                target_video_us: 10_000,
            }),
        };

        assert!(should_skip_video_frame_for_seek_recovery(
            policy,
            5_000,
            Some(4_000),
        ));
    }

    #[test]
    fn seek_recovery_keeps_frame_that_covers_target() {
        let policy = DecodePolicy {
            seek_recovery: Some(SeekRecoveryPolicy {
                target_video_us: 10_000,
            }),
        };

        assert!(!should_skip_video_frame_for_seek_recovery(
            policy,
            5_000,
            Some(6_000),
        ));
    }

    #[test]
    fn seek_recovery_skips_audio_frame_that_ends_before_target() {
        let policy = DecodePolicy {
            seek_recovery: Some(SeekRecoveryPolicy {
                target_video_us: 10_000,
            }),
        };

        assert!(should_skip_audio_frame_for_seek_recovery(
            policy,
            5_000,
            Some(4_000),
        ));
    }

    #[test]
    fn seek_recovery_keeps_audio_frame_that_covers_target() {
        let policy = DecodePolicy {
            seek_recovery: Some(SeekRecoveryPolicy {
                target_video_us: 10_000,
            }),
        };

        assert!(!should_skip_audio_frame_for_seek_recovery(
            policy,
            5_000,
            Some(6_000),
        ));
    }
}
