package store

import (
	"context"
	"errors"
	"path/filepath"
	"sync"
	"testing"
	"time"

	"github.com/egormikhaylov/esp32-s3-box-ambient/box3-ota/internal/model"
)

const hashA = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"

func openTestStore(t *testing.T, path string) *Store {
	t.Helper()
	s, err := Open(context.Background(), path, time.Second)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	t.Cleanup(func() { _ = s.Close() })
	return s
}
func addDevice(t *testing.T, s *Store, id string) {
	t.Helper()
	_, err := s.AddDevice(context.Background(), model.Device{ID: id, CertificateIdentity: "cert:" + id, DisplayName: "Device " + id, Target: "esp32-s3"})
	if err != nil {
		t.Fatalf("AddDevice: %v", err)
	}
}
func params(id, device, request string) model.CreateJobParams {
	return model.CreateJobParams{ID: id, DeviceID: device, TargetRef: "release-1", RequestedBy: "owner", RequestID: request, ExpectedBootID: "boot-old", ExpectedImageHash: hashA}
}
func TestMigrationRestartRecovery(t *testing.T) {
	path := filepath.Join(t.TempDir(), "ota.db")
	s := openTestStore(t, path)
	addDevice(t, s, "box-1")
	job, err := s.CreateJob(context.Background(), params("job-1", "box-1", "request-1"))
	if err != nil {
		t.Fatal(err)
	}
	if _, _, err = s.AppendEvent(context.Background(), model.AppendEventParams{JobID: job.ID, DeviceID: "box-1", BootID: "boot-old", Sequence: 1, Stage: model.StateOffered}); err != nil {
		t.Fatal(err)
	}
	if err = s.Close(); err != nil {
		t.Fatal(err)
	}
	s2, err := Open(context.Background(), path, time.Second)
	if err != nil {
		t.Fatalf("reopen: %v", err)
	}
	defer s2.Close()
	got, err := s2.Job(context.Background(), job.ID)
	if err != nil || got.State != model.StateOffered {
		t.Fatalf("recovered job=%+v err=%v", got, err)
	}
	events, err := s2.JobEvents(context.Background(), job.ID, 10)
	if err != nil || len(events) != 1 || events[0].Sequence != 1 {
		t.Fatalf("recovered events=%+v err=%v", events, err)
	}
	if err = s2.migrate(context.Background()); err != nil {
		t.Fatalf("idempotent migrate: %v", err)
	}
}
func TestFutureMigrationRejected(t *testing.T) {
	path := filepath.Join(t.TempDir(), "ota.db")
	s := openTestStore(t, path)
	if _, err := s.db.Exec(`INSERT INTO schema_migrations(version,applied_at) VALUES(99,?)`, nowText()); err != nil {
		t.Fatal(err)
	}
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	if _, err := Open(context.Background(), path, time.Second); err == nil {
		t.Fatal("expected future schema rejection")
	}
}
func TestCreateJobIdempotencyAndSingleActive(t *testing.T) {
	s := openTestStore(t, filepath.Join(t.TempDir(), "ota.db"))
	addDevice(t, s, "box-1")
	addDevice(t, s, "box-2")
	ctx := context.Background()
	first, err := s.CreateJob(ctx, params("job-1", "box-1", "request-1"))
	if err != nil {
		t.Fatal(err)
	}
	retry, err := s.CreateJob(ctx, params("job-1", "box-1", "request-1"))
	if err != nil || retry.ID != first.ID {
		t.Fatalf("exact retry=%+v err=%v", retry, err)
	}
	changed := params("job-1", "box-1", "request-1")
	changed.TargetRef = "release-2"
	if _, err = s.CreateJob(ctx, changed); !errors.Is(err, ErrConflict) {
		t.Fatalf("changed retry err=%v", err)
	}
	start := make(chan struct{})
	var wg sync.WaitGroup
	errs := make(chan error, 2)
	for i, p := range []model.CreateJobParams{params("job-a", "box-2", "request-a"), params("job-b", "box-2", "request-b")} {
		_ = i
		wg.Add(1)
		go func(p model.CreateJobParams) { defer wg.Done(); <-start; _, err := s.CreateJob(ctx, p); errs <- err }(p)
	}
	close(start)
	wg.Wait()
	close(errs)
	success, conflict := 0, 0
	for err := range errs {
		if err == nil {
			success++
		} else if errors.Is(err, ErrConflict) {
			conflict++
		} else {
			t.Fatalf("unexpected concurrent error: %v", err)
		}
	}
	if success != 1 || conflict != 1 {
		t.Fatalf("success=%d conflict=%d", success, conflict)
	}
}
func TestEventReplayAndConfirmationEvidence(t *testing.T) {
	s := openTestStore(t, filepath.Join(t.TempDir(), "ota.db"))
	addDevice(t, s, "box-1")
	job, err := s.CreateJob(context.Background(), params("job-1", "box-1", "request-1"))
	if err != nil {
		t.Fatal(err)
	}
	seq := int64(1)
	advance := func(state model.JobState, boot, image string) error {
		_, _, err := s.AppendEvent(context.Background(), model.AppendEventParams{JobID: job.ID, DeviceID: "box-1", BootID: boot, Sequence: seq, Stage: state, ByteCount: seq, ImageHash: image})
		seq++
		return err
	}
	for _, state := range []model.JobState{model.StateOffered, model.StateAccepted, model.StateDownloading, model.StateDownloaded, model.StateVerified, model.StatePendingReboot} {
		if err := advance(state, "boot-old", ""); err != nil {
			t.Fatalf("advance %s: %v", state, err)
		}
	}
	if err := advance(model.StateBootTesting, "boot-new", hashA); err != nil {
		t.Fatalf("boot testing: %v", err)
	}
	bad := model.AppendEventParams{JobID: job.ID, DeviceID: "box-1", BootID: "boot-new", Sequence: seq, Stage: model.StateConfirmed, ByteCount: seq, ImageHash: stringsOf('b', 64)}
	if _, _, err := s.AppendEvent(context.Background(), bad); err == nil {
		t.Fatal("mismatched confirmation accepted")
	}
	bad.ImageHash = hashA
	bad.BootID = "boot-old"
	if _, _, err := s.AppendEvent(context.Background(), bad); err == nil {
		t.Fatal("same-boot confirmation accepted")
	}
	bad.BootID = "boot-new"
	confirmed, duplicate, err := s.AppendEvent(context.Background(), bad)
	if err != nil || duplicate || confirmed.State != model.StateConfirmed {
		t.Fatalf("confirmation=%+v duplicate=%v err=%v", confirmed, duplicate, err)
	}
	again, duplicate, err := s.AppendEvent(context.Background(), bad)
	if err != nil || !duplicate || again.State != model.StateConfirmed {
		t.Fatalf("duplicate=%+v duplicate=%v err=%v", again, duplicate, err)
	}
	bad.ByteCount++
	if _, _, err := s.AppendEvent(context.Background(), bad); !errors.Is(err, ErrReplayConflict) {
		t.Fatalf("conflicting replay err=%v", err)
	}
	events, err := s.JobEvents(context.Background(), job.ID, 100)
	if err != nil || len(events) != 8 {
		t.Fatalf("events=%d err=%v", len(events), err)
	}
	device, err := s.Device(context.Background(), "box-1")
	if err != nil || device.BootID != "boot-new" || device.CurrentImageHash != hashA {
		t.Fatalf("device=%+v err=%v", device, err)
	}
}
func TestTransitionTable(t *testing.T) {
	states := []model.JobState{model.StateQueued, model.StateOffered, model.StateAccepted, model.StateDownloading, model.StateDownloaded, model.StateVerified, model.StatePendingReboot, model.StateBootTesting, model.StateConfirmed, model.StateRolledBack, model.StateFailed, model.StateCancelled}
	allowed := map[[2]model.JobState]bool{{model.StateQueued, model.StateOffered}: true, {model.StateQueued, model.StateCancelled}: true, {model.StateOffered, model.StateAccepted}: true, {model.StateOffered, model.StateCancelled}: true, {model.StateAccepted, model.StateDownloading}: true, {model.StateAccepted, model.StateCancelled}: true, {model.StateAccepted, model.StateFailed}: true, {model.StateDownloading, model.StateDownloading}: true, {model.StateDownloading, model.StateDownloaded}: true, {model.StateDownloading, model.StateCancelled}: true, {model.StateDownloading, model.StateFailed}: true, {model.StateDownloaded, model.StateVerified}: true, {model.StateDownloaded, model.StateFailed}: true, {model.StateVerified, model.StatePendingReboot}: true, {model.StateVerified, model.StateFailed}: true, {model.StatePendingReboot, model.StateBootTesting}: true, {model.StatePendingReboot, model.StateFailed}: true, {model.StateBootTesting, model.StateConfirmed}: true, {model.StateBootTesting, model.StateRolledBack}: true, {model.StateBootTesting, model.StateFailed}: true}
	for _, from := range states {
		for _, to := range states {
			want := allowed[[2]model.JobState{from, to}]
			if got := TransitionAllowed(from, to); got != want {
				t.Errorf("TransitionAllowed(%s,%s)=%v want %v", from, to, got, want)
			}
		}
	}
}
func TestConstraints(t *testing.T) {
	s := openTestStore(t, filepath.Join(t.TempDir(), "ota.db"))
	addDevice(t, s, "box-1")
	if _, err := s.AddDevice(context.Background(), model.Device{ID: "box-2", CertificateIdentity: "cert:box-1", DisplayName: "duplicate", Target: "esp32-s3"}); err == nil {
		t.Fatal("duplicate certificate accepted")
	}
	if _, err := s.CreateJob(context.Background(), params("job-x", "missing", "req-x")); err == nil {
		t.Fatal("missing foreign key accepted")
	}
	p := params(stringsOf('x', 129), "box-1", "req-y")
	if _, err := s.CreateJob(context.Background(), p); err == nil {
		t.Fatal("oversized id accepted")
	}
}
func stringsOf(r rune, n int) string {
	v := make([]rune, n)
	for i := range v {
		v[i] = r
	}
	return string(v)
}

