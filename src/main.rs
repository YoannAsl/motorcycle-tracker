#[cfg(not(target_os = "espidf"))]
fn main() {
    println!("build with --target xtensa-esp32-espidf");
}

#[cfg(target_os = "espidf")]
fn main() -> anyhow::Result<()> {
    firmware::run()
}

#[cfg(target_os = "espidf")]
mod firmware {
    use anyhow::{anyhow, Context, Result};
    use embedded_svc::{
        http::client::Client,
        io::Write as _,
        wifi::{AuthMethod, ClientConfiguration, Configuration as WifiConfig},
    };
    use esp_idf_svc::{
        eventloop::EspSystemEventLoop,
        fs::fatfs::Fatfs,
        hal::{
            gpio::{self, AnyIOPin},
            peripherals::Peripherals,
            sd::{spi::SdSpiHostDriver, SdCardConfiguration, SdCardDriver},
            spi::{config::DriverConfig, Dma, SpiDriver},
            uart::{config::Config as UartConfig, UartDriver},
            units::Hertz,
        },
        http::client::{Configuration as HttpConfig, EspHttpConnection},
        io::vfs::MountedFatfs,
        log::EspLogger,
        nvs::{EspDefaultNvs, EspDefaultNvsPartition, EspNvs},
        wifi::{BlockingWifi, EspWifi},
    };
    use log::{error, info, warn};
    use motorcycle_tracker::{
        cleanup::{
            run_storage_cleanup, CleanupSession, CleanupStorageStatus, CleanupTrigger,
            StorageCleanupSchedule, StorageCleanupStorage,
        },
        delivery::{
            build_diagnostic_upload_request, build_upload_request, confirm_diagnostic_delivery,
            read_pending_batch, select_oldest_pending_diagnostic_log, session_directory_path,
            session_file_path, validate_diagnostic_upload_response, validate_upload_response,
            BatchReadStatus, DeliveryRecovery, DeliveryRecoveryStorage, DeliveryRetrySchedule,
            DiagnosticLog, DiagnosticLogStorage, HttpRequest, RecoveryDirectoryEntry,
            RecoveryReadStatus, StoredDiagnosticLog, UploadBatch, UploadPointSource,
        },
        tracking::{GpsFix, TrackPoint, TrackingStorage, TrackingWorkflow},
    };
    use nmea::Nmea;
    use std::{
        fs::{self, File, OpenOptions},
        io::{BufRead, BufReader, Seek, SeekFrom, Write},
        path::{Path, PathBuf},
        sync::{Arc, Mutex},
        thread,
        time::{Duration, Instant},
    };

    const ROOT: &str = "/sdcard";
    const ID: &str = match option_env!("TRACKER_ID") {
        Some(value) => value,
        None => "replace-with-stable-tracker-id",
    };
    const TOKEN: &str = match option_env!("TRACKER_TOKEN") {
        Some(value) => value,
        None => "replace-with-bearer-token",
    };
    const URL: &str = match option_env!("UPLOAD_URL") {
        Some(value) => value,
        None => "https://uploads.example/v1/track-point-batches",
    };
    const SSID: &str = match option_env!("WIFI_SSID") {
        Some(value) => value,
        None => "replace-with-phone-hotspot-name",
    };
    const PASS: &str = match option_env!("WIFI_PASSWORD") {
        Some(value) => value,
        None => "replace-with-phone-hotspot-password",
    };
    const GPX_HEAD: &str = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<gpx version=\"1.1\" creator=\"ESP32 GPS Logger\" xmlns=\"http://www.topografix.com/GPX/1/1\">\n  <trk><name>Motorcycle tracking session</name><trkseg>\n";
    const GPX_TAIL: &str = "  </trkseg></trk>\n</gpx>\n";

