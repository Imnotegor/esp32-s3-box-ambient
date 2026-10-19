// Package health provides deliberately narrow process and dependency health.
package health

import (
	"context"
	"errors"
	"net/http"
	"os"
	"sync/atomic"
	"time"
)

// Database is the bounded database readiness contract.
type Database interface{ Ready(context.Context) error }

// Checker distinguishes process liveness from service readiness.
type Checker struct {
	db                   Database
	stateDir, runtimeDir string
	accepting            atomic.Bool
}

// New creates a checker in the not-ready state.
func New(db Database, stateDir, runtimeDir string) *Checker {
	return &Checker{db: db, stateDir: stateDir, runtimeDir: runtimeDir}
}

// SetAccepting changes whether both listeners are established and not draining.
func (c *Checker) SetAccepting(value bool) { c.accepting.Store(value) }

// Live serves a non-sensitive process liveness response.
func (c *Checker) Live(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		w.Header().Set("Allow", http.MethodGet)
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write([]byte("live\n"))
}

// Ready serves readiness without leaking dependency details.
func (c *Checker) Ready(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		w.Header().Set("Allow", http.MethodGet)
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if !c.accepting.Load() {
		http.Error(w, "not ready", http.StatusServiceUnavailable)
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), time.Second)
	defer cancel()
	if c.db.Ready(ctx) != nil || writable(ctx, c.stateDir) != nil || writable(ctx, c.runtimeDir) != nil {
		http.Error(w, "not ready", http.StatusServiceUnavailable)
		return
	}
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write([]byte("ready\n"))
}

func writable(ctx context.Context, dir string) error {
	select {
	case <-ctx.Done():
		return ctx.Err()
	default:
	}
	file, err := os.CreateTemp(dir, ".box3-ota-ready-")
	if err != nil {
		return err
	}
	name := file.Name()
	if err := file.Chmod(0o600); err != nil {
		_ = file.Close()
		_ = os.Remove(name)
		return err
	}
	closeErr := file.Close()
	removeErr := os.Remove(name)
	return errors.Join(closeErr, removeErr)
}
