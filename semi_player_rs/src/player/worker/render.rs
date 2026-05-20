use std::sync::{Arc, Condvar, Mutex};
use std::thread::{self, JoinHandle};

use crate::player::handle::SemiPlayerHandle;

#[derive(Default)]
struct RenderWorkerControl {
    shutdown: bool,
    wake_requested: bool,
    render_requested: bool,
}

pub struct RenderWorkerHandle {
    control: Arc<(Mutex<RenderWorkerControl>, Condvar)>,
    thread: Option<JoinHandle<()>>,
}

impl RenderWorkerHandle {
    pub fn start(player_ptr: *mut SemiPlayerHandle) -> Self {
        let control = Arc::new((Mutex::new(RenderWorkerControl::default()), Condvar::new()));
        let thread_control = Arc::clone(&control);
        let player_addr = player_ptr as usize;

        let thread = thread::Builder::new()
            .name("semi-render-worker".to_string())
            .spawn(move || worker_loop(player_addr, thread_control))
            .expect("failed to start render worker");

        Self {
            control,
            thread: Some(thread),
        }
    }

    pub fn request_render(&self) {
        let (lock, condvar) = &*self.control;
        let mut control = lock.lock().unwrap();
        control.render_requested = true;
        control.wake_requested = true;
        condvar.notify_all();
    }

    pub fn stop(&mut self) {
        let (lock, condvar) = &*self.control;
        {
            let mut control = lock.lock().unwrap();
            control.shutdown = true;
            control.wake_requested = true;
        }
        condvar.notify_all();

        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

#[allow(clippy::needless_pass_by_value)]
fn worker_loop(_player_addr: usize, control: Arc<(Mutex<RenderWorkerControl>, Condvar)>) {
    loop {
        if wait_for_signal(&control) {
            break;
        }

        // Skeleton stage only: wake/sleep lifecycle is live, but render execution
        // still happens on the current synchronous decode path until the next step.
    }
}

fn wait_for_signal(control: &Arc<(Mutex<RenderWorkerControl>, Condvar)>) -> bool {
    let (lock, condvar) = &**control;
    let mut state = lock.lock().unwrap();

    if state.shutdown {
        return true;
    }

    if !state.render_requested {
        loop {
            state = condvar.wait(state).unwrap();
            if state.shutdown {
                return true;
            }
            if state.render_requested || state.wake_requested {
                break;
            }
        }
    }

    if state.wake_requested {
        state.wake_requested = false;
    }

    state.render_requested = false;
    state.wake_requested = false;
    false
}