    struct Storage {
        nvs: EspDefaultNvs,
        recovery: DeliveryRecovery,
        raw: Option<File>,
        csv: Option<File>,
        gpx: Option<File>,
        log: Option<File>,
        log_path: Option<PathBuf>,
        pending_log: DiagnosticLog,
        stored_logs: Vec<StoredDiagnosticLog>,
        pending_cleanup_log: Vec<u8>,
    }
    impl Storage {
        fn new(nvs: EspDefaultNvs) -> Self {
            Self {
                nvs,
                recovery: DeliveryRecovery::default(),
                raw: None,
                csv: None,
                gpx: None,
                log: None,
                log_path: None,
                pending_log: DiagnosticLog::default(),
                stored_logs: Vec::new(),
                pending_cleanup_log: Vec::new(),
            }
        }
        fn restore(&mut self) -> bool {
            let mut recovery = std::mem::take(&mut self.recovery);
            let ok = recovery.restore(self);
            self.recovery = recovery;
            if ok {
                self.stored_logs = self
                    .recovery
                    .sessions()
                    .iter()
                    .filter_map(|session| {
                        let number = session.tracking_session_number;
                        let contents =
                            fs::read_to_string(full(&session_file_path(number, "session.log")))
                                .ok()?;
                        let delivery_state = fs::read_to_string(full(&session_file_path(
                            number,
                            "diagnostic-delivery-state.log",
                        )))
                        .unwrap_or_default();
                        Some(StoredDiagnosticLog {
                            tracking_session_number: number,
                            contents,
                            delivery_state,
                        })
                    })
                    .collect();
            }
            ok
        }
        fn diagnostic(&mut self, text: &str) {
            info!("{}", text.trim_end());
            let mut pending = std::mem::take(&mut self.pending_log);
            let status = if self.log_path.is_some() {
                pending.append(Some(self), text)
            } else {
                pending.append(None::<&mut Storage>, text)
            };
            self.pending_log = pending;
            if status == motorcycle_tracker::delivery::DiagnosticWriteStatus::Full {
                error!("diagnostic buffer full");
            }
        }
        fn exports(&mut self, p: &TrackPoint) {
            let opt = |v: Option<f64>| v.map(|v| v.to_string()).unwrap_or_default();
            let timestamp = p
                .gps_utc
                .clone()
                .unwrap_or_else(|| format!("BOOT+{}", p.uptime_ms));
            if let Some(f) = &mut self.csv {
                let _ = writeln!(
                    f,
                    "{},{:.6},{:.6},{},{},{},{},{}",
                    timestamp,
                    p.latitude,
                    p.longitude,
                    opt(p.altitude_m),
                    opt(p.speed_kmh),
                    opt(p.course_deg),
                    opt(p.hdop),
                    p.satellites.map(|v| v.to_string()).unwrap_or_default()
                );
                let _ = f.sync_data();
            }
            if let Some(f) = &mut self.gpx {
                if let Ok(n) = f.seek(SeekFrom::End(0)) {
                    if n >= GPX_TAIL.len() as u64 {
                        let _ = f.seek(SeekFrom::Start(n - GPX_TAIL.len() as u64));
                    }
                }
                let _ = write!(
                    f,
                    "    <trkpt lat=\"{:.6}\" lon=\"{:.6}\">",
                    p.latitude, p.longitude
                );
                if let Some(v) = p.altitude_m {
                    let _ = write!(f, "<ele>{v:.1}</ele>");
                }
                let _ = writeln!(f, "<time>{}</time></trkpt>\n{GPX_TAIL}", timestamp);
                if let Ok(end) = f.stream_position() {
                    let _ = f.set_len(end);
                }
                let _ = f.sync_data();
            }
        }
        fn confirm(&mut self, session: u32, highest: u32) -> bool {
            let Some(s) = self
                .recovery
                .sessions()
                .iter()
                .find(|s| s.tracking_session_number == session)
            else {
                return false;
            };
            if highest < s.highest_confirmed_point || highest > s.highest_recorded_point {
                return false;
            }
            if !self.append_text(
                &session_file_path(session, "delivery-state.log"),
                &motorcycle_tracker::delivery::serialize_delivery_progress(session, highest),
            ) {
                return false;
            }
            if let Some(s) = self
                .recovery
                .sessions_mut()
                .iter_mut()
                .find(|s| s.tracking_session_number == session)
            {
                s.highest_confirmed_point = highest;
                s.recovery_required = false;
                true
            } else {
                false
            }
        }
    }
    fn full(path: &str) -> PathBuf {
        Path::new(ROOT).join(path.trim_start_matches('/'))
    }
    fn append(path: impl AsRef<Path>) -> Option<File> {
        OpenOptions::new()
            .create(true)
            .read(true)
            .append(true)
            .open(path)
            .ok()
    }
    fn open_gpx(path: impl AsRef<Path>) -> Option<File> {
        OpenOptions::new()
            .create(true)
            .read(true)
            .write(true)
            .truncate(true)
            .open(path)
            .ok()
    }