func TestReadQueriesAndReadiness(t *testing.T) {
	s := openTestStore(t, filepath.Join(t.TempDir(), "ota.db"))
	addDevice(t, s, "box-2")
	addDevice(t, s, "box-1")
	if err := s.Ready(context.Background()); err != nil {
		t.Fatal(err)
	}
	devices, err := s.ListDevices(context.Background(), 1, 0)
	if err != nil || len(devices) != 1 || devices[0].ID != "box-1" {
		t.Fatalf("devices=%+v err=%v", devices, err)
	}
	if _, err := s.ListDevices(context.Background(), 101, 0); err == nil {
		t.Fatal("unbounded device list accepted")
	}
	if _, err := s.CreateJob(context.Background(), params("job-1", "box-1", "request-1")); err != nil {
		t.Fatal(err)
	}
	refs, err := s.ListTargetRefs(context.Background(), 10)
	if err != nil || len(refs) != 1 || refs[0] != "release-1" {
		t.Fatalf("refs=%v err=%v", refs, err)
	}
	if _, err := s.ListTargetRefs(context.Background(), 0); err == nil {
		t.Fatal("unbounded target ref list accepted")
	}
	if _, err := s.Device(context.Background(), "missing"); !errors.Is(err, ErrNotFound) {
		t.Fatalf("missing device err=%v", err)
	}
	if _, err := s.Job(context.Background(), "missing"); !errors.Is(err, ErrNotFound) {
		t.Fatalf("missing job err=%v", err)
	}
}

