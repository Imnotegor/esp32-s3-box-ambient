package health

import (
	"context"
	"errors"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"testing"
)

type fakeDB struct{ err error }

func (f fakeDB) Ready(context.Context) error { return f.err }
func TestHealthSemantics(t *testing.T) {
	root := t.TempDir()
	checker := New(fakeDB{}, root, root)
	req := httptest.NewRequest(http.MethodGet, "/health/live", nil)
	rec := httptest.NewRecorder()
	checker.Live(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("live=%d", rec.Code)
	}
	rec = httptest.NewRecorder()
	checker.Ready(rec, req)
	if rec.Code != http.StatusServiceUnavailable {
		t.Fatalf("not accepting ready=%d", rec.Code)
	}
	checker.SetAccepting(true)
	rec = httptest.NewRecorder()
	checker.Ready(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("ready=%d body=%q", rec.Code, rec.Body.String())
	}
	checker.SetAccepting(false)
	rec = httptest.NewRecorder()
	checker.Ready(rec, req)
	if rec.Code != http.StatusServiceUnavailable {
		t.Fatalf("draining ready=%d", rec.Code)
	}
}
func TestHealthDependencyFailureDoesNotLeak(t *testing.T) {
	root := t.TempDir()
	checker := New(fakeDB{err: errors.New("secret database path")}, root, filepath.Join(root, "missing-private-path"))
	checker.SetAccepting(true)
	rec := httptest.NewRecorder()
	checker.Ready(rec, httptest.NewRequest(http.MethodGet, "/health/ready", nil))
	if rec.Code != http.StatusServiceUnavailable || rec.Body.String() != "not ready\n" {
		t.Fatalf("response=%d %q", rec.Code, rec.Body.String())
	}
}

func TestHealthMethodRestrictions(t *testing.T) {
	checker := New(fakeDB{}, t.TempDir(), t.TempDir())
	checker.SetAccepting(true)
	for name, handler := range map[string]http.HandlerFunc{"live": checker.Live, "ready": checker.Ready} {
		t.Run(name, func(t *testing.T) {
			rec := httptest.NewRecorder()
			handler(rec, httptest.NewRequest(http.MethodPost, "/health/"+name, nil))
			if rec.Code != http.StatusMethodNotAllowed || rec.Header().Get("Allow") != http.MethodGet {
				t.Fatalf("status=%d allow=%q", rec.Code, rec.Header().Get("Allow"))
			}
		})
	}
}