    impl DiagnosticLogStorage for Storage {
        fn append_diagnostic_bytes(&mut self, bytes: &[u8]) -> usize {
            if self.log.is_none() {
                self.log = self.log_path.as_ref().and_then(append);
            }
            let Some(file) = &mut self.log else { return 0 };
            let written = motorcycle_tracker::delivery::write_synced_bytes(file, bytes, |file| {
                file.sync_data()
            });
            if written < bytes.len() {
                self.log = None;
            }
            written
        }
    }

    fn directory_size(path: &Path) -> std::io::Result<u64> {
        let mut bytes = 0u64;
        for entry in fs::read_dir(path)? {
            let entry = entry?;
            let metadata = entry.metadata()?;
            bytes = bytes.saturating_add(if metadata.is_dir() {
                directory_size(&entry.path())?
            } else {
                metadata.len()
            });
        }
        Ok(bytes)
    }

    impl StorageCleanupStorage for Storage {
        fn read_usage(&mut self) -> std::result::Result<(u64, u64), CleanupStorageStatus> {
            let root = std::ffi::CString::new(ROOT)
                .map_err(|_| CleanupStorageStatus::MetricUnavailable)?;
            let mut total = 0u64;
            let mut free = 0u64;
            let status =
                unsafe { esp_idf_svc::sys::esp_vfs_fat_info(root.as_ptr(), &mut total, &mut free) };
            if status != esp_idf_svc::sys::ESP_OK || total == 0 {
                return Err(CleanupStorageStatus::MetricUnavailable);
            }
            Ok((total, total.saturating_sub(free)))
        }

        fn read_cleanup_sessions(
            &mut self,
        ) -> std::result::Result<Vec<CleanupSession>, CleanupStorageStatus> {
            self.recovery
                .sessions()
                .iter()
                .map(|session| {
                    let number = session.tracking_session_number;
                    let diagnostic_log_confirmed = self.stored_logs.iter().any(|log| {
                        log.tracking_session_number == number
                            && log.delivery_state.contains(
                                &motorcycle_tracker::delivery::serialize_diagnostic_delivery(
                                    ID, number,
                                ),
                            )
                    });
                    Ok(CleanupSession {
                        tracking_session_number: number,
                        bytes: directory_size(&full(&session_directory_path(number)))
                            .map_err(|_| CleanupStorageStatus::ScanFailed)?,
                        inactive: session.inactive,
                        all_points_confirmed: session.highest_confirmed_point
                            >= session.highest_recorded_point,
                        diagnostic_log_confirmed,
                    })
                })
                .collect()
        }

        fn delete_session_files(
            &mut self,
            number: u32,
        ) -> std::result::Result<(), CleanupStorageStatus> {
            fs::remove_dir_all(full(&session_directory_path(number)))
                .map_err(|_| CleanupStorageStatus::DeleteFailed)
        }

        fn forget_deleted_session(&mut self, number: u32) {
            self.recovery.forget_session(number);
            self.stored_logs
                .retain(|log| log.tracking_session_number != number);
        }

        fn flush_pending_cleanup_diagnostic(
            &mut self,
        ) -> std::result::Result<(), CleanupStorageStatus> {
            if self.pending_cleanup_log.is_empty() {
                return Ok(());
            }
            let mut file =
                append(full("/cleanup.log")).ok_or(CleanupStorageStatus::DiagnosticWriteFailed)?;
            file.write_all(&self.pending_cleanup_log)
                .and_then(|_| file.sync_data())
                .map_err(|_| CleanupStorageStatus::DiagnosticWriteFailed)?;
            self.pending_cleanup_log.clear();
            Ok(())
        }

