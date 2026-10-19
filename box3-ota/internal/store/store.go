// Package store owns the SQLite lifecycle and transactional OTA state core.
package store

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"net/url"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	_ "modernc.org/sqlite"

	"github.com/egormikhaylov/esp32-s3-box-ambient/box3-ota/migrations"
)

const migrationCount = 1

// Store is the single-process owner of the service database.
type Store struct{ db *sql.DB }

// Open creates or opens a database, verifies durability pragmas, and migrates it.
func Open(ctx context.Context, path string, busyTimeout time.Duration) (*Store, error) {
	if !filepath.IsAbs(path) {
		return nil, errors.New("database path must be absolute")
	}
	if busyTimeout < 100*time.Millisecond || busyTimeout > 30*time.Second {
		return nil, errors.New("busy timeout is out of range")
	}
	dsn := "file:" + url.PathEscape(path) + "?_pragma=foreign_keys(1)&_pragma=journal_mode(WAL)&_pragma=synchronous(FULL)&_pragma=busy_timeout(" + strconv.FormatInt(busyTimeout.Milliseconds(), 10) + ")"
	db, err := sql.Open("sqlite", dsn)
	if err != nil {
		return nil, fmt.Errorf("open sqlite: %w", err)
	}
	db.SetMaxOpenConns(1)
	db.SetMaxIdleConns(1)
	s := &Store{db: db}
	if err := s.verifyPragmas(ctx, busyTimeout); err != nil {
		_ = db.Close()
		return nil, err
	}
	if err := s.migrate(ctx); err != nil {
		_ = db.Close()
		return nil, err
	}
	return s, nil
}

func (s *Store) verifyPragmas(ctx context.Context, busyTimeout time.Duration) error {
	var foreignKeys int
	if err := s.db.QueryRowContext(ctx, "PRAGMA foreign_keys").Scan(&foreignKeys); err != nil {
		return fmt.Errorf("query foreign_keys pragma: %w", err)
	}
	if foreignKeys != 1 {
		return errors.New("sqlite foreign key enforcement is disabled")
	}
	var journal string
	if err := s.db.QueryRowContext(ctx, "PRAGMA journal_mode").Scan(&journal); err != nil {
		return fmt.Errorf("query journal mode: %w", err)
	}
	if !strings.EqualFold(journal, "wal") {
		return fmt.Errorf("sqlite journal mode is %q, want WAL", journal)
	}
	var synchronous int
	if err := s.db.QueryRowContext(ctx, "PRAGMA synchronous").Scan(&synchronous); err != nil {
		return fmt.Errorf("query synchronous pragma: %w", err)
	}
	if synchronous != 2 {
		return fmt.Errorf("sqlite synchronous mode is %d, want FULL", synchronous)
	}
	var configuredBusyTimeout int64
	if err := s.db.QueryRowContext(ctx, "PRAGMA busy_timeout").Scan(&configuredBusyTimeout); err != nil {
		return fmt.Errorf("query busy_timeout pragma: %w", err)
	}
	if configuredBusyTimeout != busyTimeout.Milliseconds() {
		return fmt.Errorf("sqlite busy timeout is %dms, want %dms", configuredBusyTimeout, busyTimeout.Milliseconds())
	}
	return nil
}

func (s *Store) migrate(ctx context.Context) error {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return fmt.Errorf("begin migration: %w", err)
	}
	defer func() { _ = tx.Rollback() }()
	if _, err := tx.ExecContext(ctx, `CREATE TABLE IF NOT EXISTS schema_migrations (version INTEGER PRIMARY KEY, applied_at TEXT NOT NULL) STRICT`); err != nil {
		return fmt.Errorf("create migration table: %w", err)
	}
	var current int
	if err := tx.QueryRowContext(ctx, `SELECT COALESCE(MAX(version), 0) FROM schema_migrations`).Scan(&current); err != nil {
		return fmt.Errorf("read migration version: %w", err)
	}
	if current > migrationCount {
		return fmt.Errorf("database schema version %d is newer than supported version %d", current, migrationCount)
	}
	for version := current + 1; version <= migrationCount; version++ {
		name := fmt.Sprintf("%04d_foundation.sql", version)
		script, readErr := migrations.Files.ReadFile(name)
		if readErr != nil {
			return fmt.Errorf("read migration %d: %w", version, readErr)
		}
		if _, execErr := tx.ExecContext(ctx, string(script)); execErr != nil {
			return fmt.Errorf("apply migration %d: %w", version, execErr)
		}
		if _, execErr := tx.ExecContext(ctx, `INSERT INTO schema_migrations(version, applied_at) VALUES (?, ?)`, version, nowText()); execErr != nil {
			return fmt.Errorf("record migration %d: %w", version, execErr)
		}
	}
	if err := tx.Commit(); err != nil {
		return fmt.Errorf("commit migrations: %w", err)
	}
	return nil
}

// Close closes SQLite after all users and listeners have stopped.
func (s *Store) Close() error { return s.db.Close() }

// Ready checks that SQLite can execute a bounded trivial query.
func (s *Store) Ready(ctx context.Context) error {
	var one int
	if err := s.db.QueryRowContext(ctx, `SELECT 1`).Scan(&one); err != nil {
		return fmt.Errorf("database readiness: %w", err)
	}
	if one != 1 {
		return errors.New("database readiness returned an invalid result")
	}
	return nil
}

func nowText() string { return time.Now().UTC().Format(time.RFC3339Nano) }
