CREATE TABLE devices (
    id TEXT PRIMARY KEY,
    certificate_identity TEXT NOT NULL UNIQUE,
    display_name TEXT NOT NULL,
    target TEXT NOT NULL,
    last_seen_at TEXT,
    current_version TEXT,
    current_image_hash TEXT,
    boot_id TEXT,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL
) STRICT;

CREATE TABLE jobs (
    id TEXT PRIMARY KEY,
    device_id TEXT NOT NULL REFERENCES devices(id),
    target_ref TEXT NOT NULL,
    requested_by TEXT NOT NULL,
    request_id TEXT NOT NULL UNIQUE,
    state TEXT NOT NULL CHECK (state IN ('queued','offered','accepted','downloading','downloaded','verified','pending_reboot','boot_testing','confirmed','rolled_back','failed','cancelled')),
    progress_bytes INTEGER NOT NULL DEFAULT 0 CHECK (progress_bytes >= 0),
    expected_boot_id TEXT,
    expected_image_hash TEXT,
    last_error_code TEXT,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL
) STRICT;

CREATE UNIQUE INDEX one_active_job_per_device ON jobs(device_id)
WHERE state IN ('queued','offered','accepted','downloading','downloaded','verified','pending_reboot','boot_testing');

CREATE TABLE job_events (
    id INTEGER PRIMARY KEY,
    job_id TEXT NOT NULL REFERENCES jobs(id),
    device_id TEXT NOT NULL REFERENCES devices(id),
    boot_id TEXT NOT NULL,
    event_sequence INTEGER NOT NULL CHECK (event_sequence >= 0),
    stage TEXT NOT NULL,
    byte_count INTEGER NOT NULL CHECK (byte_count >= 0),
    image_hash TEXT,
    error_code TEXT,
    received_at TEXT NOT NULL,
    UNIQUE(job_id, boot_id, event_sequence)
) STRICT;

CREATE INDEX job_events_job_order ON job_events(job_id, id);
CREATE INDEX jobs_created_order ON jobs(created_at DESC, id DESC);