        fn persist_cleanup_diagnostic(
            &mut self,
            message: &str,
        ) -> std::result::Result<(), CleanupStorageStatus> {
            if !self.pending_cleanup_log.is_empty() {
                return Err(CleanupStorageStatus::DiagnosticWriteFailed);
            }
            self.pending_cleanup_log
                .extend_from_slice(message.as_bytes());
            self.flush_pending_cleanup_diagnostic()
        }
    }

    impl TrackingStorage for Storage {
        fn start_tracking_session(&mut self) -> Option<u32> {
            if !self.recovery.ready() {
                return None;
            }
            let old = self.nvs.get_u32("session").ok().flatten().unwrap_or(0);
            let mut n = motorcycle_tracker::delivery::next_tracking_session_number(
                old,
                self.recovery.max_stored_tracking_session_number(),
            )?;
            while full(&session_directory_path(n)).exists() {
                n = n.checked_add(1)?;
            }
            let dir = full(&session_directory_path(n));
            fs::create_dir(&dir).ok()?;
            self.nvs.set_u32("session", n).ok()?;
            self.raw = append(dir.join("track-points.ndjson"));
            self.csv = append(dir.join("gpslog.csv"));
            self.gpx = open_gpx(dir.join("gpslog.gpx"));
            let log_path = dir.join("session.log");
            self.log = append(&log_path);
            if self.raw.is_none() || self.csv.is_none() || self.gpx.is_none() || self.log.is_none()
            {
                return None;
            }
            writeln!(
                self.csv.as_mut()?,
                "timestamp,lat,lon,alt_m,speed_kmh,course_deg,hdop,sats"
            )
            .ok()?;
            write!(self.gpx.as_mut()?, "{GPX_HEAD}{GPX_TAIL}").ok()?;
            self.csv.as_ref()?.sync_data().ok()?;
            self.gpx.as_ref()?.sync_data().ok()?;
            if !self.recovery.begin_session(n) {
                return None;
            }
            self.log_path = Some(log_path);
            let mut pending = std::mem::take(&mut self.pending_log);
            let _ = pending.flush(self);
            self.pending_log = pending;
            self.diagnostic(&format!("[TRACK] session={n} started\n"));
            Some(n)
        }
        fn append_and_flush_raw_point(&mut self, p: &TrackPoint, text: &str) -> bool {
            let Some(f) = &mut self.raw else { return false };
            if f.write_all(text.as_bytes())
                .and_then(|_| f.write_all(b"\n"))
                .and_then(|_| f.sync_data())
                .is_err()
            {
                self.raw = None;
                return false;
            }
            self.recovery.record_point(p.tracking_session_number)
        }
    }
    impl DeliveryRecoveryStorage for Storage {
        fn list_root(&mut self) -> Option<Vec<RecoveryDirectoryEntry>> {
            Some(
                fs::read_dir(ROOT)
                    .ok()?
                    .filter_map(|e| {
                        let e = e.ok()?;
                        Some(RecoveryDirectoryEntry {
                            name: e.file_name().to_string_lossy().into_owned(),
                            directory: e.file_type().ok()?.is_dir(),
                        })
                    })
                    .collect(),
            )
        }
        fn count_complete_lines(&mut self, path: &str) -> (RecoveryReadStatus, u32) {
            let p = full(path);
            if !p.exists() {
                return (RecoveryReadStatus::Missing, 0);
            }
            let Ok(file) = File::open(p) else {
                return (RecoveryReadStatus::Unreadable, 0);
            };
            let mut reader = BufReader::new(file);
            let mut count = 0u32;
            loop {
                let Ok(bytes) = reader.fill_buf() else {
                    return (RecoveryReadStatus::Unreadable, 0);
                };
                if bytes.is_empty() {
                    return (RecoveryReadStatus::Readable, count);
                }
                let consumed = bytes.len();
                count = count.saturating_add(
                    bytes
                        .iter()
                        .filter(|byte| **byte == b'\n')
                        .count()
                        .min(u32::MAX as usize) as u32,
                );
                reader.consume(consumed);
            }
        }
        fn read_text(&mut self, path: &str) -> (RecoveryReadStatus, String) {
            let p = full(path);
            if !p.exists() {
                return (RecoveryReadStatus::Missing, String::new());
            }
            fs::read_to_string(p).map_or((RecoveryReadStatus::Unreadable, String::new()), |s| {
                (RecoveryReadStatus::Readable, s)
            })
        }
        fn append_text(&mut self, path: &str, text: &str) -> bool {
            append(full(path)).is_some_and(|mut f| {
                f.write_all(text.as_bytes())
                    .and_then(|_| f.sync_data())
                    .is_ok()
            })
        }
    }
    impl UploadPointSource for Storage {
        fn read_point_lines(
            &mut self,
            s: u32,
            first: u32,
            count: usize,
        ) -> (BatchReadStatus, Vec<String>) {
            let Ok(f) = File::open(full(&session_file_path(s, "track-points.ndjson"))) else {
                return (BatchReadStatus::StorageOpenFailed, vec![]);
            };
            match BufReader::new(f)
                .lines()
                .skip((first - 1) as usize)
                .take(count)
                .collect::<std::io::Result<Vec<_>>>()
            {
                Ok(v) if v.len() == count && v.iter().all(|s| !s.is_empty()) => {
                    (BatchReadStatus::Ready, v)
                }
                _ => (BatchReadStatus::MalformedInput, vec![]),
            }
        }
    }

