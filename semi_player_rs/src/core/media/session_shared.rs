use std::sync::{Arc, Mutex};

use crate::core::media::demux_impl::SeekDemuxDiagnosticsSnapshot;
use crate::core::media::session_impl::MediaSession;
use crate::core::media::video_decode::VideoDecodeDiagnosticsSnapshot;

#[derive(Clone)]
#[allow(clippy::arc_with_non_send_sync)]
pub struct SharedMediaSession {
    inner: Arc<Mutex<MediaSession>>,
}

impl SharedMediaSession {
    #[allow(clippy::arc_with_non_send_sync)]
    pub fn new(media_session: MediaSession) -> Self {
        Self {
            inner: Arc::new(Mutex::new(media_session)),
        }
    }

    pub fn with_ref<T>(&self, f: impl FnOnce(&MediaSession) -> T) -> T {
        let guard = self.inner.lock().unwrap();
        f(&guard)
    }

    pub fn with_mut<T>(&self, f: impl FnOnce(&mut MediaSession) -> T) -> T {
        let mut guard = self.inner.lock().unwrap();
        f(&mut guard)
    }

    pub fn seek_diagnostics_snapshot(&self) -> SeekDemuxDiagnosticsSnapshot {
        let guard = self.inner.lock().unwrap();
        guard.seek_diagnostics_snapshot()
    }

    pub fn video_decode_diagnostics_snapshot(&self) -> VideoDecodeDiagnosticsSnapshot {
        let guard = self.inner.lock().unwrap();
        guard.video_decode_diagnostics_snapshot()
    }
}
