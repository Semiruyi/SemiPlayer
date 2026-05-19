use ffmpeg_next as ffmpeg;
use ffmpeg_next::{Rational, Rescale};

use crate::util::time::MediaTimeUs;

pub fn probe_expected_left_keyframe_pts(
    path: &str,
    best_video_stream_index: Option<usize>,
    target_us: MediaTimeUs,
) -> Option<(MediaTimeUs, MediaTimeUs)> {
    const VIDEO_PACKET_SCAN_LIMIT: usize = 512;

    let video_stream_index = best_video_stream_index?;
    let mut input = ffmpeg::format::input(path).ok()?;
    let target = target_us.rescale((1, 1_000_000), ffmpeg::rescale::TIME_BASE);
    // Diagnostic-only heuristic: reopen the input and scan nearby main-video packets
    // so we can compare the player's actual anchor against an expected left keyframe.
    let _ = input.seek(target, ..target);

    let mut best: Option<(MediaTimeUs, MediaTimeUs)> = None;
    let mut seen_past_target = false;
    let mut video_packets_scanned = 0usize;

    for (stream, packet) in input.packets() {
        if stream.index() != video_stream_index {
            continue;
        }

        video_packets_scanned = video_packets_scanned.saturating_add(1);
        let time_base = stream.time_base();
        let pts_us = packet_timestamp_us(packet.pts(), Some(time_base));
        let dts_us = packet_timestamp_us(packet.dts(), Some(time_base));

        if pts_us > target_us && dts_us > target_us {
            seen_past_target = true;
            if best.is_some() {
                break;
            }
        }

        if packet.is_key() && pts_us >= 0 && pts_us <= target_us {
            best = Some((pts_us, dts_us));
        }

        if seen_past_target && best.is_some() {
            break;
        }

        if video_packets_scanned >= VIDEO_PACKET_SCAN_LIMIT {
            break;
        }
    }

    best
}

pub fn probe_expected_right_keyframe_pts(
    path: &str,
    best_video_stream_index: Option<usize>,
    current_us: MediaTimeUs,
) -> Option<MediaTimeUs> {
    const VIDEO_PACKET_SCAN_LIMIT: usize = 512;

    let video_stream_index = best_video_stream_index?;
    let mut input = ffmpeg::format::input(path).ok()?;
    let target = current_us.rescale((1, 1_000_000), ffmpeg::rescale::TIME_BASE);
    let _ = input.seek(target, ..target);

    let mut video_packets_scanned = 0usize;

    for (stream, packet) in input.packets() {
        if stream.index() != video_stream_index {
            continue;
        }

        video_packets_scanned = video_packets_scanned.saturating_add(1);
        let time_base = stream.time_base();
        let pts_us = packet_timestamp_us(packet.pts(), Some(time_base));

        if packet.is_key() && pts_us > current_us {
            return Some(pts_us);
        }

        if video_packets_scanned >= VIDEO_PACKET_SCAN_LIMIT {
            break;
        }
    }

    None
}

fn packet_timestamp_us(timestamp: Option<i64>, time_base: Option<Rational>) -> MediaTimeUs {
    match (timestamp, time_base) {
        (Some(value), Some(time_base)) => value.rescale(time_base, (1, 1_000_000)),
        _ => -1,
    }
}