    fn connect(w: &mut BlockingWifi<EspWifi<'static>>) -> Result<()> {
        w.set_configuration(&WifiConfig::Client(ClientConfiguration {
            ssid: SSID.try_into().map_err(|_| anyhow!("SSID too long"))?,
            password: PASS.try_into().map_err(|_| anyhow!("password too long"))?,
            auth_method: AuthMethod::WPA2Personal,
            ..Default::default()
        }))?;
        if !w.is_started()? {
            w.start()?
        }
        if !w.is_connected()? {
            w.connect()?;
            w.wait_netif_up()?
        }
        Ok(())
    }
    fn read_http_body(reader: &mut impl embedded_svc::io::Read) -> Option<String> {
        let mut body = Vec::new();
        let mut chunk = [0u8; 256];
        loop {
            let count = reader.read(&mut chunk).ok()?;
            if count == 0 {
                return String::from_utf8(body).ok();
            }
            if body.len() + count > 4096 {
                return None;
            }
            body.extend_from_slice(&chunk[..count]);
        }
    }
    fn send_request(
        w: &mut BlockingWifi<EspWifi<'static>>,
        request: &HttpRequest,
    ) -> Option<(u16, String)> {
        connect(w).ok()?;
        let cfg = HttpConfig {
            crt_bundle_attach: Some(esp_idf_svc::sys::esp_crt_bundle_attach),
            timeout: Some(Duration::from_secs(15)),
            ..Default::default()
        };
        let Ok(c) = EspHttpConnection::new(&cfg) else {
            return None;
        };
        let mut c = Client::wrap(c);
        let h: Vec<(&str, &str)> = request
            .headers
            .iter()
            .map(|x| (x.0.as_str(), x.1.as_str()))
            .collect();
        let mut q = c.post(&request.url, &h).ok()?;
        if q.write_all(request.body.as_bytes())
            .and_then(|_| q.flush())
            .is_err()
        {
            return None;
        }
        let mut p = q.submit().ok()?;
        let status = p.status();
        Some((status, read_http_body(&mut p)?))
    }
    fn upload(w: &mut BlockingWifi<EspWifi<'static>>, batch: &UploadBatch) -> bool {
        let Some(request) = build_upload_request(URL, TOKEN, batch) else {
            return false;
        };
        let Some((status, body)) = send_request(w, &request) else {
            return false;
        };
        validate_upload_response(status, &body, batch).is_some()
    }
    fn diagnostic_url() -> Option<String> {
        Some(format!(
            "{}/v1/diagnostic-logs",
            URL.strip_suffix("/v1/track-point-batches")?
        ))
    }
    fn uploader(storage: Arc<Mutex<Storage>>, mut wifi: BlockingWifi<EspWifi<'static>>) {
        let start = Instant::now();
        let mut point_retry = DeliveryRetrySchedule::default();
        let mut log_retry = DeliveryRetrySchedule::default();
        let mut cleanup = StorageCleanupSchedule::default();
        cleanup.request(CleanupTrigger::Startup);
        loop {
            let now = start.elapsed().as_millis() as u32;
            if cleanup.ready(now) {
                if let Ok(mut s) = storage.lock() {
                    let run = run_storage_cleanup(&mut *s);
                    cleanup.record_run(&run, now);
                }
            }
            if point_retry.ready(now) {
                let batch =
                    storage
                        .lock()
                        .map_or(Err(BatchReadStatus::LockUnavailable), |mut s| {
                            let sessions = s.recovery.sessions().to_vec();
                            read_pending_batch(&sessions, &mut *s, ID)
                        });
                match batch {
                    Ok(b) => {
                        let ok = upload(&mut wifi, &b)
                            && storage.lock().is_ok_and(|mut s| {
                                s.confirm(
                                    b.tracking_session_number,
                                    b.first_point_number + b.ndjson_points.len() as u32 - 1,
                                )
                            });
                        if ok {
                            point_retry.schedule_success(now);
                            cleanup.request(CleanupTrigger::DeliveryConfirmation);
                            if let Ok(mut s) = storage.lock() {
                                s.diagnostic(&format!(
                                    "[UPLOAD] session={} through={} confirmed\n",
                                    b.tracking_session_number,
                                    b.first_point_number + b.ndjson_points.len() as u32 - 1
                                ));
                            }
                        } else {
                            let delay = point_retry.schedule_failure(now);
                            if let Ok(mut s) = storage.lock() {
                                s.diagnostic(&format!(
                                    "[UPLOAD] point batch failed; retry={delay}s\n"
                                ));
                            }
                        }
                    }
                    Err(BatchReadStatus::NoPendingBatch) => point_retry.schedule_success(now),
                    Err(_) => {
                        let delay = point_retry.schedule_failure(now);
                        if let Ok(mut s) = storage.lock() {
                            s.diagnostic(&format!(
                                "[UPLOAD] point storage read failed; retry={delay}s\n"
                            ));
                        }
                    }
                }
            }
            if log_retry.ready(now) {
                let upload = storage
                    .lock()
                    .ok()
                    .and_then(|s| select_oldest_pending_diagnostic_log(ID, &s.stored_logs));
                if let Some(upload) = upload {
                    let ok = diagnostic_url()
                        .and_then(|url| build_diagnostic_upload_request(&url, TOKEN, &upload))
                        .and_then(|request| send_request(&mut wifi, &request))
                        .is_some_and(|(status, body)| {
                            validate_diagnostic_upload_response(status, &body, &upload)
                        })
                        && storage.lock().is_ok_and(|mut s| {
                            let mut logs = std::mem::take(&mut s.stored_logs);
                            let confirmed = confirm_diagnostic_delivery(
                                &mut *s,
                                ID,
                                upload.tracking_session_number,
                                &mut logs,
                            );
                            s.stored_logs = logs;
                            confirmed
                        });
                    if ok {
                        log_retry.schedule_success(now);
                        cleanup.request(CleanupTrigger::DeliveryConfirmation);
                    } else {
                        let delay = log_retry.schedule_failure(now);
                        if let Ok(mut s) = storage.lock() {
                            s.diagnostic(&format!(
                                "[UPLOAD] diagnostic log failed; retry={delay}s\n"
                            ));
                        }
                    }
                } else {
                    log_retry.schedule_success(now);
                }
            }
            thread::sleep(Duration::from_secs(1));
        }
    }
    fn fix(n: &Nmea, fresh: bool, ms: u32) -> GpsFix {
        GpsFix {
            location_valid: n.latitude.is_some()
                && n.longitude.is_some()
                && n.fix_type().is_some_and(|kind| kind.is_valid()),
            location_fresh: fresh,
            latitude: n.latitude.unwrap_or_default(),
            longitude: n.longitude.unwrap_or_default(),
            utc: n.fix_date.zip(n.fix_time).map(|(d, t)| format!("{d}T{t}Z")),
            altitude_m: n.altitude.map(f64::from),
            speed_kmh: n.speed_over_ground.map(|v| f64::from(v) * 1.852),
            course_deg: n.true_course.map(f64::from),
            hdop: n.hdop.map(f64::from),
            satellites: n.num_of_fix_satellites,
            uptime_ms: ms,
        }
    }

