package admin

import (
	"context"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/egormikhaylov/esp32-s3-box-ambient/box3-ota/internal/health"
	"github.com/egormikhaylov/esp32-s3-box-ambient/box3-ota/internal/model"
)

const testToken = "0123456789abcdef0123456789abcdef"

type fakeStore struct{}

func (fakeStore) ListDevices(context.Context, int, int) ([]model.Device, error) {
	return []model.Device{{ID: "box-1", DisplayName: "Kitchen", Target: "esp32-s3"}}, nil
}
func (fakeStore) ListTargetRefs(context.Context, int) ([]string, error) {
	return []string{"opaque-ref"}, nil
}
func (fakeStore) Job(context.Context, string) (model.Job, error) {
	return model.Job{ID: "job-1", DeviceID: "box-1", TargetRef: "opaque-ref", State: model.StateQueued}, nil
}
func (fakeStore) JobEvents(context.Context, string, int) ([]model.Event, error) { return nil, nil }

type readyDB struct{}

func (readyDB) Ready(context.Context) error { return nil }
func adminHandler(t *testing.T) http.Handler {
	t.Helper()
	root := t.TempDir()
	token := filepath.Join(root, "token")
	if err := os.WriteFile(token, []byte(testToken+"\n"), 0o400); err != nil {
		t.Fatal(err)
	}
	checker := health.New(readyDB{}, root, root)
	checker.SetAccepting(true)
	server, err := NewServer(fakeStore{}, checker, Options{Listen: "127.0.0.1:8080", AllowedHosts: []string{"localhost:8080"}, AllowedOrigins: []string{"http://localhost:8080"}, TokenFile: token})
	if err != nil {
		t.Fatal(err)
	}
	return server.Handler
}
func request(t *testing.T, h http.Handler, method, path, token, host, origin string) *httptest.ResponseRecorder {
	t.Helper()
	req := httptest.NewRequest(method, path, nil)
	req.Host = host
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	if origin != "" {
		req.Header.Set("Origin", origin)
	}
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	return rec
}
func TestAdminTrustBoundaries(t *testing.T) {
	h := adminHandler(t)
	tests := []struct {
		name, token, host, origin string
		want                      int
	}{{"missing auth", "", "localhost:8080", "", 401}, {"wrong auth", "wrong", "localhost:8080", "", 401}, {"unsafe host", testToken, "evil.example", "", 403}, {"unsafe origin", testToken, "localhost:8080", "http://evil.example", 403}, {"valid", testToken, "localhost:8080", "http://localhost:8080", 200}}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			rec := request(t, h, http.MethodGet, "/admin/devices", tt.token, tt.host, tt.origin)
			if rec.Code != tt.want {
				t.Fatalf("status=%d body=%q", rec.Code, rec.Body.String())
			}
			for _, header := range []string{"Cache-Control", "Content-Security-Policy", "Referrer-Policy", "X-Content-Type-Options", "X-Frame-Options"} {
				if rec.Header().Get(header) == "" {
					t.Errorf("missing %s", header)
				}
			}
			if strings.Contains(rec.Body.String(), testToken) {
				t.Fatal("token rendered")
			}
		})
	}
}
func TestAdminReadOnlyAndPagination(t *testing.T) {
	h := adminHandler(t)
	if got := request(t, h, http.MethodPost, "/admin/devices", testToken, "localhost:8080", "").Code; got != http.StatusMethodNotAllowed {
		t.Fatalf("POST status=%d", got)
	}
	if got := request(t, h, http.MethodGet, "/admin/devices?limit=101", testToken, "localhost:8080", "").Code; got != http.StatusBadRequest {
		t.Fatalf("pagination status=%d", got)
	}
	if got := request(t, h, http.MethodGet, "/admin/releases", testToken, "localhost:8080", ""); got.Code != http.StatusOK || !strings.Contains(got.Body.String(), "not an imported release catalog") {
		t.Fatalf("releases status=%d body=%q", got.Code, got.Body.String())
	}
	if got := request(t, h, http.MethodGet, "/admin/jobs/job-1", testToken, "localhost:8080", "").Code; got != http.StatusOK {
		t.Fatalf("job status=%d", got)
	}
}
func TestAdminRejectsRequestBody(t *testing.T) {
	h := adminHandler(t)
	req := httptest.NewRequest(http.MethodGet, "/admin/devices", strings.NewReader(strings.Repeat("x", 2048)))
	req.Host = "localhost:8080"
	req.Header.Set("Authorization", "Bearer "+testToken)
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusRequestEntityTooLarge {
		t.Fatalf("status=%d", rec.Code)
	}
}

func TestAdminRejectsForwardedIdentity(t *testing.T) {
	h := adminHandler(t)
	req := httptest.NewRequest(http.MethodGet, "/admin/devices", nil)
	req.Host = "localhost:8080"
	req.Header.Set("Authorization", "Bearer "+testToken)
	req.Header.Set("X-Forwarded-For", "127.0.0.1")
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusForbidden {
		t.Fatalf("status=%d", rec.Code)
	}
}
