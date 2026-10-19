package store

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"regexp"
	"strings"
	"time"

	"github.com/egormikhaylov/esp32-s3-box-ambient/box3-ota/internal/model"
)

var (
	// ErrNotFound means the requested durable record does not exist.
	ErrNotFound = errors.New("record not found")
	// ErrConflict means an idempotency key or active assignment conflicts.
	ErrConflict = errors.New("conflict")
	// ErrReplayConflict means an event key was reused with different payload.
	ErrReplayConflict = errors.New("conflicting event replay")
	// ErrInvalidTransition means the lifecycle transition is not allowed.
	ErrInvalidTransition = errors.New("invalid job state transition")
	identifierPattern    = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$`)
	hashPattern          = regexp.MustCompile(`^[a-f0-9]{64}$`)
)

var transitions = map[model.JobState]map[model.JobState]bool{
	model.StateQueued:        {model.StateOffered: true, model.StateCancelled: true},
	model.StateOffered:       {model.StateAccepted: true, model.StateCancelled: true},
	model.StateAccepted:      {model.StateDownloading: true, model.StateCancelled: true, model.StateFailed: true},
	model.StateDownloading:   {model.StateDownloading: true, model.StateDownloaded: true, model.StateCancelled: true, model.StateFailed: true},
	model.StateDownloaded:    {model.StateVerified: true, model.StateFailed: true},
	model.StateVerified:      {model.StatePendingReboot: true, model.StateFailed: true},
	model.StatePendingReboot: {model.StateBootTesting: true, model.StateFailed: true},
	model.StateBootTesting:   {model.StateConfirmed: true, model.StateRolledBack: true, model.StateFailed: true},
	model.StateConfirmed:     {}, model.StateRolledBack: {}, model.StateFailed: {}, model.StateCancelled: {},
}

// TransitionAllowed reports whether a state transition is in the explicit lifecycle.
func TransitionAllowed(from, to model.JobState) bool { return transitions[from][to] }

// AddDevice inserts a normalized device identity for foundation tests and future provisioning.
// It is deliberately not exposed over HTTP.
func (s *Store) AddDevice(ctx context.Context, d model.Device) (model.Device, error) {
	if err := validateID("device id", d.ID); err != nil {
		return model.Device{}, err
	}
	if err := validateText("certificate identity", d.CertificateIdentity, 512); err != nil {
		return model.Device{}, err
	}
	if err := validateText("display name", d.DisplayName, 128); err != nil {
		return model.Device{}, err
	}
	if err := validateID("target", d.Target); err != nil {
		return model.Device{}, err
	}
	now := nowText()
	_, err := s.db.ExecContext(ctx, `INSERT INTO devices(id, certificate_identity, display_name, target, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?)`, d.ID, d.CertificateIdentity, d.DisplayName, d.Target, now, now)
	if err != nil {
		return model.Device{}, fmt.Errorf("insert device: %w", err)
	}
	return s.Device(ctx, d.ID)
}

// Device returns one device by its normalized ID.
func (s *Store) Device(ctx context.Context, id string) (model.Device, error) {
	if err := validateID("device id", id); err != nil {
		return model.Device{}, err
	}
	row := s.db.QueryRowContext(ctx, `SELECT id, certificate_identity, display_name, target, last_seen_at, COALESCE(current_version,''), COALESCE(current_image_hash,''), COALESCE(boot_id,''), created_at, updated_at FROM devices WHERE id=?`, id)
	return scanDevice(row)
}

// ListDevices returns at most limit devices in deterministic ID order.
func (s *Store) ListDevices(ctx context.Context, limit, offset int) ([]model.Device, error) {
	if limit < 1 || limit > 100 || offset < 0 || offset > 100000 {
		return nil, errors.New("invalid pagination")
	}
	rows, err := s.db.QueryContext(ctx, `SELECT id, certificate_identity, display_name, target, last_seen_at, COALESCE(current_version,''), COALESCE(current_image_hash,''), COALESCE(boot_id,''), created_at, updated_at FROM devices ORDER BY id LIMIT ? OFFSET ?`, limit, offset)
	if err != nil {
		return nil, fmt.Errorf("list devices: %w", err)
	}
	defer rows.Close()
	var out []model.Device
	for rows.Next() {
		d, scanErr := scanDevice(rows)
		if scanErr != nil {
			return nil, scanErr
		}
		out = append(out, d)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("iterate devices: %w", err)
	}
	return out, nil
}

type scanner interface{ Scan(...any) error }

func scanDevice(row scanner) (model.Device, error) {
	var d model.Device
	var last sql.NullString
	var created, updated string
	if err := row.Scan(&d.ID, &d.CertificateIdentity, &d.DisplayName, &d.Target, &last, &d.CurrentVersion, &d.CurrentImageHash, &d.BootID, &created, &updated); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return d, ErrNotFound
		}
		return d, fmt.Errorf("scan device: %w", err)
	}
	var err error
	d.CreatedAt, err = parseTime(created)
	if err != nil {
		return d, err
	}
	d.UpdatedAt, err = parseTime(updated)
	if err != nil {
		return d, err
	}
	if last.Valid {
		parsed, parseErr := parseTime(last.String)
		if parseErr != nil {
			return d, parseErr
		}
		d.LastSeenAt = &parsed
	}
	return d, nil
}

// CreateJob transactionally and idempotently creates one active job per device.
func (s *Store) CreateJob(ctx context.Context, p model.CreateJobParams) (model.Job, error) {
	for name, value := range map[string]string{"job id": p.ID, "device id": p.DeviceID, "target ref": p.TargetRef, "requested by": p.RequestedBy, "request id": p.RequestID, "expected boot id": p.ExpectedBootID} {
		if err := validateID(name, value); err != nil {
			return model.Job{}, err
		}
	}
	if !hashPattern.MatchString(p.ExpectedImageHash) {
		return model.Job{}, errors.New("expected image hash must be 64 lowercase hexadecimal characters")
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return model.Job{}, fmt.Errorf("begin create job: %w", err)
	}
	defer func() { _ = tx.Rollback() }()
	existing, findErr := jobByRequestID(ctx, tx, p.RequestID)
	if findErr == nil {
		if existing.ID == p.ID && existing.DeviceID == p.DeviceID && existing.TargetRef == p.TargetRef && existing.RequestedBy == p.RequestedBy && existing.ExpectedBootID == p.ExpectedBootID && existing.ExpectedImageHash == p.ExpectedImageHash {
			return existing, nil
		}
		return model.Job{}, fmt.Errorf("request_id immutable inputs differ: %w", ErrConflict)
	}
	if !errors.Is(findErr, ErrNotFound) {
		return model.Job{}, findErr
	}
	now := nowText()
	_, err = tx.ExecContext(ctx, `INSERT INTO jobs(id,device_id,target_ref,requested_by,request_id,state,expected_boot_id,expected_image_hash,created_at,updated_at) VALUES(?,?,?,?,?,'queued',?,?,?,?)`, p.ID, p.DeviceID, p.TargetRef, p.RequestedBy, p.RequestID, p.ExpectedBootID, p.ExpectedImageHash, now, now)
	if err != nil {
		if strings.Contains(err.Error(), "UNIQUE constraint failed") {
			return model.Job{}, fmt.Errorf("active job or idempotency key already exists: %w", ErrConflict)
		}
		return model.Job{}, fmt.Errorf("insert job: %w", err)
	}
	job, err := jobByID(ctx, tx, p.ID)
	if err != nil {
		return model.Job{}, err
	}
	if err := tx.Commit(); err != nil {
		return model.Job{}, fmt.Errorf("commit create job: %w", err)
	}
	return job, nil
}

// AppendEvent validates replay, transition, and confirmation evidence in one transaction.
func (s *Store) AppendEvent(ctx context.Context, p model.AppendEventParams) (model.Job, bool, error) {
	for name, value := range map[string]string{"job id": p.JobID, "device id": p.DeviceID, "boot id": p.BootID} {
		if err := validateID(name, value); err != nil {
			return model.Job{}, false, err
		}
	}
	if p.Sequence < 0 || p.ByteCount < 0 {
		return model.Job{}, false, errors.New("event counters must be non-negative")
	}
	if _, ok := transitions[p.Stage]; !ok {
		return model.Job{}, false, errors.New("unknown event stage")
	}
	if p.ImageHash != "" && !hashPattern.MatchString(p.ImageHash) {
		return model.Job{}, false, errors.New("image hash must be 64 lowercase hexadecimal characters")
	}
	if p.ErrorCode != "" {
		if err := validateID("error code", p.ErrorCode); err != nil {
			return model.Job{}, false, err
		}
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return model.Job{}, false, fmt.Errorf("begin append event: %w", err)
	}
	defer func() { _ = tx.Rollback() }()
	var deviceID, stage, imageHash, errorCode string
	var byteCount int64
	err = tx.QueryRowContext(ctx, `SELECT device_id,stage,byte_count,COALESCE(image_hash,''),COALESCE(error_code,'') FROM job_events WHERE job_id=? AND boot_id=? AND event_sequence=?`, p.JobID, p.BootID, p.Sequence).Scan(&deviceID, &stage, &byteCount, &imageHash, &errorCode)
	if err == nil {
		if deviceID != p.DeviceID || stage != string(p.Stage) || byteCount != p.ByteCount || imageHash != p.ImageHash || errorCode != p.ErrorCode {
			return model.Job{}, false, ErrReplayConflict
		}
		job, getErr := jobByID(ctx, tx, p.JobID)
		return job, true, getErr
	}
	if !errors.Is(err, sql.ErrNoRows) {
		return model.Job{}, false, fmt.Errorf("check event replay: %w", err)
	}
	var maxSequence sql.NullInt64
	if err := tx.QueryRowContext(ctx, `SELECT MAX(event_sequence) FROM job_events WHERE job_id=? AND boot_id=?`, p.JobID, p.BootID).Scan(&maxSequence); err != nil {
		return model.Job{}, false, fmt.Errorf("read last event sequence: %w", err)
	}
	if maxSequence.Valid && p.Sequence <= maxSequence.Int64 {
		return model.Job{}, false, errors.New("event sequence must increase within a boot")
	}
	job, err := jobByID(ctx, tx, p.JobID)
	if err != nil {
		return model.Job{}, false, err
	}
	if job.DeviceID != p.DeviceID {
		return model.Job{}, false, fmt.Errorf("event device does not own job: %w", ErrConflict)
	}
	if !TransitionAllowed(job.State, p.Stage) {
		return model.Job{}, false, fmt.Errorf("%s to %s: %w", job.State, p.Stage, ErrInvalidTransition)
	}
	if p.Stage == model.StateDownloading && p.ByteCount < job.ProgressBytes {
		return model.Job{}, false, errors.New("download progress cannot decrease")
	}
	if p.Stage == model.StateBootTesting {
		if p.BootID == job.ExpectedBootID || p.ImageHash != job.ExpectedImageHash {
			return model.Job{}, false, errors.New("boot testing requires a new boot and the expected image hash")
		}
	}
	if p.Stage == model.StateConfirmed {
		var testedBoot string
		if err := tx.QueryRowContext(ctx, `SELECT boot_id FROM job_events WHERE job_id=? AND stage='boot_testing' ORDER BY id DESC LIMIT 1`, p.JobID).Scan(&testedBoot); err != nil {
			return model.Job{}, false, fmt.Errorf("read tested boot identity: %w", err)
		}
		if p.BootID != testedBoot {
			return model.Job{}, false, errors.New("confirmation must identify the tested boot")
		}
		if p.ImageHash != job.ExpectedImageHash {
			return model.Job{}, false, errors.New("confirmation image hash does not match expected image")
		}
		if p.BootID == job.ExpectedBootID {
			return model.Job{}, false, errors.New("confirmation requires a new boot identity")
		}
	}
	now := nowText()
	if _, err = tx.ExecContext(ctx, `INSERT INTO job_events(job_id,device_id,boot_id,event_sequence,stage,byte_count,image_hash,error_code,received_at) VALUES(?,?,?,?,?,?,?,?,?)`, p.JobID, p.DeviceID, p.BootID, p.Sequence, p.Stage, p.ByteCount, nullString(p.ImageHash), nullString(p.ErrorCode), now); err != nil {
		return model.Job{}, false, fmt.Errorf("insert event: %w", err)
	}
	progress := job.ProgressBytes
	if p.ByteCount > progress {
		progress = p.ByteCount
	}
	lastError := job.LastErrorCode
	if p.ErrorCode != "" {
		lastError = p.ErrorCode
	}
	if _, err = tx.ExecContext(ctx, `UPDATE jobs SET state=?,progress_bytes=?,last_error_code=?,updated_at=? WHERE id=?`, p.Stage, progress, nullString(lastError), now, p.JobID); err != nil {
		return model.Job{}, false, fmt.Errorf("update job: %w", err)
	}
	if p.Stage == model.StateConfirmed {
		if _, err = tx.ExecContext(ctx, `UPDATE devices SET current_image_hash=?,boot_id=?,last_seen_at=?,updated_at=? WHERE id=?`, p.ImageHash, p.BootID, now, now, p.DeviceID); err != nil {
			return model.Job{}, false, fmt.Errorf("update confirmed device: %w", err)
		}
	}
	job, err = jobByID(ctx, tx, p.JobID)
	if err != nil {
		return model.Job{}, false, err
	}
	if err = tx.Commit(); err != nil {
		return model.Job{}, false, fmt.Errorf("commit event: %w", err)
	}
	return job, false, nil
}

// Job returns one job by ID.
func (s *Store) Job(ctx context.Context, id string) (model.Job, error) {
	if err := validateID("job id", id); err != nil {
		return model.Job{}, err
	}
	return jobByID(ctx, s.db, id)
}

// JobEvents returns an ordered, bounded event history.
func (s *Store) JobEvents(ctx context.Context, jobID string, limit int) ([]model.Event, error) {
	if err := validateID("job id", jobID); err != nil {
		return nil, err
	}
	if limit < 1 || limit > 100 {
		return nil, errors.New("invalid event limit")
	}
	rows, err := s.db.QueryContext(ctx, `SELECT id,job_id,device_id,boot_id,event_sequence,stage,byte_count,COALESCE(image_hash,''),COALESCE(error_code,''),received_at FROM job_events WHERE job_id=? ORDER BY id LIMIT ?`, jobID, limit)
	if err != nil {
		return nil, fmt.Errorf("list events: %w", err)
	}
	defer rows.Close()
	var out []model.Event
	for rows.Next() {
		var e model.Event
		var received string
		if err := rows.Scan(&e.ID, &e.JobID, &e.DeviceID, &e.BootID, &e.Sequence, &e.Stage, &e.ByteCount, &e.ImageHash, &e.ErrorCode, &received); err != nil {
			return nil, fmt.Errorf("scan event: %w", err)
		}
		e.ReceivedAt, err = parseTime(received)
		if err != nil {
			return nil, err
		}
		out = append(out, e)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("iterate events: %w", err)
	}
	return out, nil
}

// ListTargetRefs returns bounded assignment references observed in jobs. They are
// not asserted to be imported, signed, compatible, or installable releases.
func (s *Store) ListTargetRefs(ctx context.Context, limit int) ([]string, error) {
	if limit < 1 || limit > 100 {
		return nil, errors.New("invalid target ref limit")
	}
	rows, err := s.db.QueryContext(ctx, `SELECT DISTINCT target_ref FROM jobs ORDER BY target_ref LIMIT ?`, limit)
	if err != nil {
		return nil, fmt.Errorf("list target refs: %w", err)
	}
	defer rows.Close()
	var out []string
	for rows.Next() {
		var ref string
		if err := rows.Scan(&ref); err != nil {
			return nil, fmt.Errorf("scan target ref: %w", err)
		}
		out = append(out, ref)
	}
	return out, rows.Err()
}

func jobByRequestID(ctx context.Context, q interface {
	QueryRowContext(context.Context, string, ...any) *sql.Row
}, id string) (model.Job, error) {
	return scanJob(q.QueryRowContext(ctx, jobSelect+` WHERE request_id=?`, id))
}
func jobByID(ctx context.Context, q interface {
	QueryRowContext(context.Context, string, ...any) *sql.Row
}, id string) (model.Job, error) {
	return scanJob(q.QueryRowContext(ctx, jobSelect+` WHERE id=?`, id))
}

const jobSelect = `SELECT id,device_id,target_ref,requested_by,request_id,state,progress_bytes,COALESCE(expected_boot_id,''),COALESCE(expected_image_hash,''),COALESCE(last_error_code,''),created_at,updated_at FROM jobs`

func scanJob(row scanner) (model.Job, error) {
	var j model.Job
	var created, updated string
	if err := row.Scan(&j.ID, &j.DeviceID, &j.TargetRef, &j.RequestedBy, &j.RequestID, &j.State, &j.ProgressBytes, &j.ExpectedBootID, &j.ExpectedImageHash, &j.LastErrorCode, &created, &updated); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return j, ErrNotFound
		}
		return j, fmt.Errorf("scan job: %w", err)
	}
	var err error
	j.CreatedAt, err = parseTime(created)
	if err != nil {
		return j, err
	}
	j.UpdatedAt, err = parseTime(updated)
	return j, err
}
func parseTime(value string) (time.Time, error) {
	parsed, err := time.Parse(time.RFC3339Nano, value)
	if err != nil {
		return time.Time{}, fmt.Errorf("parse stored timestamp: %w", err)
	}
	return parsed, nil
}
func validateID(name, value string) error {
	if !identifierPattern.MatchString(value) {
		return fmt.Errorf("%s is not normalized or is too long", name)
	}
	return nil
}
func validateText(name, value string, max int) error {
	if value == "" || strings.TrimSpace(value) != value || len(value) > max || strings.ContainsAny(value, "\x00\r\n") {
		return fmt.Errorf("%s is empty, unnormalized, or too long", name)
	}
	return nil
}
func nullString(value string) any {
	if value == "" {
		return nil
	}
	return value
}
