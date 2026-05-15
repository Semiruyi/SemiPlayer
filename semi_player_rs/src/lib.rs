use std::ffi::{c_char, c_double, c_int, CStr, CString};
use std::ptr;
use std::sync::atomic::{AtomicU32, Ordering};

const SEMI_OK: c_int = 0;
const SEMI_E_INVALID_ARG: c_int = -1;
const SEMI_E_INVALID_STATE: c_int = -2;

const SEMI_PLAYER_STATE_IDLE: u32 = 0;
const SEMI_PLAYER_STATE_READY: u32 = 1;
const SEMI_PLAYER_STATE_PLAYING: u32 = 2;
const SEMI_PLAYER_STATE_PAUSED: u32 = 3;

#[repr(C)]
pub struct SemiPlayerHandle {
    state: AtomicU32,
    speed: c_double,
    position_ms: i64,
    duration_ms: i64,
    media_path: Option<String>,
    subtitles_visible: bool,
}

impl SemiPlayerHandle {
    fn new() -> Self {
        Self {
            state: AtomicU32::new(SEMI_PLAYER_STATE_IDLE),
            speed: 1.0,
            position_ms: 0,
            duration_ms: 0,
            media_path: None,
            subtitles_visible: true,
        }
    }

    fn is_media_loaded(&self) -> bool {
        self.media_path.is_some()
    }
}

fn with_player_mut<T>(
    player: *mut SemiPlayerHandle,
    f: impl FnOnce(&mut SemiPlayerHandle) -> T,
) -> Result<T, c_int> {
    if player.is_null() {
        return Err(SEMI_E_INVALID_ARG);
    }

    let player = unsafe { &mut *player };
    Ok(f(player))
}

fn cstr_to_string(input: *const c_char) -> Result<String, c_int> {
    if input.is_null() {
        return Err(SEMI_E_INVALID_ARG);
    }

    let c_str = unsafe { CStr::from_ptr(input) };
    Ok(c_str.to_string_lossy().into_owned())
}

#[no_mangle]
pub extern "C" fn semi_free_string(s: *mut c_char) {
    if !s.is_null() {
        unsafe { drop(CString::from_raw(s)) };
    }
}

#[no_mangle]
pub extern "C" fn semi_player_create(out_player: *mut *mut SemiPlayerHandle) -> c_int {
    if out_player.is_null() {
        return SEMI_E_INVALID_ARG;
    }

    let player = Box::new(SemiPlayerHandle::new());
    unsafe {
        *out_player = Box::into_raw(player);
    }
    SEMI_OK
}

#[no_mangle]
pub extern "C" fn semi_player_destroy(player: *mut SemiPlayerHandle) {
    if !player.is_null() {
        unsafe { drop(Box::from_raw(player)) };
    }
}

#[no_mangle]
pub extern "C" fn semi_player_open(
    player: *mut SemiPlayerHandle,
    path_utf8: *const c_char,
) -> c_int {
    let path = match cstr_to_string(path_utf8) {
        Ok(path) if !path.trim().is_empty() => path,
        Ok(_) => return SEMI_E_INVALID_ARG,
        Err(code) => return code,
    };

    match with_player_mut(player, |player| {
        player.media_path = Some(path);
        player.position_ms = 0;
        player.duration_ms = 0;
        player.speed = 1.0;
        player.subtitles_visible = true;
        player.state.store(SEMI_PLAYER_STATE_READY, Ordering::SeqCst);
    }) {
        Ok(_) => SEMI_OK,
        Err(code) => code,
    }
}

#[no_mangle]
pub extern "C" fn semi_player_play(player: *mut SemiPlayerHandle) -> c_int {
    match with_player_mut(player, |player| {
        if !player.is_media_loaded() {
            return SEMI_E_INVALID_STATE;
        }

        player.state.store(SEMI_PLAYER_STATE_PLAYING, Ordering::SeqCst);
        SEMI_OK
    }) {
        Ok(code) => code,
        Err(code) => code,
    }
}

