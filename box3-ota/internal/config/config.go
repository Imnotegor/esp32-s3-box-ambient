// Package config loads and validates the explicit service configuration.
package config

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/url"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"
	"unicode"
)

// Duration is a JSON duration string.
type Duration struct{ time.Duration }

// UnmarshalJSON parses a non-empty time.Duration string.
func (d *Duration) UnmarshalJSON(data []byte) error {
	var value string
	if err := json.Unmarshal(data, &value); err != nil {
		return fmt.Errorf("duration must be a string: %w", err)
	}
	parsed, err := time.ParseDuration(value)
	if err != nil {
		return fmt.Errorf("parse duration: %w", err)
	}
	d.Duration = parsed
	return nil
}

// Config is the complete, default-free runtime contract.
type Config struct {
	SchemaVersion              int      `json:"schema_version"`
	StateDir                   string   `json:"state_dir"`
	DatabasePath               string   `json:"database_path"`
	RuntimeDir                 string   `json:"runtime_dir"`
	DeviceSocketPath           string   `json:"device_socket_path"`
	AdminListen                string   `json:"admin_listen"`
	ContainerOnlyAllowWildcard bool     `json:"container_only_allow_wildcard"`
	AdminTokenFile             string   `json:"admin_token_file"`
	AdminAllowedHosts          []string `json:"admin_allowed_hosts"`
	AdminAllowedOrigins        []string `json:"admin_allowed_origins"`
	ReadHeaderTimeout          Duration `json:"read_header_timeout"`
	ReadTimeout                Duration `json:"read_timeout"`
	IdleTimeout                Duration `json:"idle_timeout"`
	ShutdownTimeout            Duration `json:"shutdown_timeout"`
	SQLiteBusyTimeout          Duration `json:"sqlite_busy_timeout"`
}

// Load reads one strict JSON document and validates paths and secret modes.
func Load(path string) (Config, error) {
	var cfg Config
	if !filepath.IsAbs(path) {
		return cfg, errors.New("config path must be absolute")
	}
	info, err := os.Lstat(path)
	if err != nil {
		return cfg, fmt.Errorf("stat config: %w", err)
	}
	if !info.Mode().IsRegular() || info.Mode().Perm()&0o137 != 0 {
		return cfg, errors.New("config must be a non-executable regular file with mode 0640 or tighter")
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return cfg, fmt.Errorf("read config: %w", err)
	}
	dec := json.NewDecoder(bytes.NewReader(data))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&cfg); err != nil {
		return cfg, fmt.Errorf("decode config: %w", err)
	}
	if err := dec.Decode(&struct{}{}); !errors.Is(err, io.EOF) {
		if err == nil {
			return cfg, errors.New("config contains multiple JSON values")
		}
		return cfg, fmt.Errorf("decode trailing config data: %w", err)
	}
	if err := cfg.Validate(); err != nil {
		return cfg, err
	}
	if err := validateTokenFile(cfg.AdminTokenFile); err != nil {
		return cfg, err
	}
	return cfg, nil
}