func TestEventValidationAndProgress(t *testing.T) {
	s := openTestStore(t, filepath.Join(t.TempDir(), "ota.db"))
	addDevice(t, s, "box-1")
	job, err := s.CreateJob(context.Background(), params("job-1", "box-1", "request-1"))
	if err != nil {
		t.Fatal(err)
	}
	if _, _, err := s.AppendEvent(context.Background(), model.AppendEventParams{JobID: job.ID, DeviceID: "box-1", BootID: "boot-old", Sequence: -1, Stage: model.StateOffered}); err == nil {
		t.Fatal("negative sequence accepted")
	}
	if _, _, err := s.AppendEvent(context.Background(), model.AppendEventParams{JobID: job.ID, DeviceID: "box-1", BootID: "boot-old", Sequence: 1, Stage: model.StateConfirmed, ImageHash: hashA}); !errors.Is(err, ErrInvalidTransition) {
		t.Fatalf("direct confirmation err=%v", err)
	}
	steps := []model.AppendEventParams{
		{JobID: job.ID, DeviceID: "box-1", BootID: "boot-old", Sequence: 1, Stage: model.StateOffered},
		{JobID: job.ID, DeviceID: "box-1", BootID: "boot-old", Sequence: 2, Stage: model.StateAccepted},
		{JobID: job.ID, DeviceID: "box-1", BootID: "boot-old", Sequence: 3, Stage: model.StateDownloading, ByteCount: 100},
	}
	for _, step := range steps {
		if _, _, err := s.AppendEvent(context.Background(), step); err != nil {
			t.Fatal(err)
		}
	}
	if _, _, err := s.AppendEvent(context.Background(), model.AppendEventParams{JobID: job.ID, DeviceID: "box-1", BootID: "boot-old", Sequence: 4, Stage: model.StateDownloading, ByteCount: 99}); err == nil {
		t.Fatal("decreasing progress accepted")
	}
}
