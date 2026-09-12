const CLEANUP_RETRY_DELAY_MS: u32 = 60_000;

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct CleanupSession {
    pub tracking_session_number: u32,
    pub bytes: u64,
    pub inactive: bool,
    pub all_points_confirmed: bool,
    pub diagnostic_log_confirmed: bool,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct CleanupPlan {
    pub cleanup_required: bool,
    pub target_reached: bool,
    pub protected_data_remains: bool,
    pub sessions_to_delete: Vec<u32>,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum CleanupStorageStatus {
    #[default]
    Ok,
    LockUnavailable,
    MetricUnavailable,
    ScanFailed,
    DeleteFailed,
    DiagnosticWriteFailed,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum CleanupOutcome {
    #[default]
    NotRequired,
    TargetReached,
    WaitingForDelivery,
    RetryRequired,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct CleanupRun {
    pub outcome: CleanupOutcome,
    pub failure: CleanupStorageStatus,
    pub deleted_sessions: Vec<u32>,
}

pub trait StorageCleanupStorage {
    fn read_usage(&mut self) -> Result<(u64, u64), CleanupStorageStatus>;
    fn read_cleanup_sessions(&mut self) -> Result<Vec<CleanupSession>, CleanupStorageStatus>;
    fn delete_session_files(
        &mut self,
        tracking_session_number: u32,
    ) -> Result<(), CleanupStorageStatus>;
    fn forget_deleted_session(&mut self, tracking_session_number: u32);
    fn flush_pending_cleanup_diagnostic(&mut self) -> Result<(), CleanupStorageStatus>;
    fn persist_cleanup_diagnostic(&mut self, message: &str) -> Result<(), CleanupStorageStatus>;
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum CleanupTrigger {
    #[default]
    Startup,
    DeliveryConfirmation,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct StorageCleanupSchedule {
    pending: bool,
    retry_at_milliseconds: u32,
    trigger: CleanupTrigger,
}

impl StorageCleanupSchedule {
    pub fn request(&mut self, trigger: CleanupTrigger) {
        self.pending = true;
        self.retry_at_milliseconds = 0;
        self.trigger = trigger;
    }

    pub fn ready(&self, now_milliseconds: u32) -> bool {
        self.pending && now_milliseconds.wrapping_sub(self.retry_at_milliseconds) as i32 >= 0
    }

    pub fn record_run(&mut self, run: &CleanupRun, now_milliseconds: u32) {
        if run.outcome == CleanupOutcome::RetryRequired {
            self.retry_at_milliseconds = now_milliseconds.wrapping_add(CLEANUP_RETRY_DELAY_MS);
        } else {
            self.pending = false;
        }
    }

    pub fn pending(&self) -> bool {
        self.pending
    }

    pub fn trigger(&self) -> CleanupTrigger {
        self.trigger
    }
}

fn ceiling_fraction(value: u64, numerator: u64, denominator: u64) -> u64 {
    let quotient = value / denominator;
    let remainder = value % denominator;
    quotient * numerator + (remainder * numerator).div_ceil(denominator)
}

fn eligible_for_deletion(session: &CleanupSession) -> bool {
    session.inactive && session.all_points_confirmed && session.diagnostic_log_confirmed
}

pub fn storage_cleanup_required(capacity_bytes: u64, used_bytes: u64) -> bool {
    capacity_bytes != 0 && used_bytes >= ceiling_fraction(capacity_bytes, 4, 5)
}

pub fn storage_cleanup_target_reached(capacity_bytes: u64, used_bytes: u64) -> bool {
    capacity_bytes != 0 && used_bytes < ceiling_fraction(capacity_bytes, 7, 10)
}

pub fn plan_storage_cleanup(
    capacity_bytes: u64,
    mut used_bytes: u64,
    sessions: &[CleanupSession],
) -> CleanupPlan {
    if !storage_cleanup_required(capacity_bytes, used_bytes) {
        return CleanupPlan::default();
    }

    let mut plan = CleanupPlan {
        cleanup_required: true,
        ..Default::default()
    };
    let target = ceiling_fraction(capacity_bytes, 7, 10);
    let mut oldest = sessions.to_vec();
    oldest.sort_by_key(|session| session.tracking_session_number);
    for session in &oldest {
        if used_bytes < target {
            break;
        }
        if !eligible_for_deletion(session) {
            if session.bytes > 0 {
                plan.protected_data_remains = true;
            }
            continue;
        }
        plan.sessions_to_delete
            .push(session.tracking_session_number);
        used_bytes = used_bytes.saturating_sub(session.bytes);
    }
    plan.target_reached = storage_cleanup_target_reached(capacity_bytes, used_bytes);
    plan
}

fn remember_failure(run: &mut CleanupRun, status: CleanupStorageStatus) {
    if run.failure == CleanupStorageStatus::Ok {
        run.failure = status;
    }
}

fn persist(
    storage: &mut impl StorageCleanupStorage,
    message: &str,
    run: &mut CleanupRun,
) -> Result<(), CleanupStorageStatus> {
    storage
        .persist_cleanup_diagnostic(message)
        .inspect_err(|status| {
            remember_failure(run, *status);
            run.outcome = CleanupOutcome::RetryRequired;
        })
}

fn refresh_usage(
    storage: &mut impl StorageCleanupStorage,
    run: &mut CleanupRun,
) -> Option<(u64, u64)> {
    match storage.read_usage() {
        Ok(usage) => Some(usage),
        Err(status) => {
            run.outcome = CleanupOutcome::RetryRequired;
            remember_failure(run, status);
            None
        }
    }
}

pub fn run_storage_cleanup(storage: &mut impl StorageCleanupStorage) -> CleanupRun {
    let mut run = CleanupRun::default();
    if let Err(status) = storage.flush_pending_cleanup_diagnostic() {
        run.outcome = CleanupOutcome::RetryRequired;
        run.failure = status;
        return run;
    }
    let Some((mut capacity_bytes, mut used_bytes)) = refresh_usage(storage, &mut run) else {
        return run;
    };
    if !storage_cleanup_required(capacity_bytes, used_bytes) {
        return run;
    }

    let mut sessions = match storage.read_cleanup_sessions() {
        Ok(sessions) => sessions,
        Err(status) => {
            run.outcome = CleanupOutcome::RetryRequired;
            run.failure = status;
            let _ = persist(storage, "[ERROR] cleanup session scan failed\n", &mut run);
            return run;
        }
    };
    let plan = plan_storage_cleanup(capacity_bytes, used_bytes, &sessions);
    sessions.sort_by_key(|session| session.tracking_session_number);
    let mut deletion_failed = false;

    for session in sessions
        .iter()
        .filter(|session| eligible_for_deletion(session))
    {
        match storage.delete_session_files(session.tracking_session_number) {
            Ok(()) => {
                storage.forget_deleted_session(session.tracking_session_number);
                run.deleted_sessions.push(session.tracking_session_number);
                let message = format!(
                    "[CLEANUP] deleted delivered session={}\n",
                    session.tracking_session_number
                );
                if persist(storage, &message, &mut run).is_err() {
                    return run;
                }
            }
            Err(status) => {
                deletion_failed = true;
                remember_failure(&mut run, status);
                let message = format!(
                    "[ERROR] cleanup delete failed session={}\n",
                    session.tracking_session_number
                );
                if persist(storage, &message, &mut run).is_err() {
                    return run;
                }
            }
        }

        let Some(usage) = refresh_usage(storage, &mut run) else {
            return run;
        };
        (capacity_bytes, used_bytes) = usage;
        if storage_cleanup_target_reached(capacity_bytes, used_bytes)
            && run.failure != CleanupStorageStatus::DiagnosticWriteFailed
        {
            run.outcome = CleanupOutcome::TargetReached;
            return run;
        }
    }

    run.outcome = if deletion_failed {
        CleanupOutcome::RetryRequired
    } else {
        CleanupOutcome::WaitingForDelivery
    };
    if !plan.protected_data_remains {
        run.outcome = CleanupOutcome::RetryRequired;
    }
    let message = if plan.protected_data_remains {
        "[CLEANUP] unable to reach below 70 percent; pending data protected\n"
    } else {
        "[ERROR] cleanup unable to reach below 70 percent\n"
    };
    if persist(storage, message, &mut run).is_err() {
        return run;
    }
    let Some((capacity_bytes, used_bytes)) = refresh_usage(storage, &mut run) else {
        return run;
    };
    if storage_cleanup_target_reached(capacity_bytes, used_bytes)
        && run.failure != CleanupStorageStatus::DiagnosticWriteFailed
    {
        run.outcome = CleanupOutcome::TargetReached;
    } else if run.failure == CleanupStorageStatus::DiagnosticWriteFailed {
        run.outcome = CleanupOutcome::RetryRequired;
    }
    run
}

pub fn cleanup_storage_status_name(status: CleanupStorageStatus) -> &'static str {
    match status {
        CleanupStorageStatus::Ok => "none",
        CleanupStorageStatus::LockUnavailable => "SD lock unavailable",
        CleanupStorageStatus::MetricUnavailable => "SD metrics unavailable",
        CleanupStorageStatus::ScanFailed => "session scan failed",
        CleanupStorageStatus::DeleteFailed => "session deletion failed",
        CleanupStorageStatus::DiagnosticWriteFailed => "diagnostic persistence failed",
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn delivered(number: u32, bytes: u64) -> CleanupSession {
        CleanupSession {
            tracking_session_number: number,
            bytes,
            inactive: true,
            all_points_confirmed: true,
            diagnostic_log_confirmed: true,
        }
    }

    #[derive(Default)]
    struct MemoryStorage {
        capacity: u64,
        used: u64,
        sessions: Vec<CleanupSession>,
        deleted: Vec<u32>,
        forgotten: Vec<u32>,
        diagnostics: String,
        diagnostic_bytes: u64,
        deletion_to_fail: Option<u32>,
        usage_failure: Option<CleanupStorageStatus>,
        diagnostic_failure: Option<CleanupStorageStatus>,
        pending_diagnostic: String,
    }

    impl StorageCleanupStorage for MemoryStorage {
        fn read_usage(&mut self) -> Result<(u64, u64), CleanupStorageStatus> {
            self.usage_failure
                .map_or(Ok((self.capacity, self.used)), Err)
        }

        fn read_cleanup_sessions(&mut self) -> Result<Vec<CleanupSession>, CleanupStorageStatus> {
            Ok(self.sessions.clone())
        }

        fn delete_session_files(&mut self, number: u32) -> Result<(), CleanupStorageStatus> {
            self.deleted.push(number);
            if self.deletion_to_fail == Some(number) {
                return Err(CleanupStorageStatus::DeleteFailed);
            }
            if let Some(session) = self
                .sessions
                .iter()
                .find(|session| session.tracking_session_number == number)
            {
                self.used = self.used.saturating_sub(session.bytes);
            }
            Ok(())
        }

        fn forget_deleted_session(&mut self, number: u32) {
            self.forgotten.push(number);
        }

        fn flush_pending_cleanup_diagnostic(&mut self) -> Result<(), CleanupStorageStatus> {
            if self.pending_diagnostic.is_empty() {
                return Ok(());
            }
            if let Some(status) = self.diagnostic_failure {
                return Err(status);
            }
            self.diagnostics.push_str(&self.pending_diagnostic);
            self.pending_diagnostic.clear();
            self.used += self.diagnostic_bytes;
            Ok(())
        }

        fn persist_cleanup_diagnostic(
            &mut self,
            message: &str,
        ) -> Result<(), CleanupStorageStatus> {
            if let Some(status) = self.diagnostic_failure {
                self.pending_diagnostic = message.into();
                return Err(status);
            }
            self.diagnostics.push_str(message);
            self.used += self.diagnostic_bytes;
            Ok(())
        }
    }

    #[test]
    fn plan_starts_at_eighty_and_deletes_oldest_until_below_seventy() {
        assert!(!plan_storage_cleanup(100, 79, &[delivered(41, 20)]).cleanup_required);
        let plan = plan_storage_cleanup(
            100,
            90,
            &[delivered(44, 11), delivered(41, 10), delivered(43, 10)],
        );
        assert_eq!(plan.sessions_to_delete, [41, 43, 44]);
        assert!(plan.target_reached);
    }

    #[test]
    fn pending_data_is_protected() {
        let mut pending = delivered(41, 30);
        pending.all_points_confirmed = false;
        let plan = plan_storage_cleanup(100, 90, &[pending, delivered(42, 10)]);
        assert_eq!(plan.sessions_to_delete, [42]);
        assert!(plan.protected_data_remains && !plan.target_reached);
    }

    #[test]
    fn run_rechecks_usage_after_diagnostic_writes() {
        let mut storage = MemoryStorage {
            capacity: 100,
            used: 90,
            sessions: vec![delivered(41, 21), delivered(42, 10)],
            diagnostic_bytes: 2,
            ..Default::default()
        };
        let run = run_storage_cleanup(&mut storage);
        assert_eq!(run.outcome, CleanupOutcome::TargetReached);
        assert_eq!(storage.deleted, [41, 42]);
        assert_eq!(storage.forgotten, [41, 42]);
        assert!(storage.used < 70);
    }

    #[test]
    fn partial_delete_failure_is_visible_but_other_sessions_continue() {
        let mut storage = MemoryStorage {
            capacity: 100,
            used: 90,
            sessions: vec![delivered(41, 10), delivered(42, 25)],
            deletion_to_fail: Some(41),
            ..Default::default()
        };
        let run = run_storage_cleanup(&mut storage);
        assert_eq!(run.outcome, CleanupOutcome::TargetReached);
        assert_eq!(run.failure, CleanupStorageStatus::DeleteFailed);
        assert_eq!(storage.forgotten, [42]);
        assert!(storage.diagnostics.contains("delete failed session=41"));
    }

    #[test]
    fn retry_schedule_handles_delay_and_clock_wrap() {
        let mut schedule = StorageCleanupSchedule::default();
        schedule.request(CleanupTrigger::Startup);
        schedule.record_run(
            &CleanupRun {
                outcome: CleanupOutcome::RetryRequired,
                ..Default::default()
            },
            u32::MAX - 29_999,
        );
        assert!(!schedule.ready(29_999));
        assert!(schedule.ready(30_000));
    }
}
