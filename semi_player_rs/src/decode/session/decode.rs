use std::collections::VecDeque;

use ffmpeg_next as ffmpeg;
use ffmpeg_next::Packet;

use crate::decode::decoder::{
    DecoderDrainingState, MediaPacket, OpenedAudioDecoder, OpenedVideoDecoder,
    collect_audio_frames, collect_video_frames, decode_audio_packet, decode_video_packet,
    send_audio_decoder_eof, send_video_decoder_eof,
};
use crate::decode::error::MediaOpenError;
use crate::decode::output::{DecodePolicy, DecodedOutput, DecodedOutputPoll};
use crate::demux::demux_impl::SeekDemuxDiagnostics;
use crate::demux::probe::MediaInfo;

#[derive(Default)]
pub(crate) struct SessionDecodeState {
    pending_outputs: VecDeque<DecodedOutput>,
    draining_state: DecoderDrainingState,
}

impl SessionDecodeState {
    pub(crate) fn reset(
        &mut self,
        video_decoder: &mut Option<OpenedVideoDecoder>,
        audio_decoder: &mut Option<OpenedAudioDecoder>,
    ) {
        if let Some(video_decoder) = video_decoder.as_mut() {
            video_decoder.decoder.flush();
        }

        if let Some(audio_decoder) = audio_decoder.as_mut() {
            audio_decoder.decoder.flush();
        }

        self.pending_outputs.clear();
        self.draining_state = DecoderDrainingState::default();
    }

    pub(crate) fn poll_decoded_output(
        &mut self,
        input: &mut ffmpeg::format::context::Input,
        info: &MediaInfo,
        video_decoder: &mut Option<OpenedVideoDecoder>,
        audio_decoder: &mut Option<OpenedAudioDecoder>,
        seek_diagnostics: &mut SeekDemuxDiagnostics,
        policy: DecodePolicy,
        max_packets: usize,
    ) -> Result<DecodedOutputPoll, MediaOpenError> {
        if let Some(output) = self.pending_outputs.pop_front() {
            return Ok(DecodedOutputPoll::Output(output));
        }

        if self.draining_state.input_exhausted {
            self.collect_drained_outputs(video_decoder, audio_decoder, seek_diagnostics, policy)?;

            if let Some(output) = self.pending_outputs.pop_front() {
                return Ok(DecodedOutputPoll::Output(output));
            }

            if self.has_fully_drained() && !self.draining_state.end_of_stream_emitted {
                self.draining_state.end_of_stream_emitted = true;
                return Ok(DecodedOutputPoll::Output(DecodedOutput::EndOfStream));
            }

            return Ok(if self.has_fully_drained() {
                DecodedOutputPoll::Finished
            } else {
                DecodedOutputPoll::Pending
            });
        }

        let packet_budget = max_packets.max(1);
        for _ in 0..packet_budget {
            let Some(media_packet) = read_next_packet(input, info, seek_diagnostics)? else {
                self.enter_draining_mode(video_decoder, audio_decoder)?;
                return Ok(DecodedOutputPoll::Pending);
            };

            if video_decoder
                .as_ref()
                .is_some_and(|decoder| decoder.index == media_packet.stream_index)
            {
                let video_decoder = video_decoder.as_mut().expect("video decoder exists");
                decode_video_packet(
                    video_decoder,
                    &media_packet.packet,
                    &mut self.pending_outputs,
                    seek_diagnostics,
                    policy,
                )?;
            } else if audio_decoder
                .as_ref()
                .is_some_and(|decoder| decoder.index == media_packet.stream_index)
            {
                let audio_decoder = audio_decoder.as_mut().expect("audio decoder exists");
                decode_audio_packet(
                    audio_decoder,
                    &media_packet.packet,
                    &mut self.pending_outputs,
                    seek_diagnostics,
                    policy,
                )?;
            }

            if let Some(output) = self.pending_outputs.pop_front() {
                return Ok(DecodedOutputPoll::Output(output));
            }
        }

        Ok(DecodedOutputPoll::Pending)
    }

    fn enter_draining_mode(
        &mut self,
        video_decoder: &mut Option<OpenedVideoDecoder>,
        audio_decoder: &mut Option<OpenedAudioDecoder>,
    ) -> Result<(), MediaOpenError> {
        self.draining_state.input_exhausted = true;

        if let Some(video_decoder) = video_decoder.as_mut() {
            send_video_decoder_eof(&mut video_decoder.decoder)?;
            self.draining_state.video_eof_sent = true;
        } else {
            self.draining_state.video_drained = true;
        }

        if let Some(audio_decoder) = audio_decoder.as_mut() {
            send_audio_decoder_eof(&mut audio_decoder.decoder)?;
            self.draining_state.audio_eof_sent = true;
        } else {
            self.draining_state.audio_drained = true;
        }

        Ok(())
    }

    fn collect_drained_outputs(
        &mut self,
        video_decoder: &mut Option<OpenedVideoDecoder>,
        audio_decoder: &mut Option<OpenedAudioDecoder>,
        seek_diagnostics: &mut SeekDemuxDiagnostics,
        policy: DecodePolicy,
    ) -> Result<(), MediaOpenError> {
        if !self.draining_state.video_drained {
            if let Some(video_decoder) = video_decoder.as_mut() {
                self.draining_state.video_drained = collect_video_frames(
                    video_decoder,
                    &mut self.pending_outputs,
                    seek_diagnostics,
                    true,
                    policy,
                )?;
            } else {
                self.draining_state.video_drained = true;
            }
        }

        if !self.draining_state.audio_drained {
            if let Some(audio_decoder) = audio_decoder.as_mut() {
                self.draining_state.audio_drained = collect_audio_frames(
                    audio_decoder,
                    &mut self.pending_outputs,
                    seek_diagnostics,
                    true,
                    policy,
                )?;
            } else {
                self.draining_state.audio_drained = true;
            }
        }

        Ok(())
    }

    fn has_fully_drained(&self) -> bool {
        self.draining_state.video_drained && self.draining_state.audio_drained
    }
}

fn read_next_packet(
    input: &mut ffmpeg::format::context::Input,
    info: &MediaInfo,
    seek_diagnostics: &mut SeekDemuxDiagnostics,
) -> Result<Option<MediaPacket>, MediaOpenError> {
    let mut packet = Packet::empty();
    match packet.read(input) {
        Ok(()) => {
            let stream_index = packet.stream();
            seek_diagnostics.observe_packet(
                stream_index,
                &packet,
                info.best_video_stream_index,
                info.best_audio_stream_index,
                input.stream(stream_index).map(|stream| stream.time_base()),
            );
            Ok(Some(MediaPacket {
                stream_index,
                packet,
            }))
        }
        Err(ffmpeg::Error::Eof) => Ok(None),
        Err(error) => Err(MediaOpenError::ReadPacket(error)),
    }
}
