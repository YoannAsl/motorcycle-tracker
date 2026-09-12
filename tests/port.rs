use motorcycle_tracker::{delivery::*, tracking::*};
use serde_json::Value;

#[derive(Default)]
struct TrackStore {
    start_ok: bool,
    write_ok: bool,
    lines: Vec<String>,
}

impl TrackingStorage for TrackStore {
    fn start_tracking_session(&mut self) -> Option<u32> {
        self.start_ok.then_some(41)
    }

    fn append_and_flush_raw_point(&mut self, _: &TrackPoint, ndjson: &str) -> bool {
        if self.write_ok {
            self.lines.push(ndjson.to_owned());
        }
        self.write_ok
    }
}

fn moving_fix() -> GpsFix {
    GpsFix {
        location_valid: true,
        location_fresh: true,
        latitude: 48.856_613,
        longitude: 2.352_222,
        utc: Some("2026-08-30T12:00:00Z".into()),
        speed_kmh: Some(12.5),
        hdop: Some(0.9),
        ..Default::default()
    }
}

fn recovered(
    number: u32,
    recorded: u32,
    confirmed: u32,
    inactive: bool,
) -> RecoveredTrackingSession {
    RecoveredTrackingSession {
        tracking_session_number: number,
        highest_recorded_point: recorded,
        highest_confirmed_point: confirmed,
        inactive,
        ..Default::default()
    }
}

fn point(number: u32) -> String {
    format!(
        r#"{{"schema_version":1,"tracker_id":"tracker-01","tracking_session_number":41,"point_number":{number}}}"#
    )
}

fn batch() -> UploadBatch {
    UploadBatch {
        schema_version: 1,
        tracker_id: "tracker-01".into(),
        tracking_session_number: 41,
        first_point_number: 1,
        ndjson_points: (1..=30).map(point).collect(),
    }
}

fn header<'a>(request: &'a HttpRequest, name: &str) -> Option<&'a str> {
    request
        .headers
        .iter()
        .find(|(key, _)| key == name)
        .map(|(_, value)| value.as_str())
}

#[test]
fn tracking_starts_on_third_good_fix_and_retries_failed_point_one() {
    let mut workflow = TrackingWorkflow::new("tracker-01");
    let mut storage = TrackStore {
        start_ok: true,
        write_ok: true,
        ..Default::default()
    };
    let mut stationary = moving_fix();
    stationary.speed_kmh = Some(0.0);
    assert!(
        !workflow
            .process_fix(&stationary, &mut storage)
            .raw_point_recorded
    );
    assert!(
        !workflow
            .process_fix(&moving_fix(), &mut storage)
            .raw_point_recorded
    );
    assert!(
        !workflow
            .process_fix(&moving_fix(), &mut storage)
            .raw_point_recorded
    );

    storage.write_ok = false;
    let failed = workflow.process_fix(&moving_fix(), &mut storage);
    assert!(failed.tracking_session_started && failed.raw_point_write_failed);
    assert_eq!(failed.point.unwrap().point_number, 1);

    storage.write_ok = true;
    let retried = workflow.process_fix(&moving_fix(), &mut storage);
    assert!(retried.raw_point_recorded);
    assert_eq!(retried.point.unwrap().point_number, 1);
}

#[test]
fn tracking_records_fresh_stopped_points_and_serializes_missing_values_as_null() {
    let mut workflow = TrackingWorkflow::new("a\"b");
    let mut storage = TrackStore {
        start_ok: true,
        write_ok: true,
        ..Default::default()
    };
    for _ in 0..3 {
        workflow.process_fix(&moving_fix(), &mut storage);
    }
    let mut stopped = moving_fix();
    stopped.utc = None;
    stopped.altitude_m = Some(f64::NAN);
    stopped.speed_kmh = Some(0.0);
    stopped.hdop = Some(8.2);
    stopped.satellites = Some(9);
    stopped.uptime_ms = 123_456;
    let decision = workflow.process_fix(&stopped, &mut storage);

    assert!(decision.raw_point_recorded);
    assert!(!decision.write_filtered_csv && !decision.write_filtered_gpx);
    assert_eq!(decision.point.unwrap().point_number, 2);
    let json: Value = serde_json::from_str(storage.lines.last().unwrap()).unwrap();
    assert_eq!(json["tracker_id"], "a\"b");
    assert!(json["gps_utc"].is_null() && json["altitude_m"].is_null());
    assert_eq!(json["satellites"], 9);
    assert_eq!(json["uptime_ms"], 123_456);
}

