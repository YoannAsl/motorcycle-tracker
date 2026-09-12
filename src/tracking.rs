use serde_json::{json, Value};

#[derive(Clone, Debug, Default, PartialEq)]
pub struct GpsFix {
    pub location_valid: bool,
    pub location_fresh: bool,
    pub latitude: f64,
    pub longitude: f64,
    pub utc: Option<String>,
    pub altitude_m: Option<f64>,
    pub speed_kmh: Option<f64>,
    pub course_deg: Option<f64>,
    pub hdop: Option<f64>,
    pub satellites: Option<u32>,
    pub uptime_ms: u32,
}

#[derive(Clone, Debug, PartialEq)]
pub struct TrackPoint {
    pub schema_version: u32,
    pub tracker_id: String,
    pub tracking_session_number: u32,
    pub point_number: u32,
    pub gps_utc: Option<String>,
    pub latitude: f64,
    pub longitude: f64,
    pub altitude_m: Option<f64>,
    pub speed_kmh: Option<f64>,
    pub course_deg: Option<f64>,
    pub hdop: Option<f64>,
    pub satellites: Option<u32>,
    pub uptime_ms: u32,
}

pub trait TrackingStorage {
    fn start_tracking_session(&mut self) -> Option<u32>;
    fn append_and_flush_raw_point(&mut self, point: &TrackPoint, ndjson: &str) -> bool;
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct TrackingDecision {
    pub tracking_session_started: bool,
    pub raw_point_recorded: bool,
    pub raw_point_write_failed: bool,
    pub write_filtered_csv: bool,
    pub write_filtered_gpx: bool,
    pub point: Option<TrackPoint>,
}

pub struct TrackingWorkflow {
    tracker_id: String,
    consecutive_start_candidates: u8,
    active: bool,
    tracking_session_number: u32,
    next_point_number: u32,
}

impl TrackingWorkflow {
    pub fn new(tracker_id: impl Into<String>) -> Self {
        Self {
            tracker_id: tracker_id.into(),
            consecutive_start_candidates: 0,
            active: false,
            tracking_session_number: 0,
            next_point_number: 1,
        }
    }

    pub fn process_fix(
        &mut self,
        fix: &GpsFix,
        storage: &mut impl TrackingStorage,
    ) -> TrackingDecision {
        let mut decision = TrackingDecision::default();
        if !self.active {
            if !is_start_candidate(fix) {
                self.consecutive_start_candidates = 0;
                return decision;
            }
            self.consecutive_start_candidates += 1;
            if self.consecutive_start_candidates < 3 {
                return decision;
            }
            let Some(number) = storage.start_tracking_session() else {
                self.consecutive_start_candidates = 2;
                return decision;
            };
            self.tracking_session_number = number;
            self.active = true;
            decision.tracking_session_started = true;
        } else if !is_fresh_location(fix) {
            return decision;
        }

        let point = self.make_point(fix);
        let ndjson = serialize_track_point(&point);
        if !storage.append_and_flush_raw_point(&point, &ndjson) {
            decision.raw_point_write_failed = true;
            decision.point = Some(point);
            return decision;
        }

        decision.raw_point_recorded = true;
        decision.write_filtered_csv = point.speed_kmh.is_some_and(|speed| speed > 2.0);
        decision.write_filtered_gpx = decision.write_filtered_csv;
        decision.point = Some(point);
        self.next_point_number = self.next_point_number.wrapping_add(1);
        decision
    }

    pub fn tracking_session_active(&self) -> bool {
        self.active
    }

