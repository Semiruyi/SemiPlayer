use std::ffi::c_int;

pub type ResultCode = c_int;

pub const SEMI_OK: ResultCode = 0;
pub const SEMI_E_INVALID_ARG: ResultCode = -1;
pub const SEMI_E_INVALID_STATE: ResultCode = -2;