#[test]
fn recovery_uses_last_safe_progress_and_orders_oldest_first() {
    let good = serialize_delivery_progress(44, 30);
    let sessions = recover_tracking_sessions(&[
        StoredTrackingSession {
            tracking_session_number: 44,
            highest_recorded_point: 60,
            delivery_state: format!("{good}damaged\n"),
            delivery_state_readable: true,
        },
        StoredTrackingSession {
            tracking_session_number: 41,
            highest_recorded_point: 4,
            delivery_state: String::new(),
            delivery_state_readable: false,
        },
    ]);

    assert_eq!(sessions[0].tracking_session_number, 41);
    assert!(sessions[0].inactive && sessions[0].recovery_required);
    assert_eq!(sessions[1].highest_confirmed_point, 30);
    assert!(sessions[1].recovery_required);
    assert_eq!(next_tracking_session_number(40, 105), Some(106));
    assert_eq!(next_tracking_session_number(u32::MAX, 1), None);
}

#[derive(Default)]
struct RecoveryStore {
    append_ok: bool,
    appended: Vec<(String, String)>,
}

impl DeliveryRecoveryStorage for RecoveryStore {
    fn list_root(&mut self) -> Option<Vec<RecoveryDirectoryEntry>> {
        Some(Vec::new())
    }

    fn count_complete_lines(&mut self, _: &str) -> (RecoveryReadStatus, u32) {
        (RecoveryReadStatus::Missing, 0)
    }

    fn read_text(&mut self, _: &str) -> (RecoveryReadStatus, String) {
        (RecoveryReadStatus::Missing, String::new())
    }

    fn append_text(&mut self, path: &str, contents: &str) -> bool {
        if self.append_ok {
            self.appended.push((path.into(), contents.into()));
        }
        self.append_ok
    }
}

#[test]
fn recovery_advances_only_after_progress_is_persisted() {
    let mut recovery = DeliveryRecovery::default();
    let mut storage = RecoveryStore::default();
    assert!(recovery.restore(&mut storage));
    assert!(recovery.begin_session(1));
    assert!(recovery.record_point(1));

    assert!(!recovery.confirm_delivery_through(&mut storage, 1, 1));
    assert_eq!(recovery.sessions()[0].highest_confirmed_point, 0);
    storage.append_ok = true;
    assert!(recovery.confirm_delivery_through(&mut storage, 1, 1));
    assert_eq!(recovery.sessions()[0].highest_confirmed_point, 1);
    assert_eq!(
        storage.appended[0].0,
        "/session-0000000001/delivery-state.log"
    );
}

#[test]
fn scheduler_selects_only_deliverable_ranges_and_retry_caps() {
    assert!(select_oldest_pending_batch(&[recovered(41, 29, 0, false)]).is_none());
    let selected = select_oldest_pending_batch(&[
        recovered(44, 60, 30, true),
        recovered(41, 7, 0, true),
        recovered(43, 60, 0, true),
    ])
    .unwrap();
    assert_eq!(
        selected,
        PendingDeliveryBatch {
            tracking_session_number: 41,
            first_point_number: 1,
            point_count: 7,
        }
    );

    let mut retry = DeliveryRetrySchedule::default();
    assert_eq!(
        (0..6).map(|_| retry.record_failure()).collect::<Vec<_>>(),
        [15, 30, 60, 120, 300, 300]
    );
    retry.record_success();
    assert_eq!(retry.record_failure(), 15);
}

struct PointSource {
    status: BatchReadStatus,
    line_count: usize,
    empty_line: bool,
    request: Option<(u32, u32, usize)>,
}

impl UploadPointSource for PointSource {
    fn read_point_lines(
        &mut self,
        session: u32,
        first: u32,
        count: usize,
    ) -> (BatchReadStatus, Vec<String>) {
        self.request = Some((session, first, count));
        let mut lines = vec!["{}".to_owned(); self.line_count];
        if self.empty_line && lines.len() > 4 {
            lines[4].clear();
        }
        (self.status, lines)
    }
}

