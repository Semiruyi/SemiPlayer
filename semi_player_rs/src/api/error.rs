use std::ffi::c_int;

pub type ResultCode = c_int;

pub const SEMI_OK: ResultCode = 0;
pub const SEMI_E_INVALID_ARG: ResultCode = -1;
pub const SEMI_E_INVALID_STATE: ResultCode = -2;
pub const SEMI_E_MEDIA_OPEN_FAILED: ResultCode = -3;
pub const SEMI_E_MEDIA_PROBE_FAILED: ResultCode = -4;
