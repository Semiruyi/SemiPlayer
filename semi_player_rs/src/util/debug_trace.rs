use std::fs::{self, OpenOptions};
use std::io::Write;
use std::path::PathBuf;
use std::sync::{Mutex, OnceLock};
use std::time::{SystemTime, UNIX_EPOCH};

fn trace_path() -> &'static PathBuf {
    static TRACE_PATH: OnceLock<PathBuf> = OnceLock::new();
    TRACE_PATH.get_or_init(|| {
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("runtime-trace.log")
    })
}

fn trace_lock() -> &'static Mutex<()> {
    static TRACE_LOCK: OnceLock<Mutex<()>> = OnceLock::new();
    TRACE_LOCK.get_or_init(|| Mutex::new(()))
}

pub fn reset_trace_file() {
    let _guard = trace_lock().lock().unwrap();
    let _ = fs::write(trace_path(), []);
}

pub fn append_trace_line(message: &str) {
    let _guard = trace_lock().lock().unwrap();
    let timestamp_ms = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_millis())
        .unwrap_or(0);

    let Ok(mut file) = OpenOptions::new()
        .create(true)
        .append(true)
        .open(trace_path())
    else {
        return;
    };

    let _ = writeln!(file, "[{timestamp_ms}] {message}");
}
