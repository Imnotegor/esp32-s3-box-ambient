package deviceapi

import (
	"context"
	"errors"
	"io"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/egormikhaylov/esp32-s3-box-ambient/box3-ota/internal/health"
)

type readyDB struct{}

func (readyDB) Ready(context.Context) error { return nil }
func TestUnixListenerAndFailClosedRoutes(t *testing.T) {
	root, err := os.MkdirTemp("/tmp", "box3-ota-socket-")
	if err != nil {
		t.Fatal(err)
	}
	defer os.RemoveAll(root)
	runtimeDir := filepath.Join(root, "run")
	socket := filepath.Join(runtimeDir, "device.sock")
	listener, err := Listen(runtimeDir, socket)
	if err != nil {
		t.Fatal(err)
	}
	checker := health.New(readyDB{}, runtimeDir, runtimeDir)
	checker.SetAccepting(true)
	server := NewServer(checker, time.Second, time.Second, time.Second)
	done := make(chan error, 1)
	go func() { done <- server.Serve(listener) }()
	transport := &http.Transport{DialContext: func(ctx context.Context, _, _ string) (net.Conn, error) {
		return (&net.Dialer{}).DialContext(ctx, "unix", socket)
	}}
	client := &http.Client{Transport: transport, Timeout: time.Second}
	defer transport.CloseIdleConnections()
	info, err := os.Lstat(socket)
	if err != nil {
		t.Fatal(err)
	}
	if info.Mode()&os.ModeSocket == 0 || info.Mode().Perm() != 0o660 {
		t.Fatalf("socket mode=%v", info.Mode())
	}
	for _, tt := range []struct {
		method, path string
		body         io.Reader
		want         int
	}{{http.MethodGet, "/health/live", nil, 200}, {http.MethodGet, "/v1/foundation/status", nil, 503}, {http.MethodPost, "/v1/foundation/status", nil, 405}, {http.MethodGet, "/undeclared", nil, 404}, {http.MethodGet, "/health/live", strings.NewReader(strings.Repeat("x", 2048)), 413}} {
		req, err := http.NewRequest(tt.method, "http://unix"+tt.path, tt.body)
		if err != nil {
			t.Fatal(err)
		}
		req.Header.Set("X-Device-Identity", "untrusted")
		resp, err := client.Do(req)
		if err != nil {
			t.Fatalf("%s %s: %v", tt.method, tt.path, err)
		}
		_ = resp.Body.Close()
		if resp.StatusCode != tt.want {
			t.Errorf("%s %s=%d want %d", tt.method, tt.path, resp.StatusCode, tt.want)
		}
	}
	if err := server.Shutdown(context.Background()); err != nil {
		t.Fatal(err)
	}
	<-done
	if err := listener.Cleanup(); err != nil && !os.IsNotExist(err) && !errors.Is(err, net.ErrClosed) {
		t.Fatalf("cleanup: %v", err)
	}
	if _, err := os.Lstat(socket); !os.IsNotExist(err) {
		t.Fatalf("socket remains: %v", err)
	}
}
func TestRefusesActiveSocket(t *testing.T) {
	root, err := os.MkdirTemp("/tmp", "box3-ota-active-")
	if err != nil {
		t.Fatal(err)
	}
	defer os.RemoveAll(root)
	runtimeDir := filepath.Join(root, "run")
	socket := filepath.Join(runtimeDir, "device.sock")
	listener, err := Listen(runtimeDir, socket)
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Cleanup()
	if _, err := Listen(runtimeDir, socket); err == nil {
		t.Fatal("active socket was replaced")
	}
}

func TestRefusesUnsafeExistingPath(t *testing.T) {
	runtimeDir := filepath.Join(t.TempDir(), "run")
	if err := os.Mkdir(runtimeDir, 0o750); err != nil {
		t.Fatal(err)
	}
	socket := filepath.Join(runtimeDir, "device.sock")
	if err := os.WriteFile(socket, []byte("do not unlink"), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := Listen(runtimeDir, socket); err == nil {
		t.Fatal("unsafe existing file accepted")
	}
	data, err := os.ReadFile(socket)
	if err != nil || string(data) != "do not unlink" {
		t.Fatalf("file changed: %q err=%v", data, err)
	}
}

func TestRefusesPermissiveRuntimeDirectory(t *testing.T) {
	root, err := os.MkdirTemp("/tmp", "box3-ota-mode-")
	if err != nil {
		t.Fatal(err)
	}
	defer os.RemoveAll(root)
	runtimeDir := filepath.Join(root, "run")
	if err := os.Mkdir(runtimeDir, 0o770); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(runtimeDir, 0o770); err != nil {
		t.Fatal(err)
	}
	if _, err := Listen(runtimeDir, filepath.Join(runtimeDir, "device.sock")); err == nil {
		t.Fatal("permissive runtime directory accepted")
	}
}