    fn make_point(&self, fix: &GpsFix) -> TrackPoint {
        TrackPoint {
            schema_version: 1,
            tracker_id: self.tracker_id.clone(),
            tracking_session_number: self.tracking_session_number,
            point_number: self.next_point_number,
            gps_utc: fix.utc.clone().filter(|value| !value.is_empty()),
            latitude: fix.latitude,
            longitude: fix.longitude,
            altitude_m: finite(fix.altitude_m),
            speed_kmh: finite(fix.speed_kmh),
            course_deg: finite(fix.course_deg),
            hdop: finite(fix.hdop),
            satellites: fix.satellites,
            uptime_ms: fix.uptime_ms,
        }
    }
}

fn finite(value: Option<f64>) -> Option<f64> {
    value.filter(|value| value.is_finite())
}

fn is_fresh_location(fix: &GpsFix) -> bool {
    fix.location_valid
        && fix.location_fresh
        && fix.latitude.is_finite()
        && fix.longitude.is_finite()
}

fn is_start_candidate(fix: &GpsFix) -> bool {
    is_fresh_location(fix)
        && fix.utc.as_ref().is_some_and(|utc| !utc.is_empty())
        && fix
            .speed_kmh
            .is_some_and(|speed| speed.is_finite() && speed > 2.0)
        && fix.hdop.is_some_and(|hdop| hdop.is_finite() && hdop <= 5.0)
}

pub fn serialize_track_point(point: &TrackPoint) -> String {
    let value = json!({
        "schema_version": point.schema_version,
        "tracker_id": point.tracker_id,
        "tracking_session_number": point.tracking_session_number,
        "point_number": point.point_number,
        "gps_utc": point.gps_utc,
        "latitude": point.latitude,
        "longitude": point.longitude,
        "altitude_m": finite(point.altitude_m),
        "speed_kmh": finite(point.speed_kmh),
        "course_deg": finite(point.course_deg),
        "hdop": finite(point.hdop),
        "satellites": point.satellites,
        "uptime_ms": point.uptime_ms,
    });
    serde_json::to_string(&value).expect("track point contains valid JSON values")
}

pub fn track_point_identity(line: &str) -> Option<(u32, String, u32, u32)> {
    let value: Value = serde_json::from_str(line).ok()?;
    let object = value.as_object()?;
    Some((
        u32::try_from(object.get("schema_version")?.as_u64()?).ok()?,
        object.get("tracker_id")?.as_str()?.to_owned(),
        u32::try_from(object.get("tracking_session_number")?.as_u64()?).ok()?,
        u32::try_from(object.get("point_number")?.as_u64()?).ok()?,
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Default)]
    struct MemoryStorage {
        start_ok: bool,
        write_ok: bool,
        points: Vec<String>,
    }

    impl TrackingStorage for MemoryStorage {
        fn start_tracking_session(&mut self) -> Option<u32> {
            self.start_ok.then_some(41)
        }

        fn append_and_flush_raw_point(&mut self, _: &TrackPoint, ndjson: &str) -> bool {
            if self.write_ok {
                self.points.push(ndjson.to_owned());
            }
            self.write_ok
        }
    }

    fn moving_fix() -> GpsFix {
        GpsFix {
            location_valid: true,
            location_fresh: true,
            latitude: 48.856613,
            longitude: 2.352222,
            utc: Some("2026-08-30T12:34:56Z".into()),
            speed_kmh: Some(42.5),
            hdop: Some(0.9),
            ..Default::default()
        }
    }

    #[test]
    fn third_qualifying_fix_starts_and_records_point_one() {
        let mut workflow = TrackingWorkflow::new("a\"b");
        let mut storage = MemoryStorage {
            start_ok: true,
            write_ok: true,
            ..Default::default()
        };
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
        let decision = workflow.process_fix(&moving_fix(), &mut storage);
        assert!(decision.tracking_session_started && decision.raw_point_recorded);
        assert_eq!(decision.point.unwrap().point_number, 1);
    }

    #[test]
    fn active_session_keeps_stopped_points_but_filters_exports() {
        let mut workflow = TrackingWorkflow::new("tracker-01");
        let mut storage = MemoryStorage {
            start_ok: true,
            write_ok: true,
            ..Default::default()
        };
        for _ in 0..3 {
            workflow.process_fix(&moving_fix(), &mut storage);
        }
        let mut stopped = moving_fix();
        stopped.speed_kmh = Some(0.0);
        stopped.hdop = Some(20.0);
        let decision = workflow.process_fix(&stopped, &mut storage);
        assert!(decision.raw_point_recorded);
        assert!(!decision.write_filtered_csv && !decision.write_filtered_gpx);
    }

    #[test]
    fn nonfinite_optional_values_serialize_as_null() {
        let mut workflow = TrackingWorkflow::new("a\"b");
        let mut storage = MemoryStorage {
            start_ok: true,
            write_ok: true,
            ..Default::default()
        };
        let mut fix = moving_fix();
        fix.altitude_m = Some(f64::NAN);
        for _ in 0..3 {
            workflow.process_fix(&fix, &mut storage);
        }
        let value: Value = serde_json::from_str(&storage.points[0]).unwrap();
        assert_eq!(value["tracker_id"], "a\"b");
        assert!(value["altitude_m"].is_null());
    }
}
