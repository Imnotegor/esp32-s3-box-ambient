package config

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func validConfig(root string) Config {
	return Config{SchemaVersion: 1, StateDir: filepath.Join(root, "state"), DatabasePath: filepath.Join(root, "state", "ota.db"), RuntimeDir: filepath.Join(root, "run"), DeviceSocketPath: filepath.Join(root, "run", "device.sock"), AdminListen: "127.0.0.1:8080", AdminTokenFile: filepath.Join(root, "token"), AdminAllowedHosts: []string{"localhost:8080"}, AdminAllowedOrigins: []string{"http://localhost:8080"}, ReadHeaderTimeout: Duration{5 * time.Second}, ReadTimeout: Duration{10 * time.Second}, IdleTimeout: Duration{time.Minute}, ShutdownTimeout: Duration{10 * time.Second}, SQLiteBusyTimeout: Duration{time.Second}}
}
func TestValidate(t *testing.T) {
	root := t.TempDir()
	tests := []struct {
		name   string
		mutate func(*Config)
	}{
		{"valid", func(*Config) {}}, {"zero port", func(c *Config) { c.AdminListen = "127.0.0.1:0" }}, {"named port", func(c *Config) { c.AdminListen = "127.0.0.1:http" }}, {"large port", func(c *Config) { c.AdminListen = "127.0.0.1:65536" }}, {"wildcard", func(c *Config) { c.AdminListen = "0.0.0.0:8080" }}, {"non-loopback", func(c *Config) { c.AdminListen = "192.0.2.1:8080" }}, {"relative path", func(c *Config) { c.StateDir = "state" }}, {"escaping database", func(c *Config) { c.DatabasePath = filepath.Join(root, "other", "ota.db") }}, {"empty hosts", func(c *Config) { c.AdminAllowedHosts = nil }}, {"wildcard host", func(c *Config) { c.AdminAllowedHosts = []string{"*"} }}, {"invalid timeout", func(c *Config) { c.ReadTimeout = Duration{time.Millisecond} }}, {"future schema", func(c *Config) { c.SchemaVersion = 2 }}}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			cfg := validConfig(root)
			tt.mutate(&cfg)
			err := cfg.Validate()
			if tt.name == "valid" && err != nil {
				t.Fatalf("valid config: %v", err)
			}
			if tt.name != "valid" && err == nil {
				t.Fatal("expected validation failure")
			}
		})
	}
}
func TestExplicitContainerWildcard(t *testing.T) {
	cfg := validConfig(t.TempDir())
	cfg.AdminListen = "0.0.0.0:8080"
	cfg.ContainerOnlyAllowWildcard = true
	if err := cfg.Validate(); err != nil {
		t.Fatalf("explicit wildcard: %v", err)
	}
}
func TestLoadStrictAndSecretModes(t *testing.T) {
	root := t.TempDir()
	token := filepath.Join(root, "token")
	if err := os.WriteFile(token, []byte(strings.Repeat("x", 32)), 0o440); err != nil {
		t.Fatal(err)
	}
	base := `{"schema_version":1,"state_dir":"ROOT/state","database_path":"ROOT/state/ota.db","runtime_dir":"ROOT/run","device_socket_path":"ROOT/run/device.sock","admin_listen":"127.0.0.1:8080","container_only_allow_wildcard":false,"admin_token_file":"ROOT/token","admin_allowed_hosts":["localhost:8080"],"admin_allowed_origins":[],"read_header_timeout":"5s","read_timeout":"10s","idle_timeout":"1m","shutdown_timeout":"10s","sqlite_busy_timeout":"1s"}`
	base = strings.ReplaceAll(base, "ROOT", root)
	for _, tt := range []struct {
		name, data            string
		configMode, tokenMode os.FileMode
		wantErr               bool
	}{{"valid", base, 0o640, 0o440, false}, {"unknown", strings.TrimSuffix(base, "}") + `,"extra":true}`, 0o640, 0o440, true}, {"trailing", base + ` garbage`, 0o640, 0o440, true}, {"permissive config", base, 0o644, 0o440, true}, {"permissive token", base, 0o640, 0o444, true}} {
		t.Run(tt.name, func(t *testing.T) {
			if err := os.Chmod(token, tt.tokenMode); err != nil {
				t.Fatal(err)
			}
			path := filepath.Join(root, "config-"+strings.ReplaceAll(tt.name, " ", "-")+".json")
			if err := os.WriteFile(path, []byte(tt.data), tt.configMode); err != nil {
				t.Fatal(err)
			}
			_, err := Load(path)
			if (err != nil) != tt.wantErr {
				t.Fatalf("Load error=%v, wantErr=%v", err, tt.wantErr)
			}
		})
	}
}

func TestLoadRejectsEmptyToken(t *testing.T) {
	root := t.TempDir()
	token := filepath.Join(root, "token")
	if err := os.WriteFile(token, nil, 0o400); err != nil {
		t.Fatal(err)
	}
	data := `{"schema_version":1,"state_dir":"ROOT/state","database_path":"ROOT/state/ota.db","runtime_dir":"ROOT/run","device_socket_path":"ROOT/run/device.sock","admin_listen":"127.0.0.1:8080","container_only_allow_wildcard":false,"admin_token_file":"ROOT/token","admin_allowed_hosts":["localhost:8080"],"admin_allowed_origins":[],"read_header_timeout":"5s","read_timeout":"10s","idle_timeout":"1m","shutdown_timeout":"10s","sqlite_busy_timeout":"1s"}`
	data = strings.ReplaceAll(data, "ROOT", root)
	path := filepath.Join(root, "config.json")
	if err := os.WriteFile(path, []byte(data), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := Load(path); err == nil {
		t.Fatal("empty token accepted")
	}
}
