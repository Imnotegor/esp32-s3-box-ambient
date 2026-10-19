package store

import (
	"context"
	"path/filepath"
	"testing"

	"github.com/egormikhaylov/esp32-s3-box-ambient/box3-ota/internal/model"
)

func pendingTestJob(t *testing.T, s *Store) {
	t.Helper()
	addDevice(t, s, "box-1")
	if _, err := s.CreateJob(context.Background(), params("job-1", "box-1", "req-1")); err != nil {
		t.Fatal(err)
	}
	for i, stage := range []model.JobState{model.StateOffered, model.StateAccepted, model.StateDownloading, model.StateDownloaded, model.StateVerified, model.StatePendingReboot} {
		if _, _, err := s.AppendEvent(context.Background(), model.AppendEventParams{JobID: "job-1", DeviceID: "box-1", BootID: "boot-old", Sequence: int64(i + 1), Stage: stage}); err != nil {
			t.Fatal(err)
		}
	}
}

func TestBootTestingEvidence(t *testing.T) {
	for _, tt := range []struct {
		name, testingBoot, confirmingBoot string
		rejectTesting, rejectConfirmation bool
	}{
		{"old boot cannot test", "boot-old", "boot-new", true, false},
		{"confirmation must match tested boot", "boot-new", "boot-other", false, true},
		{"same tested boot can confirm", "boot-new", "boot-new", false, false},
	} {
		t.Run(tt.name, func(t *testing.T) {
			s := openTestStore(t, filepath.Join(t.TempDir(), "ota.db"))
			pendingTestJob(t, s)
			_, _, err := s.AppendEvent(context.Background(), model.AppendEventParams{JobID: "job-1", DeviceID: "box-1", BootID: tt.testingBoot, Sequence: 7, Stage: model.StateBootTesting, ImageHash: hashA})
			if tt.rejectTesting {
				if err == nil {
					t.Fatal("old boot accepted for testing")
				}
				return
			}
			if err != nil {
				t.Fatal(err)
			}
			_, _, err = s.AppendEvent(context.Background(), model.AppendEventParams{JobID: "job-1", DeviceID: "box-1", BootID: tt.confirmingBoot, Sequence: 8, Stage: model.StateConfirmed, ImageHash: hashA})
			if (err != nil) != tt.rejectConfirmation {
				t.Fatalf("confirmation err=%v want rejection=%v", err, tt.rejectConfirmation)
			}
		})
	}
}

func TestEventSequenceMonotonicity(t *testing.T) {
	s := openTestStore(t, filepath.Join(t.TempDir(), "ota.db"))
	addDevice(t, s, "box-1")
	if _, err := s.CreateJob(context.Background(), params("job-1", "box-1", "req-1")); err != nil {
		t.Fatal(err)
	}
	first := model.AppendEventParams{JobID: "job-1", DeviceID: "box-1", BootID: "boot-old", Sequence: 2, Stage: model.StateOffered}
	if _, _, err := s.AppendEvent(context.Background(), first); err != nil {
		t.Fatal(err)
	}
	if _, dup, err := s.AppendEvent(context.Background(), first); err != nil || !dup {
		t.Fatalf("exact replay: duplicate=%v err=%v", dup, err)
	}
	older := first
	older.Sequence = 1
	older.Stage = model.StateAccepted
	if _, _, err := s.AppendEvent(context.Background(), older); err == nil {
		t.Fatal("out-of-order sequence accepted")
	}
	got, err := s.Job(context.Background(), "job-1")
	if err != nil || got.State != model.StateOffered {
		t.Fatalf("failed event changed job: %+v %v", got, err)
	}
}