// Validate checks all non-filesystem configuration invariants.
func (c Config) Validate() error {
	if c.SchemaVersion != 1 {
		return fmt.Errorf("unsupported config schema_version %d", c.SchemaVersion)
	}
	for name, value := range map[string]string{
		"state_dir": c.StateDir, "database_path": c.DatabasePath,
		"runtime_dir": c.RuntimeDir, "device_socket_path": c.DeviceSocketPath,
		"admin_token_file": c.AdminTokenFile,
	} {
		if !filepath.IsAbs(value) || filepath.Clean(value) != value {
			return fmt.Errorf("%s must be a clean absolute path", name)
		}
	}
	if !within(c.StateDir, c.DatabasePath) || filepath.Dir(c.DatabasePath) != c.StateDir {
		return errors.New("database_path must be directly inside state_dir")
	}
	if !within(c.RuntimeDir, c.DeviceSocketPath) || filepath.Dir(c.DeviceSocketPath) != c.RuntimeDir {
		return errors.New("device_socket_path must be directly inside runtime_dir")
	}
	host, port, err := net.SplitHostPort(c.AdminListen)
	if err != nil || net.ParseIP(host) == nil {
		return errors.New("admin_listen must contain a literal IP and port")
	}
	number, err := strconv.Atoi(port)
	if err != nil || number < 1 || number > 65535 || strconv.Itoa(number) != port {
		return errors.New("admin_listen port must be a decimal integer from 1 to 65535")
	}
	ip := net.ParseIP(host)
	if ip.IsUnspecified() {
		if !c.ContainerOnlyAllowWildcard {
			return errors.New("wildcard admin listen requires container_only_allow_wildcard")
		}
	} else if !ip.IsLoopback() {
		return errors.New("admin_listen must be loopback or an explicitly allowed container wildcard")
	}
	if len(c.AdminAllowedHosts) == 0 {
		return errors.New("admin_allowed_hosts must not be empty")
	}
	seenHosts := make(map[string]struct{}, len(c.AdminAllowedHosts))
	for _, host := range c.AdminAllowedHosts {
		if strings.TrimSpace(host) != host || host == "" || host == "*" || strings.ContainsAny(host, "/\\") {
			return errors.New("admin_allowed_hosts contains an invalid exact host")
		}
		if _, exists := seenHosts[host]; exists {
			return errors.New("admin_allowed_hosts contains a duplicate")
		}
		seenHosts[host] = struct{}{}
	}
	for _, raw := range c.AdminAllowedOrigins {
		u, parseErr := url.Parse(raw)
		if parseErr != nil || u.Scheme == "" || u.Host == "" || u.Path != "" || u.RawQuery != "" || u.Fragment != "" || u.User != nil {
			return errors.New("admin_allowed_origins must contain exact origins")
		}
	}
	if err := bounded("read_header_timeout", c.ReadHeaderTimeout.Duration, time.Second, 30*time.Second); err != nil {
		return err
	}
	if err := bounded("read_timeout", c.ReadTimeout.Duration, time.Second, 2*time.Minute); err != nil {
		return err
	}
	if err := bounded("idle_timeout", c.IdleTimeout.Duration, time.Second, 10*time.Minute); err != nil {
		return err
	}
	if err := bounded("shutdown_timeout", c.ShutdownTimeout.Duration, time.Second, time.Minute); err != nil {
		return err
	}
	if err := bounded("sqlite_busy_timeout", c.SQLiteBusyTimeout.Duration, 100*time.Millisecond, 30*time.Second); err != nil {
		return err
	}
	return nil
}

func bounded(name string, value, min, max time.Duration) error {
	if value < min || value > max {
		return fmt.Errorf("%s must be between %s and %s", name, min, max)
	}
	return nil
}

func within(parent, child string) bool {
	rel, err := filepath.Rel(parent, child)
	return err == nil && rel != "." && rel != ".." && !strings.HasPrefix(rel, ".."+string(filepath.Separator))
}

func validateTokenFile(path string) error {
	info, err := os.Lstat(path)
	if err != nil {
		return fmt.Errorf("stat admin token: %w", err)
	}
	mode := info.Mode()
	if !mode.IsRegular() || mode.Perm()&0o337 != 0 || mode.Perm()&0o040 == 0 && mode.Perm()&0o400 == 0 {
		return errors.New("admin token must be a regular, non-writable owner/group-readable file with no other access")
	}
	if info.Size() > 1024 {
		return errors.New("admin token file is too large")
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return fmt.Errorf("read admin token: %w", err)
	}
	token := strings.TrimSpace(string(data))
	if len(token) < 32 || len(token) > 256 || strings.IndexFunc(token, unicode.IsSpace) >= 0 {
		return errors.New("admin token must contain 32 to 256 non-whitespace bytes")
	}
	return nil
}
