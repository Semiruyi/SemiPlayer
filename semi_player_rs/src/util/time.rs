pub type MediaTimeUs = i64;

pub const fn ms_to_us(ms: i64) -> MediaTimeUs {
    ms.saturating_mul(1_000)
}

pub const fn us_to_ms(us: MediaTimeUs) -> i64 {
    us / 1_000
}
