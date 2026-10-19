// Package model defines the durable OTA foundation domain values.
package model

import "time"

// JobState is a durable lifecycle state. It does not imply hardware success
// unless it is Confirmed and the store has validated boot and image evidence.
type JobState string

const (
	StateQueued        JobState = "queued"
	StateOffered       JobState = "offered"
	StateAccepted      JobState = "accepted"
	StateDownloading   JobState = "downloading"
	StateDownloaded    JobState = "downloaded"
	StateVerified      JobState = "verified"
	StatePendingReboot JobState = "pending_reboot"
	StateBootTesting   JobState = "boot_testing"
	StateConfirmed     JobState = "confirmed"
	StateRolledBack    JobState = "rolled_back"
	StateFailed        JobState = "failed"
	StateCancelled     JobState = "cancelled"
)

// Device is the persisted identity and last reported boot metadata.
type Device struct {
	ID, CertificateIdentity, DisplayName, Target string
	LastSeenAt                                   *time.Time
	CurrentVersion, CurrentImageHash, BootID     string
	CreatedAt, UpdatedAt                         time.Time
}

// Job is an immutable assignment plus its current durable lifecycle state.
type Job struct {
	ID, DeviceID, TargetRef, RequestedBy, RequestID  string
	State                                            JobState
	ProgressBytes                                    int64
	ExpectedBootID, ExpectedImageHash, LastErrorCode string
	CreatedAt, UpdatedAt                             time.Time
}

// Event is immutable device-reported progress. ImageHash is required when
// confirming a boot and otherwise records reported image evidence.
type Event struct {
	ID         int64
	JobID      string
	DeviceID   string
	BootID     string
	Sequence   int64
	Stage      JobState
	ByteCount  int64
	ImageHash  string
	ErrorCode  string
	ReceivedAt time.Time
}

// CreateJobParams contains immutable assignment inputs.
type CreateJobParams struct {
	ID, DeviceID, TargetRef, RequestedBy, RequestID string
	ExpectedBootID, ExpectedImageHash               string
}

// AppendEventParams contains a normalized event and requested state transition.
type AppendEventParams struct {
	JobID, DeviceID, BootID, ImageHash, ErrorCode string
	Sequence, ByteCount                           int64
	Stage                                         JobState
}
