package main

import (
	"context"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestRunRequiresConfig(t *testing.T) {
	if err := run(context.Background(), nil); err == nil {
		t.Fatal("missing config accepted")
	}
	if err := run(context.Background(), []string{"--unknown"}); err == nil {
		t.Fatal("unknown argument accepted")
	}
}

func TestRunLoadsConfigAndShutsDown(t *testing.T) {
	root, err := os.MkdirTemp("/tmp", "box3-ota-command-")
	if err != nil {
		t.Fatal(err)
	}
	defer os.RemoveAll(root)
	stateDir := filepath.Join(root, "state")
	runtimeDir := filepath.Join(root, "run")
	tokenPath := filepath.Join(root, "token")
	configPath := filepath.Join(root, "config.json")
	if err := os.WriteFile(tokenPath, []byte("0123456789abcdef0123456789abcdef"), 0o400); err != nil {
		t.Fatal(err)
	}
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	address := listener.Addr().String()
	if err := listener.Close(); err != nil {
		t.Fatal(err)
	}
	data := fmt.Sprintf(`{
		"schema_version":1,
		"state_dir":%q,
		"database_path":%q,
		"runtime_dir":%q,
		"device_socket_path":%q,
		"admin_listen":%q,
		"container_only_allow_wildcard":false,
		"admin_token_file":%q,
		"admin_allowed_hosts":["localhost"],
		"admin_allowed_origins":[],
		"read_header_timeout":"1s",
		"read_timeout":"1s",
		"idle_timeout":"1s",
		"shutdown_timeout":"5s",
		"sqlite_busy_timeout":"1s"
	}`, stateDir, filepath.Join(stateDir, "ota.db"), runtimeDir, filepath.Join(runtimeDir, "device.sock"), address, tokenPath)
	if err := os.WriteFile(configPath, []byte(data), 0o600); err != nil {
		t.Fatal(err)
	}
	wildcardPath := filepath.Join(root, "wildcard.json")
	wildcard := strings.ReplaceAll(data, "127.0.0.1:", "0.0.0.0:")
	wildcard = strings.ReplaceAll(wildcard, `"container_only_allow_wildcard":false`, `"container_only_allow_wildcard":true`)
	if err := os.WriteFile(wildcardPath, []byte(wildcard), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := run(context.Background(), []string{"--config", wildcardPath, "--require-loopback"}); err == nil || !strings.Contains(err.Error(), "forbids wildcard") {
		t.Fatalf("host-network wildcard not rejected: %v", err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- run(ctx, []string{"--config", configPath, "--require-loopback"}) }()
	deadline := time.Now().Add(5 * time.Second)
	for {
		if _, err := os.Lstat(filepath.Join(runtimeDir, "device.sock")); err == nil {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("socket did not appear")
		}
		time.Sleep(10 * time.Millisecond)
	}
	if err := run(context.Background(), []string{"--config", configPath, "--check-ready"}); err != nil {
		t.Fatalf("readiness command: %v", err)
	}
	cancel()
	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("run: %v", err)
		}
	case <-time.After(6 * time.Second):
		t.Fatal("shutdown exceeded bound")
	}
	if err := checkReady(context.Background(), filepath.Join(runtimeDir, "device.sock")); err == nil {
		t.Fatal("readiness succeeded after shutdown")
	}
}