#[test]
fn upload_queue_preserves_source_failures_and_rejects_bad_input() {
    let sessions = [recovered(7, 30, 0, false)];
    for status in [
        BatchReadStatus::LockUnavailable,
        BatchReadStatus::StorageOpenFailed,
        BatchReadStatus::MalformedInput,
    ] {
        let mut source = PointSource {
            status,
            line_count: 30,
            empty_line: false,
            request: None,
        };
        assert_eq!(
            read_pending_batch(&sessions, &mut source, "tracker-01"),
            Err(status)
        );
    }

    let mut short = PointSource {
        status: BatchReadStatus::Ready,
        line_count: 29,
        empty_line: false,
        request: None,
    };
    assert_eq!(
        read_pending_batch(&sessions, &mut short, "tracker-01"),
        Err(BatchReadStatus::MalformedInput)
    );
    assert_eq!(short.request, Some((7, 1, 30)));

    let mut unused = PointSource {
        status: BatchReadStatus::Ready,
        line_count: 0,
        empty_line: false,
        request: None,
    };
    assert_eq!(
        read_pending_batch(&[recovered(7, 29, 30, false)], &mut unused, "tracker-01"),
        Err(BatchReadStatus::MalformedInput)
    );
    assert_eq!(unused.request, None);
}

#[test]
fn upload_contract_carries_identity_and_accepts_only_matching_confirmation() {
    let batch = batch();
    let request = build_upload_request(
        "https://uploads.example/v1/track-point-batches",
        "secret-token",
        &batch,
    )
    .unwrap();
    assert_eq!(
        header(&request, "Authorization"),
        Some("Bearer secret-token")
    );
    assert_eq!(
        header(&request, "Content-Type"),
        Some("application/x-ndjson")
    );
    assert_eq!(header(&request, "X-Tracker-ID"), Some("tracker-01"));
    assert_eq!(header(&request, "X-First-Point-Number"), Some("1"));
    assert_eq!(header(&request, "X-Last-Point-Number"), Some("30"));
    assert!(request.body.ends_with('\n'));

    let response = r#"{"tracker_id":"tracker-01","tracking_session_number":41,"highest_stored_point_number":30}"#;
    assert_eq!(
        validate_upload_response(200, response, &batch)
            .unwrap()
            .highest_stored_point_number,
        30
    );
    assert!(validate_upload_response(401, response, &batch).is_none());
    assert!(validate_upload_response(200, "not-json", &batch).is_none());
    assert!(validate_upload_response(
        200,
        r#"{"tracker_id":"wrong","tracker_id":"tracker-01","tracking_session_number":41,"highest_stored_point_number":30}"#,
        &batch
    )
    .is_none());

    let mut invalid = batch.clone();
    invalid.ndjson_points[12] = point(14);
    assert!(build_upload_request(
        "https://uploads.example/v1/track-point-batches",
        "secret-token",
        &invalid
    )
    .is_none());
    assert!(build_upload_request(
        "http://uploads.example/v1/track-point-batches",
        "secret-token",
        &batch
    )
    .is_none());
}

#[derive(Default)]
struct DiagnosticWriter {
    limit: usize,
    persisted: String,
}

impl DiagnosticLogStorage for DiagnosticWriter {
    fn append_diagnostic_bytes(&mut self, text: &[u8]) -> usize {
        let written = text.len().min(self.limit);
        self.persisted
            .push_str(std::str::from_utf8(&text[..written]).unwrap_or(""));
        written
    }
}

#[test]
fn diagnostics_retain_short_writes_and_require_exact_confirmation() {
    let mut log = DiagnosticLog::new(64);
    assert_eq!(
        log.append(None::<&mut DiagnosticWriter>, "[BOOT] reset=watchdog\n"),
        DiagnosticWriteStatus::Retained
    );
    let mut writer = DiagnosticWriter {
        limit: 7,
        ..Default::default()
    };
    assert_eq!(log.flush(&mut writer), DiagnosticWriteStatus::Retained);
    writer.limit = usize::MAX;
    assert_eq!(log.flush(&mut writer), DiagnosticWriteStatus::Persisted);
    assert_eq!(writer.persisted, "[BOOT] reset=watchdog\n");

    let upload = DiagnosticLogUpload {
        tracker_id: "tracker-01".into(),
        tracking_session_number: 41,
        contents: writer.persisted,
    };
    let first = build_diagnostic_upload_request(
        "https://uploads.example/v1/diagnostic-logs",
        "secret",
        &upload,
    )
    .unwrap();
    let retry = build_diagnostic_upload_request(
        "https://uploads.example/v1/diagnostic-logs",
        "secret",
        &upload,
    )
    .unwrap();
    assert_eq!(header(&first, "Idempotency-Key"), Some("tracker-01:41"));
    assert_eq!(first, retry);

    let response =
        r#"{"tracker_id":"tracker-01","tracking_session_number":41,"diagnostic_log_stored":true}"#;
    assert!(validate_diagnostic_upload_response(200, response, &upload));
    assert!(!validate_diagnostic_upload_response(
        200,
        r#"{"tracker_id":"tracker-01","tracking_session_number":41,"diagnostic_log_stored":true,"extra":1}"#,
        &upload
    ));
    assert!(!validate_diagnostic_upload_response(
        200,
        r#"{"tracker_id":"wrong","tracker_id":"tracker-01","tracking_session_number":41,"diagnostic_log_stored":true}"#,
        &upload
    ));
    assert_eq!(
        classify_http_failure(-11, false),
        DiagnosticUploadFailure::Timeout
    );
    assert!(diagnostic_upload_failure_message(DiagnosticUploadFailure::Tls).contains("TLS"));
}