#[no_mangle]
pub extern "C" fn semi_player_pause(player: *mut SemiPlayerHandle) -> c_int {
    match with_player_mut(player, |player| {
        if !player.is_media_loaded() {
            return SEMI_E_INVALID_STATE;
        }

        player.state.store(SEMI_PLAYER_STATE_PAUSED, Ordering::SeqCst);
        SEMI_OK
    }) {
        Ok(code) => code,
        Err(code) => code,
    }
}

#[no_mangle]
pub extern "C" fn semi_player_seek(
    player: *mut SemiPlayerHandle,
    position_ms: i64,
    _exact: c_int,
) -> c_int {
    match with_player_mut(player, |player| {
        if !player.is_media_loaded() {
            return SEMI_E_INVALID_STATE;
        }
        if position_ms < 0 {
            return SEMI_E_INVALID_ARG;
        }

        player.position_ms = position_ms;
        SEMI_OK
    }) {
        Ok(code) => code,
        Err(code) => code,
    }
}

#[no_mangle]
pub extern "C" fn semi_player_reset(player: *mut SemiPlayerHandle) -> c_int {
    match with_player_mut(player, |player| {
        player.media_path = None;
        player.position_ms = 0;
        player.duration_ms = 0;
        player.speed = 1.0;
        player.subtitles_visible = true;
        player.state.store(SEMI_PLAYER_STATE_IDLE, Ordering::SeqCst);
        SEMI_OK
    }) {
        Ok(code) => code,
        Err(code) => code,
    }
}

#[no_mangle]
pub extern "C" fn semi_player_set_speed(player: *mut SemiPlayerHandle, speed: c_double) -> c_int {
    match with_player_mut(player, |player| {
        if !player.is_media_loaded() {
            return SEMI_E_INVALID_STATE;
        }
        if !speed.is_finite() || speed <= 0.0 {
            return SEMI_E_INVALID_ARG;
        }

        player.speed = speed;
        SEMI_OK
    }) {
        Ok(code) => code,
        Err(code) => code,
    }
}

#[no_mangle]
pub extern "C" fn semi_player_set_subtitle_visible(
    player: *mut SemiPlayerHandle,
    visible: c_int,
) -> c_int {
    match with_player_mut(player, |player| {
        if !player.is_media_loaded() {
            return SEMI_E_INVALID_STATE;
        }

        player.subtitles_visible = visible != 0;
        SEMI_OK
    }) {
        Ok(code) => code,
        Err(code) => code,
    }
}

#[no_mangle]
pub extern "C" fn semi_player_get_state(
    player: *mut SemiPlayerHandle,
    out_state: *mut u32,
) -> c_int {
    if out_state.is_null() {
        return SEMI_E_INVALID_ARG;
    }

    match with_player_mut(player, |player| {
        unsafe {
            *out_state = player.state.load(Ordering::SeqCst);
        }
    }) {
        Ok(_) => SEMI_OK,
        Err(code) => code,
    }
}

#[no_mangle]
pub extern "C" fn semi_player_get_position_ms(
    player: *mut SemiPlayerHandle,
    out_position_ms: *mut i64,
) -> c_int {
    if out_position_ms.is_null() {
        return SEMI_E_INVALID_ARG;
    }

    match with_player_mut(player, |player| {
        unsafe {
            *out_position_ms = player.position_ms;
        }
    }) {
        Ok(_) => SEMI_OK,
        Err(code) => code,
    }
}

#[no_mangle]
pub extern "C" fn semi_player_get_duration_ms(
    player: *mut SemiPlayerHandle,
    out_duration_ms: *mut i64,
) -> c_int {
    if out_duration_ms.is_null() {
        return SEMI_E_INVALID_ARG;
    }

    match with_player_mut(player, |player| {
        unsafe {
            *out_duration_ms = player.duration_ms;
        }
    }) {
        Ok(_) => SEMI_OK,
        Err(code) => code,
    }
}

#[no_mangle]
pub extern "C" fn semi_ffmpeg_version_string() -> *mut c_char {
    let version = ffmpeg_next::util::version();
    match CString::new(format!("FFmpeg version: {}", version)) {
        Ok(value) => value.into_raw(),
        Err(_) => ptr::null_mut(),
    }
}