    pub fn run() -> Result<()> {
        esp_idf_svc::sys::link_patches();
        EspLogger::initialize_default();
        let p = Peripherals::take()?;
        let pins = p.pins;
        let spi = SpiDriver::new(
            p.spi3,
            pins.gpio18,
            pins.gpio23,
            Some(pins.gpio19),
            &DriverConfig::default().dma(Dma::Auto(4096)),
        )?;
        let card = SdCardDriver::new_spi(
            SdSpiHostDriver::new(
                spi,
                Some(pins.gpio5),
                AnyIOPin::none(),
                AnyIOPin::none(),
                AnyIOPin::none(),
                None,
            )?,
            &SdCardConfiguration::new(),
        )?;
        // Four session files stay open; uploads and confirmations need one transient descriptor.
        let _fat = MountedFatfs::mount(Fatfs::new_sdcard(0, card)?, ROOT, 5).context("mount SD")?;
        let part = EspDefaultNvsPartition::take()?;
        let mut s = Storage::new(EspNvs::new(part.clone(), "tracker", true)?);
        let recovered = s.restore();
        if !recovered {
            warn!("recovery failed");
        }
        s.diagnostic(if recovered {
            "[BOOT] delivery recovery complete\n"
        } else {
            "[BOOT] delivery recovery failed; new sessions disabled\n"
        });
        let s = Arc::new(Mutex::new(s));
        let loop_ = EspSystemEventLoop::take()?;
        let wifi = BlockingWifi::wrap(EspWifi::new(p.modem, loop_.clone(), Some(part))?, loop_)?;
        let bg = Arc::clone(&s);
        thread::Builder::new()
            .name("track-upload".into())
            .stack_size(8192)
            .spawn(move || uploader(bg, wifi))?;
        let uart = UartDriver::new(
            p.uart1,
            pins.gpio17,
            pins.gpio16,
            Option::<gpio::Gpio0>::None,
            Option::<gpio::Gpio1>::None,
            &UartConfig::new().baudrate(Hertz(9600)),
        )?;
        let mut n = Nmea::default();
        let mut line = Vec::with_capacity(128);
        let mut flow = TrackingWorkflow::new(ID);
        let start = Instant::now();
        let mut sample = Instant::now();
        let mut previous = None;
        let mut points = 0u32;
        let mut missed = 0u32;
        let mut health = Instant::now();
        loop {
            let mut b = [0u8; 64];
            if let Ok(c) = uart.read(&mut b, 10) {
                for x in &b[..c] {
                    if *x == b'\n' {
                        if let Ok(v) = std::str::from_utf8(&line) {
                            let _ = n.parse(v.trim());
                        }
                        line.clear();
                    } else if *x != b'\r' && line.len() < 160 {
                        line.push(*x);
                    }
                }
            }
            if sample.elapsed() >= Duration::from_secs(1) {
                sample = Instant::now();
                let current = n.fix_time;
                let fresh = current.is_some() && current != previous;
                if fresh {
                    previous = current
                } else {
                    missed = missed.wrapping_add(1)
                }
                let f = fix(&n, fresh, start.elapsed().as_millis() as u32);
                if let Ok(mut st) = s.lock() {
                    let d = flow.process_fix(&f, &mut *st);
                    if let Some(p) = d.point {
                        if d.raw_point_recorded {
                            points = points.wrapping_add(1);
                            if d.write_filtered_csv {
                                st.exports(&p)
                            }
                        } else if d.raw_point_write_failed {
                            st.diagnostic(&format!(
                                "[SD] Raw point {} append/flush FAILED\n",
                                p.point_number
                            ));
                        }
                    }
                }
            }
            if flow.tracking_session_active() && health.elapsed() >= Duration::from_secs(60) {
                health = Instant::now();
                if let Ok(mut st) = s.lock() {
                    st.diagnostic(&format!(
                        "[HEALTH] uptime_ms={} raw_points={} no_fresh_location={}\n",
                        start.elapsed().as_millis(),
                        points,
                        missed
                    ));
                }
            }
        }
    }
}
