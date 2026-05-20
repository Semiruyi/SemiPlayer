use std::collections::VecDeque;

use crate::scheduler::snapshot::StageMap;
use crate::scheduler::types::{SchedulerDecision, SchedulerEvent};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SchedulerPhase {
    Idle,
    Dispatching,
}

impl Default for SchedulerPhase {
    fn default() -> Self {
        Self::Idle
    }
}

#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct SchedulerTraceState {
    pub phase: SchedulerPhase,
    pub queued_events: usize,
    pub processed_events: u64,
    pub last_event: Option<SchedulerEvent>,
    pub last_decision: Option<SchedulerDecision>,
    pub stages: StageMap,
}

#[derive(Debug, Default)]
pub struct SchedulerState {
    phase: SchedulerPhase,
    queue: VecDeque<SchedulerEvent>,
    processed_events: u64,
    last_event: Option<SchedulerEvent>,
    last_decision: Option<SchedulerDecision>,
    stages: StageMap,
}

impl SchedulerState {
    pub fn enqueue(&mut self, event: SchedulerEvent) {
        self.queue.push_back(event);
    }

    pub fn is_dispatching(&self) -> bool {
        self.phase == SchedulerPhase::Dispatching
    }

    pub fn try_begin_dispatch(&mut self) -> bool {
        if self.is_dispatching() {
            return false;
        }

        self.phase = SchedulerPhase::Dispatching;
        true
    }

    pub fn pop_next_event(&mut self) -> Option<SchedulerEvent> {
        self.queue.pop_front()
    }

    pub fn apply_event(&mut self, event: &SchedulerEvent) {
        self.observe_event(event);
    }

    pub fn note_stage_requested(&mut self, stage: crate::scheduler::types::StageId) {
        let stage_state = &mut self.stages[stage];
        stage_state.requested = true;
    }

    pub fn finish_event(&mut self, event: SchedulerEvent, decision: SchedulerDecision) {
        self.processed_events = self.processed_events.saturating_add(1);
        self.last_event = Some(event);
        self.last_decision = Some(decision);
    }

    pub fn end_dispatch(&mut self) {
        self.phase = SchedulerPhase::Idle;
    }

    pub fn trace_snapshot(&self) -> SchedulerTraceState {
        SchedulerTraceState {
            phase: self.phase,
            queued_events: self.queue.len(),
            processed_events: self.processed_events,
            last_event: self.last_event.clone(),
            last_decision: self.last_decision.clone(),
            stages: self.stages,
        }
    }

    pub fn stage_snapshot(&self) -> StageMap {
        self.stages
    }

    fn observe_event(&mut self, event: &SchedulerEvent) {
        match event {
            SchedulerEvent::StageStarted(stage) => {
                let stage_state = &mut self.stages[*stage];
                stage_state.requested = false;
                stage_state.in_flight = true;
                stage_state.blocked = false;
            }
            SchedulerEvent::StageProgress { stage, .. } => {
                let stage_state = &mut self.stages[*stage];
                stage_state.requested = false;
                stage_state.in_flight = false;
                stage_state.blocked = false;
                stage_state.last_progress_generation = self.processed_events;
            }
            SchedulerEvent::StageBlocked(stage) => {
                let stage_state = &mut self.stages[*stage];
                stage_state.requested = false;
                stage_state.in_flight = false;
                stage_state.blocked = true;
            }
            SchedulerEvent::StageIdle(stage) => {
                let stage_state = &mut self.stages[*stage];
                stage_state.requested = false;
                stage_state.in_flight = false;
                stage_state.blocked = false;
            }
            SchedulerEvent::StageRequested(stage) => {
                let stage_state = &mut self.stages[*stage];
                stage_state.requested = true;
            }
            SchedulerEvent::PlaybackDemandChanged
            | SchedulerEvent::PlaybackAdvanced
            | SchedulerEvent::SeekStarted
            | SchedulerEvent::SeekCompleted
            | SchedulerEvent::MediaLoaded
            | SchedulerEvent::MediaUnloaded
            | SchedulerEvent::PlayerStateChanged(_)
            | SchedulerEvent::ShutdownRequested => {}
        }
    }
}

#[cfg(test)]
mod tests {
    use crate::api::types::PlayerState;
    use crate::scheduler::types::{SchedulerDecision, SchedulerEvent, StageId};

    use super::{SchedulerPhase, SchedulerState, SchedulerTraceState};

    #[test]
    fn state_serializes_dispatch_until_end_called() {
        let mut state = SchedulerState::default();

        assert!(state.try_begin_dispatch());
        assert!(!state.try_begin_dispatch());

        state.end_dispatch();

        assert!(state.try_begin_dispatch());
    }

    #[test]
    fn state_tracks_last_event_and_decision() {
        let mut state = SchedulerState::default();
        let event = SchedulerEvent::PlayerStateChanged(PlayerState::Playing);
        let decision = SchedulerDecision {
            wake_playback: true,
            wake_stages: vec![StageId::VideoDecode],
            next_deadline_us: Some(12_345),
        };

        state.enqueue(event.clone());
        let _ = state.try_begin_dispatch();
        assert_eq!(state.pop_next_event(), Some(event.clone()));
        state.apply_event(&event);
        state.finish_event(event.clone(), decision.clone());

        assert_eq!(
            state.trace_snapshot(),
            SchedulerTraceState {
                phase: SchedulerPhase::Dispatching,
                queued_events: 0,
                processed_events: 1,
                last_event: Some(event),
                last_decision: Some(decision),
                stages: state.stage_snapshot(),
            }
        );
    }

    #[test]
    fn stage_progress_clears_in_flight_and_records_progress() {
        let mut state = SchedulerState::default();

        let _ = state.try_begin_dispatch();
        state.apply_event(&SchedulerEvent::StageStarted(StageId::VideoRender));
        state.finish_event(
            SchedulerEvent::StageStarted(StageId::VideoRender),
            SchedulerDecision::default(),
        );
        state.apply_event(&SchedulerEvent::StageProgress {
            stage: StageId::VideoRender,
            produced: Vec::new(),
        });
        state.finish_event(
            SchedulerEvent::StageProgress {
                stage: StageId::VideoRender,
                produced: Vec::new(),
            },
            SchedulerDecision::default(),
        );

        let stages = state.stage_snapshot();
        assert!(!stages[StageId::VideoRender].in_flight);
        assert!(!stages[StageId::VideoRender].blocked);
        assert_eq!(stages[StageId::VideoRender].last_progress_generation, 1);
    }
}
