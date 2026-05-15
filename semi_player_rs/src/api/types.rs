#[repr(u32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PlayerState {
    Idle = 0,
    Ready = 1,
    Playing = 2,
    Paused = 3,
}

impl PlayerState {
    pub const fn as_raw(self) -> u32 {
        self as u32
    }

    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(Self::Idle),
            1 => Some(Self::Ready),
            2 => Some(Self::Playing),
            3 => Some(Self::Paused),
            _ => None,
        }
    }
}
