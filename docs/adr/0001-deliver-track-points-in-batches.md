---
status: accepted
---

# Deliver track points in batches

The tracker can lose power without warning and its phone hotspot may drop, so the SD card remains the local record while the firmware sends ordered, 30-point NDJSON batches to a general HTTPS upload service. Stable tracker, session, and point numbers make retries safe, and the upload service confirms the highest stored point before the firmware marks data as delivered. This replaces direct uploads of growing CSV or GPX files and keeps R2, databases, maps, and statistics outside the firmware repo.
