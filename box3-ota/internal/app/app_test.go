package app

import (
	"context"
	"errors"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/egormikhaylov/esp32-s3-box-ambient/box3-ota/internal/config"
	"github.com/egormikhaylov/esp32-s3-box-ambient/box3-ota/internal/store"
)

func TestRunGracefulShutdownAndRecovery(t *testing.T) {
	root, err := os.MkdirTemp("/tmp", "box3-ota-app-")
	if err != nil {
		t.Fatal(err)
	}
	defer os.RemoveAll(root)
	stateDir := filepath.Join(root, "state")
	runtimeDir := filepath.Join(root, "run")
	token := filepath.Join(root, "token")
	if err := os.WriteFile(token, []byte("0123456789abcdef0123456789abcdef"), 0o400); err != nil {
		t.Fatal(err)
	}
	cfg := config.Config{SchemaVersion: 1, StateDir: stateDir, DatabasePath: filepath.Join(stateDir, "ota.db"), RuntimeDir: runtimeDir, DeviceSocketPath: filepath.Join(runtimeDir, "device.sock"), AdminListen: "127.0.0.1:0", AdminTokenFile: token, AdminAllowedHosts: []string{"localhost"}, ReadHeaderTimeout: config.Duration{Duration: time.Second}, ReadTimeout: config.Duration{Duration: time.Second}, IdleTimeout: config.Duration{Duration: time.Second}, ShutdownTimeout: config.Duration{Duration: 5 * time.Second}, SQLiteBusyTimeout: config.Duration{Duration: time.Second}}
	// Run is wired directly here to request an OS-assigned port; production
	// configuration loading rejects ephemeral ports.
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- Run(ctx, cfg) }()
	deadline := time.Now().Add(5 * time.Second)
	for {
		if _, err := os.Lstat(cfg.DeviceSocketPath); err == nil {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("socket did not appear")
		}
		time.Sleep(10 * time.Millisecond)
	}
	transport := &http.Transport{DialContext: func(ctx context.Context, _, _ string) (net.Conn, error) {
		return (&net.Dialer{}).DialContext(ctx, "unix", cfg.DeviceSocketPath)
	}}
	client := &http.Client{Transport: transport, Timeout: time.Second}
	resp, err := client.Get("http://unix/health/ready")
	if err != nil {
		t.Fatal(err)
	}
	_ = resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("ready status=%d", resp.StatusCode)
	}
	transport.CloseIdleConnections()
	cancel()
	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("Run: %v", err)
		}
	case <-time.After(6 * time.Second):
		t.Fatal("shutdown exceeded bound")
	}
	if _, err := os.Lstat(cfg.DeviceSocketPath); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("socket remains: %v", err)
	}
	reopened, err := store.Open(context.Background(), cfg.DatabasePath, time.Second)
	if err != nil {
		t.Fatalf("reopen database: %v", err)
	}
	if err := reopened.Close(); err != nil {
		t.Fatal(err)
	}
}
func TestEnsureDirectoryRejectsPermissiveMode(t *testing.T) {
	path := filepath.Join(t.TempDir(), "state")
	if err := os.Mkdir(path, 0o777); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(path, 0o777); err != nil {
		t.Fatal(err)
	}
	if err := ensureDirectory(path, 0o750); err == nil {
		t.Fatal("permissive directory accepted")
	}
}
