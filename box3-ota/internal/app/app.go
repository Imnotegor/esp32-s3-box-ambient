// Package app wires the foundation's owned resources and graceful lifecycle.
package app

import (
	"context"
	"errors"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"sync"

	"github.com/egormikhaylov/esp32-s3-box-ambient/box3-ota/internal/admin"
	"github.com/egormikhaylov/esp32-s3-box-ambient/box3-ota/internal/config"
	"github.com/egormikhaylov/esp32-s3-box-ambient/box3-ota/internal/deviceapi"
	"github.com/egormikhaylov/esp32-s3-box-ambient/box3-ota/internal/health"
	"github.com/egormikhaylov/esp32-s3-box-ambient/box3-ota/internal/store"
)

// Run owns both listeners and SQLite until ctx is cancelled or a server fails.
func Run(ctx context.Context, cfg config.Config) error {
	if err := ensureDirectory(cfg.StateDir, 0o750); err != nil {
		return fmt.Errorf("state directory: %w", err)
	}
	db, err := store.Open(ctx, cfg.DatabasePath, cfg.SQLiteBusyTimeout.Duration)
	if err != nil {
		return err
	}
	dbOpen := true
	defer func() {
		if dbOpen {
			_ = db.Close()
		}
	}()
	checker := health.New(db, cfg.StateDir, cfg.RuntimeDir)
	adminServer, err := admin.NewServer(db, checker, admin.Options{Listen: cfg.AdminListen, AllowedHosts: cfg.AdminAllowedHosts, AllowedOrigins: cfg.AdminAllowedOrigins, TokenFile: cfg.AdminTokenFile, ReadHeaderTimeout: cfg.ReadHeaderTimeout.Duration, ReadTimeout: cfg.ReadTimeout.Duration, IdleTimeout: cfg.IdleTimeout.Duration})
	if err != nil {
		return err
	}
	adminListener, err := net.Listen("tcp", cfg.AdminListen)
	if err != nil {
		return fmt.Errorf("listen admin: %w", err)
	}
	deviceListener, err := deviceapi.Listen(cfg.RuntimeDir, cfg.DeviceSocketPath)
	if err != nil {
		_ = adminListener.Close()
		return err
	}
	deviceServer := deviceapi.NewServer(checker, cfg.ReadHeaderTimeout.Duration, cfg.ReadTimeout.Duration, cfg.IdleTimeout.Duration)
	checker.SetAccepting(true)
	log.Printf("box3-ota stage-2 foundation started (admin=%s wildcard=%t)", cfg.AdminListen, cfg.ContainerOnlyAllowWildcard)
	errCh := make(chan error, 2)
	var serveWG sync.WaitGroup
	serve := func(name string, server *http.Server, listener net.Listener) {
		defer serveWG.Done()
		serveErr := server.Serve(listener)
		if serveErr != nil && !errors.Is(serveErr, http.ErrServerClosed) {
			errCh <- fmt.Errorf("%s server: %w", name, serveErr)
			return
		}
		errCh <- nil
	}
	serveWG.Add(2)
	go serve("admin", adminServer, adminListener)
	go serve("device", deviceServer, deviceListener)
	var serveErr error
	select {
	case <-ctx.Done():
	case serveErr = <-errCh:
	}
	checker.SetAccepting(false)
	shutdownCtx, cancel := context.WithTimeout(context.Background(), cfg.ShutdownTimeout.Duration)
	defer cancel()
	var wg sync.WaitGroup
	shutdownErrors := make(chan error, 2)
	for _, server := range []*http.Server{adminServer, deviceServer} {
		wg.Add(1)
		go func(s *http.Server) {
			defer wg.Done()
			if shutdownErr := s.Shutdown(shutdownCtx); shutdownErr != nil {
				shutdownErrors <- shutdownErr
			}
		}(server)
	}
	wg.Wait()
	close(shutdownErrors)
	forced := false
	for shutdownErr := range shutdownErrors {
		forced = true
		serveErr = errors.Join(serveErr, shutdownErr)
	}
	if forced || shutdownCtx.Err() != nil {
		if closeErr := adminServer.Close(); closeErr != nil {
			serveErr = errors.Join(serveErr, closeErr)
		}
		if closeErr := deviceServer.Close(); closeErr != nil {
			serveErr = errors.Join(serveErr, closeErr)
		}
	}
	serveWG.Wait()
	if cleanupErr := deviceListener.Cleanup(); cleanupErr != nil && !errors.Is(cleanupErr, net.ErrClosed) {
		serveErr = errors.Join(serveErr, fmt.Errorf("cleanup device socket: %w", cleanupErr))
	}
	if closeErr := db.Close(); closeErr != nil {
		serveErr = errors.Join(serveErr, fmt.Errorf("close database: %w", closeErr))
	}
	dbOpen = false
	if shutdownCtx.Err() != nil {
		serveErr = errors.Join(serveErr, shutdownCtx.Err())
	}
	return serveErr
}
func ensureDirectory(path string, mode os.FileMode) error {
	if err := os.MkdirAll(path, mode); err != nil {
		return err
	}
	info, err := os.Lstat(path)
	if err != nil {
		return err
	}
	if !info.IsDir() || info.Mode().Perm()&0o027 != 0 {
		return errors.New("must be a directory with mode 0750 or tighter")
	}
	return nil
}
