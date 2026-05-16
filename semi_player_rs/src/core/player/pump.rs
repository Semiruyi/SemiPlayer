use crate::api::error::{ResultCode, SEMI_E_INVALID_STATE};
use crate::core::player::execution::execute_playback_cycle;
use crate::core::player::handle::SemiPlayerHandle;
use crate::core::player::schedule::PlayerScheduleService;

pub fn pump_player(player: &mut SemiPlayerHandle, max_iterations: u32) -> ResultCode {
    if !player.is_media_loaded() {
        return SEMI_E_INVALID_STATE;
    }

    if player.opened_media.is_none() {
        return SEMI_E_INVALID_STATE;
    }

    execute_playback_cycle(
        player,
        PlayerScheduleService::evaluate_decode(player).should_decode_now,
        max_iterations,
    )
}