#[test]
fn diagnostics_survive_a_write_split_inside_utf8() {
    struct ByteWriter(Vec<u8>);
    impl DiagnosticLogStorage for ByteWriter {
        fn append_diagnostic_bytes(&mut self, bytes: &[u8]) -> usize {
            let written = bytes.len().min(1);
            self.0.extend_from_slice(&bytes[..written]);
            written
        }
    }

    let mut log = DiagnosticLog::new(8);
    let mut writer = ByteWriter(Vec::new());
    assert_eq!(
        log.append(Some(&mut writer), "é"),
        DiagnosticWriteStatus::Retained
    );
    assert_eq!(writer.0, [0xc3]);
    assert_eq!(log.pending(), [0xa9]);
}

#[test]
fn diagnostic_logs_stay_pending_until_confirmation_is_persisted() {
    let mut logs = vec![
        StoredDiagnosticLog {
            tracking_session_number: 42,
            contents: "newer".into(),
            delivery_state: String::new(),
        },
        StoredDiagnosticLog {
            tracking_session_number: 41,
            contents: "older".into(),
            delivery_state: String::new(),
        },
    ];
    assert_eq!(
        select_oldest_pending_diagnostic_log("tracker-01", &logs)
            .unwrap()
            .tracking_session_number,
        41
    );

    let mut storage = RecoveryStore::default();
    assert!(!confirm_diagnostic_delivery(
        &mut storage,
        "tracker-01",
        41,
        &mut logs
    ));
    assert!(logs[1].delivery_state.is_empty());
    storage.append_ok = true;
    assert!(confirm_diagnostic_delivery(
        &mut storage,
        "tracker-01",
        41,
        &mut logs
    ));
    assert_eq!(
        select_oldest_pending_diagnostic_log("tracker-01", &logs)
            .unwrap()
            .tracking_session_number,
        42
    );
}

struct RawWriter {
    limits: Vec<usize>,
    bytes: Vec<u8>,
    flushes: usize,
    abandoned: bool,
}

impl RawPointLogStorage for RawWriter {
    fn append(&mut self, bytes: &[u8]) -> usize {
        let written = bytes.len().min(self.limits.remove(0));
        self.bytes.extend_from_slice(&bytes[..written]);
        written
    }

    fn flush(&mut self) {
        self.flushes += 1;
    }

    fn abandon(&mut self) {
        self.abandoned = true;
    }
}

#[test]
fn raw_point_append_flushes_complete_records_and_abandons_partial_ones() {
    let mut complete = RawWriter {
        limits: vec![usize::MAX, 1],
        bytes: Vec::new(),
        flushes: 0,
        abandoned: false,
    };
    assert!(append_complete_raw_point(&mut complete, r#"{"point":1}"#));
    assert_eq!(complete.bytes, b"{\"point\":1}\n");
    assert_eq!(complete.flushes, 1);
    assert!(!complete.abandoned);

    for limits in [vec![5], vec![usize::MAX, 0]] {
        let mut partial = RawWriter {
            limits,
            bytes: Vec::new(),
            flushes: 0,
            abandoned: false,
        };
        assert!(!append_complete_raw_point(&mut partial, r#"{"point":1}"#));
        assert_eq!(partial.flushes, 1);
        assert!(partial.abandoned);
    }
}
