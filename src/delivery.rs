use crate::tracking::track_point_identity;
use serde::Deserialize;
use std::io::{self, Write};

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct RecoveredTrackingSession {
    pub tracking_session_number: u32,
    pub highest_recorded_point: u32,
    pub highest_confirmed_point: u32,
    pub inactive: bool,
    pub recovery_required: bool,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct StoredTrackingSession {
    pub tracking_session_number: u32,
    pub highest_recorded_point: u32,
    pub delivery_state: String,
    pub delivery_state_readable: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RecoveryReadStatus {
    Readable,
    Missing,
    Unreadable,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RecoveryDirectoryEntry {
    pub name: String,
    pub directory: bool,
}

pub trait DeliveryRecoveryStorage {
    fn list_root(&mut self) -> Option<Vec<RecoveryDirectoryEntry>>;
    fn count_complete_lines(&mut self, path: &str) -> (RecoveryReadStatus, u32);
    fn read_text(&mut self, path: &str) -> (RecoveryReadStatus, String);
    fn append_text(&mut self, path: &str, contents: &str) -> bool;
}

#[derive(Default)]
pub struct DeliveryRecovery {
    ready: bool,
    max_stored_tracking_session_number: u32,
    sessions: Vec<RecoveredTrackingSession>,
}

impl DeliveryRecovery {
    pub fn restore(&mut self, storage: &mut impl DeliveryRecoveryStorage) -> bool {
        self.ready = false;
        let Some(entries) = storage.list_root() else {
            return false;
        };
        let mut stored = Vec::new();
        let mut maximum = 0;
        for entry in entries {
            let Some(number) = entry
                .directory
                .then(|| parse_session_directory_name(&entry.name))
                .flatten()
            else {
                continue;
            };
            let (points_status, highest_recorded_point) =
                storage.count_complete_lines(&session_file_path(number, "track-points.ndjson"));
            if points_status != RecoveryReadStatus::Readable {
                return false;
            }
            let (state_status, delivery_state) =
                storage.read_text(&session_file_path(number, "delivery-state.log"));
            stored.push(StoredTrackingSession {
                tracking_session_number: number,
                highest_recorded_point,
                delivery_state,
                delivery_state_readable: state_status == RecoveryReadStatus::Readable,
            });
            maximum = maximum.max(number);
        }
        self.sessions = recover_tracking_sessions(&stored);
        self.max_stored_tracking_session_number = maximum;
        self.ready = true;
        true
    }

    pub fn ready(&self) -> bool {
        self.ready
    }
    pub fn max_stored_tracking_session_number(&self) -> u32 {
        self.max_stored_tracking_session_number
    }
    pub fn sessions(&self) -> &[RecoveredTrackingSession] {
        &self.sessions
    }
    pub fn sessions_mut(&mut self) -> &mut Vec<RecoveredTrackingSession> {
        &mut self.sessions
    }

    pub fn oldest_pending_session(&self) -> Option<&RecoveredTrackingSession> {
        self.sessions.iter().find(|session| {
            session.inactive && session.highest_confirmed_point < session.highest_recorded_point
        })
    }

    pub fn begin_session(&mut self, number: u32) -> bool {
        if !self.ready || number == 0 || number <= self.max_stored_tracking_session_number {
            return false;
        }
        self.sessions.push(RecoveredTrackingSession {
            tracking_session_number: number,
            inactive: false,
            ..Default::default()
        });
        self.max_stored_tracking_session_number = number;
        true
    }

    pub fn record_point(&mut self, number: u32) -> bool {
        let Some(session) = self.sessions.iter_mut().find(|session| {
            session.tracking_session_number == number
                && !session.inactive
                && session.highest_recorded_point != u32::MAX
        }) else {
            return false;
        };
        session.highest_recorded_point += 1;
        true
    }

    pub fn forget_session(&mut self, number: u32) {
        self.sessions
            .retain(|session| session.tracking_session_number != number);
    }

    pub fn confirm_delivery_through(
        &mut self,
        storage: &mut impl DeliveryRecoveryStorage,
        number: u32,
        highest: u32,
    ) -> bool {
        let Some(index) = self
            .sessions
            .iter()
            .position(|session| session.tracking_session_number == number)
        else {
            return false;
        };
        let session = &self.sessions[index];
        if highest < session.highest_confirmed_point || highest > session.highest_recorded_point {
            return false;
        }
        if !storage.append_text(
            &session_file_path(number, "delivery-state.log"),
            &serialize_delivery_progress(number, highest),
        ) {
            return false;
        }
        self.sessions[index].highest_confirmed_point = highest;
        self.sessions[index].recovery_required = false;
        true
    }
}

fn checksum(session: u32, confirmed: u32) -> u32 {
    let mut value = 2_166_136_261u32;
    for field in [1u32, session, confirmed] {
        for byte in field.to_le_bytes() {
            value ^= u32::from(byte);
            value = value.wrapping_mul(16_777_619);
        }
    }
    value
}

fn parse_progress(line: &str, expected_session: u32) -> Option<u32> {
    let mut fields = line.split(',');
    let version = fields.next()?.parse::<u32>().ok()?;
    let session = fields.next()?.parse::<u32>().ok()?;
    let confirmed = fields.next()?.parse::<u32>().ok()?;
    let stored_checksum = u32::from_str_radix(fields.next()?, 16).ok()?;
    if fields.next().is_some()
        || version != 1
        || session != expected_session
        || stored_checksum != checksum(session, confirmed)
    {
        return None;
    }
    Some(confirmed)
}

fn parse_session_directory_name(path: &str) -> Option<u32> {
    let name = path.rsplit(['/', '\\']).next()?;
    let digits = name.strip_prefix("session-")?;
    (!digits.is_empty() && digits.bytes().all(|byte| byte.is_ascii_digit()))
        .then(|| digits.parse::<u32>().ok())
        .flatten()
        .filter(|number| *number != 0)
}

pub fn session_directory_path(number: u32) -> String {
    format!("/session-{number:010}")
}

pub fn session_file_path(number: u32, filename: &str) -> String {
    format!("{}/{filename}", session_directory_path(number))
}

pub fn serialize_delivery_progress(session: u32, confirmed: u32) -> String {
    format!(
        "1,{session},{confirmed},{:08x}\n",
        checksum(session, confirmed)
    )
}

pub fn next_tracking_session_number(persisted: u32, stored: u32) -> Option<u32> {
    persisted.max(stored).checked_add(1)
}

pub fn recover_tracking_sessions(
    stored: &[StoredTrackingSession],
) -> Vec<RecoveredTrackingSession> {
    let mut recovered = Vec::with_capacity(stored.len());
    for stored in stored {
        let mut session = RecoveredTrackingSession {
            tracking_session_number: stored.tracking_session_number,
            highest_recorded_point: stored.highest_recorded_point,
            inactive: true,
            recovery_required: !stored.delivery_state_readable
                || (stored.highest_recorded_point > 0 && stored.delivery_state.is_empty()),
            ..Default::default()
        };
        for line in stored.delivery_state.lines() {
            match parse_progress(line, stored.tracking_session_number) {
                Some(confirmed) if confirmed <= stored.highest_recorded_point => {
                    if confirmed < session.highest_confirmed_point {
                        session.recovery_required = true;
                    }
                    session.highest_confirmed_point = confirmed;
                }
                _ => session.recovery_required = true,
            }
        }
        recovered.push(session);
    }
    recovered.sort_by_key(|session| session.tracking_session_number);
    recovered
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct PendingDeliveryBatch {
    pub tracking_session_number: u32,
    pub first_point_number: u32,
    pub point_count: u32,
}

pub fn select_oldest_pending_batch(
    sessions: &[RecoveredTrackingSession],
) -> Option<PendingDeliveryBatch> {
    let selected = sessions
        .iter()
        .filter(|session| session.highest_confirmed_point < session.highest_recorded_point)
        .filter(|session| {
            session.inactive
                || session.highest_recorded_point - session.highest_confirmed_point >= 30
        })
        .min_by_key(|session| session.tracking_session_number)?;
    let pending = selected.highest_recorded_point - selected.highest_confirmed_point;
    Some(PendingDeliveryBatch {
        tracking_session_number: selected.tracking_session_number,
        first_point_number: selected.highest_confirmed_point + 1,
        point_count: pending.min(30),
    })
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct DeliveryRetrySchedule {
    consecutive_failures: u32,
    next_attempt_at_ms: u32,
}

impl DeliveryRetrySchedule {
    pub fn record_failure(&mut self) -> u32 {
        let delays = [15, 30, 60, 120, 300];
        let delay = delays[(self.consecutive_failures as usize).min(delays.len() - 1)];
        self.consecutive_failures = self.consecutive_failures.saturating_add(1);
        delay
    }
    pub fn record_success(&mut self) {
        self.consecutive_failures = 0;
    }
    pub fn ready(&self, now_ms: u32) -> bool {
        now_ms.wrapping_sub(self.next_attempt_at_ms) as i32 >= 0
    }
    pub fn schedule_failure(&mut self, now_ms: u32) -> u32 {
        let seconds = self.record_failure();
        self.next_attempt_at_ms = now_ms.wrapping_add(seconds * 1000);
        seconds
    }
    pub fn schedule_success(&mut self, now_ms: u32) {
        self.record_success();
        self.next_attempt_at_ms = now_ms;
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BatchReadStatus {
    Ready,
    NoPendingBatch,
    LockUnavailable,
    StorageOpenFailed,
    MalformedInput,
}

pub trait UploadPointSource {
    fn read_point_lines(
        &mut self,
        session: u32,
        first: u32,
        count: usize,
    ) -> (BatchReadStatus, Vec<String>);
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct UploadBatch {
    pub schema_version: u32,
    pub tracker_id: String,
    pub tracking_session_number: u32,
    pub first_point_number: u32,
    pub ndjson_points: Vec<String>,
}

impl Default for UploadBatch {
    fn default() -> Self {
        Self {
            schema_version: 1,
            tracker_id: String::new(),
            tracking_session_number: 0,
            first_point_number: 0,
            ndjson_points: Vec::new(),
        }
    }
}

pub fn read_pending_batch(
    sessions: &[RecoveredTrackingSession],
    source: &mut impl UploadPointSource,
    tracker_id: &str,
) -> Result<UploadBatch, BatchReadStatus> {
    if sessions
        .iter()
        .any(|session| session.highest_confirmed_point > session.highest_recorded_point)
    {
        return Err(BatchReadStatus::MalformedInput);
    }
    let pending = select_oldest_pending_batch(sessions).ok_or(BatchReadStatus::NoPendingBatch)?;
    let (status, lines) = source.read_point_lines(
        pending.tracking_session_number,
        pending.first_point_number,
        pending.point_count as usize,
    );
    if status != BatchReadStatus::Ready {
        return Err(status);
    }
    if lines.len() != pending.point_count as usize || lines.iter().any(String::is_empty) {
        return Err(BatchReadStatus::MalformedInput);
    }
    Ok(UploadBatch {
        schema_version: 1,
        tracker_id: tracker_id.into(),
        tracking_session_number: pending.tracking_session_number,
        first_point_number: pending.first_point_number,
        ndjson_points: lines,
    })
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct HttpRequest {
    pub url: String,
    pub headers: Vec<(String, String)>,
    pub body: String,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct UploadConfirmation {
    pub highest_stored_point_number: u32,
}

fn has_path(url: &str, required: &str) -> bool {
    url.strip_prefix("https://")
        .and_then(|rest| rest.find('/').map(|index| &rest[index..]))
        == Some(required)
}

pub fn build_upload_request(url: &str, token: &str, batch: &UploadBatch) -> Option<HttpRequest> {
    if !has_path(url, "/v1/track-point-batches")
        || token.is_empty()
        || batch.tracker_id.is_empty()
        || batch.tracking_session_number == 0
        || batch.first_point_number == 0
        || batch.ndjson_points.is_empty()
        || batch.ndjson_points.len() > 30
    {
        return None;
    }

    for (index, point) in batch.ndjson_points.iter().enumerate() {
        let expected = batch.first_point_number.checked_add(index as u32)?;
        let identity = track_point_identity(point)?;
        if identity
            != (
                batch.schema_version,
                batch.tracker_id.clone(),
                batch.tracking_session_number,
                expected,
            )
        {
            return None;
        }
    }
    let last = batch
        .first_point_number
        .checked_add(batch.ndjson_points.len() as u32 - 1)?;
    Some(HttpRequest {
        url: url.into(),
        headers: vec![
            ("Authorization".into(), format!("Bearer {token}")),
            ("Content-Type".into(), "application/x-ndjson".into()),
            (
                "X-Track-Point-Schema-Version".into(),
                batch.schema_version.to_string(),
            ),
            ("X-Tracker-ID".into(), batch.tracker_id.clone()),
            (
                "X-Tracking-Session-Number".into(),
                batch.tracking_session_number.to_string(),
            ),
            (
                "X-First-Point-Number".into(),
                batch.first_point_number.to_string(),
            ),
            ("X-Last-Point-Number".into(), last.to_string()),
        ],
        body: format!("{}\n", batch.ndjson_points.join("\n")),
    })
}

pub fn validate_upload_response(
    status: u16,
    body: &str,
    batch: &UploadBatch,
) -> Option<UploadConfirmation> {
    if !(200..300).contains(&status) || batch.ndjson_points.is_empty() {
        return None;
    }
    #[derive(Deserialize)]
    struct Response {
        tracker_id: String,
        tracking_session_number: u32,
        highest_stored_point_number: u32,
    }
    let response: Response = serde_json::from_str(body).ok()?;
    let required = batch
        .first_point_number
        .checked_add(batch.ndjson_points.len() as u32 - 1)?;
    (response.tracker_id == batch.tracker_id
        && response.tracking_session_number == batch.tracking_session_number
        && response.highest_stored_point_number >= required)
        .then_some(UploadConfirmation {
            highest_stored_point_number: response.highest_stored_point_number,
        })
}

pub trait RawPointLogStorage {
    fn append(&mut self, bytes: &[u8]) -> usize;
    fn flush(&mut self);
    fn abandon(&mut self);
}

pub fn append_complete_raw_point(storage: &mut impl RawPointLogStorage, ndjson: &str) -> bool {
    if storage.append(ndjson.as_bytes()) != ndjson.len() {
        storage.flush();
        storage.abandon();
        return false;
    }
    let complete = storage.append(b"\n") == 1;
    storage.flush();
    if !complete {
        storage.abandon();
    }
    complete
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DiagnosticWriteStatus {
    Persisted,
    Retained,
    Full,
}

pub trait DiagnosticLogStorage {
    fn append_diagnostic_bytes(&mut self, bytes: &[u8]) -> usize;
}

pub fn write_synced_bytes<W>(
    writer: &mut W,
    bytes: &[u8],
    sync: impl FnOnce(&mut W) -> io::Result<()>,
) -> usize
where
    W: Write,
{
    let mut written = 0;
    while written < bytes.len() {
        match writer.write(&bytes[written..]) {
            Ok(0) | Err(_) => break,
            Ok(count) => written += count,
        }
    }
    if sync(writer).is_ok() {
        written
    } else {
        // ponytail: retry may duplicate unsynced bytes; track offsets if exact log replay is needed.
        0
    }
}

pub struct DiagnosticLog {
    capacity: usize,
    pending: Vec<u8>,
}

impl DiagnosticLog {
    pub fn new(capacity: usize) -> Self {
        Self {
            capacity,
            pending: Vec::new(),
        }
    }
    pub fn pending(&self) -> &[u8] {
        &self.pending
    }
    fn retain(&mut self, text: &str) -> DiagnosticWriteStatus {
        let available = self.capacity.saturating_sub(self.pending.len());
        let cutoff = available.min(text.len());
        self.pending.extend_from_slice(&text.as_bytes()[..cutoff]);
        if text.len() <= available {
            DiagnosticWriteStatus::Retained
        } else {
            DiagnosticWriteStatus::Full
        }
    }
    pub fn flush(&mut self, storage: &mut impl DiagnosticLogStorage) -> DiagnosticWriteStatus {
        if self.pending.is_empty() {
            return DiagnosticWriteStatus::Persisted;
        }
        let written = storage
            .append_diagnostic_bytes(&self.pending)
            .min(self.pending.len());
        self.pending.drain(..written);
        if self.pending.is_empty() {
            DiagnosticWriteStatus::Persisted
        } else {
            DiagnosticWriteStatus::Retained
        }
    }
    pub fn append(
        &mut self,
        storage: Option<&mut impl DiagnosticLogStorage>,
        text: &str,
    ) -> DiagnosticWriteStatus {
        if let Some(storage) = storage {
            if self.flush(storage) == DiagnosticWriteStatus::Persisted {
                let written = storage
                    .append_diagnostic_bytes(text.as_bytes())
                    .min(text.len());
                if written == text.len() {
                    return DiagnosticWriteStatus::Persisted;
                }
                let suffix = &text.as_bytes()[written..];
                let available = self.capacity.saturating_sub(self.pending.len());
                let cutoff = available.min(suffix.len());
                self.pending.extend_from_slice(&suffix[..cutoff]);
                return if suffix.len() <= available {
                    DiagnosticWriteStatus::Retained
                } else {
                    DiagnosticWriteStatus::Full
                };
            }
        }
        self.retain(text)
    }
}

impl Default for DiagnosticLog {
    fn default() -> Self {
        Self::new(2048)
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct DiagnosticLogUpload {
    pub tracker_id: String,
    pub tracking_session_number: u32,
    pub contents: String,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct StoredDiagnosticLog {
    pub tracking_session_number: u32,
    pub contents: String,
    pub delivery_state: String,
}

pub fn build_diagnostic_upload_request(
    url: &str,
    token: &str,
    upload: &DiagnosticLogUpload,
) -> Option<HttpRequest> {
    if !has_path(url, "/v1/diagnostic-logs")
        || token.is_empty()
        || upload.tracker_id.is_empty()
        || upload.tracking_session_number == 0
        || upload.contents.is_empty()
    {
        return None;
    }
    Some(HttpRequest {
        url: url.into(),
        headers: vec![
            ("Authorization".into(), format!("Bearer {token}")),
            ("Content-Type".into(), "text/plain; charset=utf-8".into()),
            ("X-Tracker-ID".into(), upload.tracker_id.clone()),
            (
                "X-Tracking-Session-Number".into(),
                upload.tracking_session_number.to_string(),
            ),
            (
                "Idempotency-Key".into(),
                format!("{}:{}", upload.tracker_id, upload.tracking_session_number),
            ),
        ],
        body: upload.contents.clone(),
    })
}

pub fn validate_diagnostic_upload_response(
    status: u16,
    body: &str,
    upload: &DiagnosticLogUpload,
) -> bool {
    if !(200..300).contains(&status) {
        return false;
    }
    #[derive(Deserialize)]
    #[serde(deny_unknown_fields)]
    struct Response {
        tracker_id: String,
        tracking_session_number: u32,
        diagnostic_log_stored: bool,
    }
    let Ok(response) = serde_json::from_str::<Response>(body) else {
        return false;
    };
    response.tracker_id == upload.tracker_id
        && response.tracking_session_number == upload.tracking_session_number
        && response.diagnostic_log_stored
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DiagnosticUploadFailure {
    Wifi,
    Connection,
    Authentication,
    Http,
    Tls,
    Transport,
    MalformedResponse,
    Timeout,
}

pub fn diagnostic_upload_failure_message(failure: DiagnosticUploadFailure) -> &'static str {
    match failure {
        DiagnosticUploadFailure::Wifi => "Wi-Fi connection failed",
        DiagnosticUploadFailure::Connection => "DNS or connection failed",
        DiagnosticUploadFailure::Authentication => "authentication rejected",
        DiagnosticUploadFailure::Http => "HTTP request rejected",
        DiagnosticUploadFailure::Tls => "TLS setup or certificate validation failed",
        DiagnosticUploadFailure::Transport => "HTTP transport failed",
        DiagnosticUploadFailure::MalformedResponse => "malformed or mismatched confirmation",
        DiagnosticUploadFailure::Timeout => "network timeout",
    }
}

pub fn classify_http_failure(status: i32, tls_failure: bool) -> DiagnosticUploadFailure {
    if status < 0 && tls_failure {
        DiagnosticUploadFailure::Tls
    } else if matches!(status, 401 | 403) {
        DiagnosticUploadFailure::Authentication
    } else if (400..600).contains(&status) {
        DiagnosticUploadFailure::Http
    } else if matches!(status, -11 | -12) {
        DiagnosticUploadFailure::Timeout
    } else if matches!(status, -1 | -4 | -5 | -7) {
        DiagnosticUploadFailure::Connection
    } else if status < 0 {
        DiagnosticUploadFailure::Transport
    } else {
        DiagnosticUploadFailure::MalformedResponse
    }
}

pub fn serialize_diagnostic_delivery(tracker_id: &str, session: u32) -> String {
    format!("v1 tracker={tracker_id} session={session} delivered\n")
}

pub fn select_oldest_pending_diagnostic_log(
    tracker_id: &str,
    logs: &[StoredDiagnosticLog],
) -> Option<DiagnosticLogUpload> {
    let log = logs
        .iter()
        .filter(|log| log.tracking_session_number != 0 && !log.contents.is_empty())
        .filter(|log| {
            !log.delivery_state.contains(&serialize_diagnostic_delivery(
                tracker_id,
                log.tracking_session_number,
            ))
        })
        .min_by_key(|log| log.tracking_session_number)?;
    Some(DiagnosticLogUpload {
        tracker_id: tracker_id.into(),
        tracking_session_number: log.tracking_session_number,
        contents: log.contents.clone(),
    })
}

pub fn confirm_diagnostic_delivery(
    storage: &mut impl DeliveryRecoveryStorage,
    tracker_id: &str,
    session: u32,
    logs: &mut [StoredDiagnosticLog],
) -> bool {
    let Some(log) = logs
        .iter_mut()
        .find(|log| log.tracking_session_number == session)
    else {
        return false;
    };
    let record = serialize_diagnostic_delivery(tracker_id, session);
    if !storage.append_text(
        &session_file_path(session, "diagnostic-delivery-state.log"),
        &record,
    ) {
        return false;
    }
    log.delivery_state.push_str(&record);
    true
}

#[cfg(test)]
mod tests {
    use super::*;

    fn point(number: u32) -> String {
        format!(
            r#"{{"schema_version":1,"tracker_id":"tracker-01","tracking_session_number":41,"point_number":{number}}}"#
        )
    }

    #[test]
    fn progress_checksum_recovers_last_safe_point() {
        let good = serialize_delivery_progress(41, 30);
        let recovered = recover_tracking_sessions(&[StoredTrackingSession {
            tracking_session_number: 41,
            highest_recorded_point: 60,
            delivery_state: format!("{good}damaged\n"),
            delivery_state_readable: true,
        }]);
        assert_eq!(recovered[0].highest_confirmed_point, 30);
        assert!(recovered[0].recovery_required);
    }

    #[test]
    fn active_sessions_expose_only_full_batches() {
        let mut session = RecoveredTrackingSession {
            tracking_session_number: 1,
            highest_recorded_point: 29,
            inactive: false,
            ..Default::default()
        };
        assert!(select_oldest_pending_batch(&[session.clone()]).is_none());
        session.highest_recorded_point = 30;
        assert_eq!(
            select_oldest_pending_batch(&[session]).unwrap().point_count,
            30
        );
    }

    #[test]
    fn upload_contract_checks_identity_range_and_confirmation() {
        let batch = UploadBatch {
            schema_version: 1,
            tracker_id: "tracker-01".into(),
            tracking_session_number: 41,
            first_point_number: 1,
            ndjson_points: (1..=30).map(point).collect(),
        };
        let request = build_upload_request(
            "https://uploads.example/v1/track-point-batches",
            "secret",
            &batch,
        )
        .unwrap();
        assert_eq!(request.body.lines().count(), 30);
        assert_eq!(validate_upload_response(200, r#"{"tracker_id":"tracker-01","tracking_session_number":41,"highest_stored_point_number":30}"#, &batch).unwrap().highest_stored_point_number, 30);
        assert!(validate_upload_response(200, r#"{"tracker_id":"other","tracking_session_number":41,"highest_stored_point_number":30}"#, &batch).is_none());
    }

    #[test]
    fn retry_delays_cap_and_reset() {
        let mut retry = DeliveryRetrySchedule::default();
        assert_eq!(
            (0..6).map(|_| retry.record_failure()).collect::<Vec<_>>(),
            [15, 30, 60, 120, 300, 300]
        );
        retry.record_success();
        assert_eq!(retry.record_failure(), 15);
    }

    struct ShortWriter {
        limit: usize,
        abandoned: bool,
        bytes: Vec<u8>,
    }
    impl RawPointLogStorage for ShortWriter {
        fn append(&mut self, bytes: &[u8]) -> usize {
            let count = bytes.len().min(self.limit);
            self.bytes.extend_from_slice(&bytes[..count]);
            count
        }
        fn flush(&mut self) {}
        fn abandon(&mut self) {
            self.abandoned = true;
        }
    }

    #[test]
    fn short_raw_point_body_is_not_newline_terminated() {
        let mut writer = ShortWriter {
            limit: 2,
            abandoned: false,
            bytes: vec![],
        };
        assert!(!append_complete_raw_point(&mut writer, "point"));
        assert_eq!(writer.bytes, b"po");
        assert!(writer.abandoned);
    }

    #[test]
    fn diagnostic_confirmation_requires_exact_matching_shape() {
        let upload = DiagnosticLogUpload {
            tracker_id: "tracker-01".into(),
            tracking_session_number: 41,
            contents: "log".into(),
        };
        assert!(validate_diagnostic_upload_response(
            200,
            r#"{"tracker_id":"tracker-01","tracking_session_number":41,"diagnostic_log_stored":true}"#,
            &upload
        ));
        assert!(!validate_diagnostic_upload_response(
            200,
            r#"{"tracker_id":"tracker-01","tracking_session_number":41,"diagnostic_log_stored":true,"extra":1}"#,
            &upload
        ));
    }
}
