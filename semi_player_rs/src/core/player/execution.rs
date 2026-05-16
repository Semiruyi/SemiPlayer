mod decode_supply;
mod playback_advance;

use crate::api::error::{ResultCode, SEMI_OK};
use crate::core::player::handle::SemiPlayerHandle;

pub use decode_supply::decode_supply;
pub(crate) use decode_supply::{apply_decoded_output, poll_decoded_output_once};
pub use playback_advance::advance_playback;
pub(crate) use playback_advance::{
    execute_playback_plan, finish_playback_advance, plan_playback_advance,
};

pub fn execute_playback_cycle(
    player: &mut SemiPlayerHandle,
    should_decode: bool,
    decode_iterations: u32,
) -> ResultCode {
    advance_playback(player);

    if should_decode {
        let code = decode_supply(player, decode_iterations);
        if code != SEMI_OK {
            return code;
        }
    }

    advance_playback(player);
    SEMI_OK
}
