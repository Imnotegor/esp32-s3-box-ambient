// Package deviceapi owns the fail-closed Unix-domain HTTP boundary.
package deviceapi

import (
	"errors"
	"fmt"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"syscall"
	"time"

	"github.com/egormikhaylov/esp32-s3-box-ambient/box3-ota/internal/health"
)

// Listener owns a Unix listener and inode-safe cleanup operation.
type Listener struct {
	net.Listener
	path          string
	device, inode uint64
}

// Listen creates a restrictive Unix socket, refusing unsafe stale path types or owners.
func Listen(runtimeDir, socketPath string) (*Listener, error) {
	if !filepath.IsAbs(runtimeDir) || filepath.Dir(socketPath) != runtimeDir {
		return nil, errors.New("socket must be directly inside an absolute runtime directory")
	}
	if err := os.MkdirAll(runtimeDir, 0o750); err != nil {
		return nil, fmt.Errorf("create runtime directory: %w", err)
	}
	dirInfo, err := os.Lstat(runtimeDir)
	if err != nil {
		return nil, fmt.Errorf("inspect runtime directory: %w", err)
	}
	if !dirInfo.IsDir() || dirInfo.Mode().Perm()&0o027 != 0 {
		return nil, errors.New("runtime directory must be mode 0750 or tighter")
	}
	dirStat, ok := dirInfo.Sys().(*syscall.Stat_t)
	if !ok || int(dirStat.Uid) != os.Geteuid() {
		return nil, errors.New("runtime directory must be owned by the service user")
	}
	if info, statErr := os.Lstat(socketPath); statErr == nil {
		if info.Mode()&os.ModeSocket == 0 {
			return nil, errors.New("refusing to remove non-socket device path")
		}
		stat, ok := info.Sys().(*syscall.Stat_t)
		if !ok || int(stat.Uid) != os.Geteuid() {
			return nil, errors.New("refusing to remove socket owned by another user")
		}
		conn, dialErr := net.DialTimeout("unix", socketPath, 100*time.Millisecond)
		if dialErr == nil {
			_ = conn.Close()
			return nil, errors.New("device socket is already accepting connections")
		}
		if err := os.Remove(socketPath); err != nil {
			return nil, fmt.Errorf("remove stale socket: %w", err)
		}
	} else if !errors.Is(statErr, os.ErrNotExist) {
		return nil, fmt.Errorf("inspect device socket: %w", statErr)
	}
	oldMask := syscall.Umask(0o117)
	unixListener, err := net.ListenUnix("unix", &net.UnixAddr{Name: socketPath, Net: "unix"})
	syscall.Umask(oldMask)
	if err != nil {
		return nil, fmt.Errorf("listen on Unix socket: %w", err)
	}
	unixListener.SetUnlinkOnClose(false)
	if err := os.Chown(socketPath, -1, int(dirStat.Gid)); err != nil {
		_ = unixListener.Close()
		_ = os.Remove(socketPath)
		return nil, fmt.Errorf("set socket group from runtime directory: %w", err)
	}
	if err := os.Chmod(socketPath, 0o660); err != nil {
		_ = unixListener.Close()
		_ = os.Remove(socketPath)
		return nil, fmt.Errorf("set socket mode: %w", err)
	}
	info, err := os.Lstat(socketPath)
	if err != nil {
		_ = unixListener.Close()
		return nil, fmt.Errorf("inspect created socket: %w", err)
	}
	stat, ok := info.Sys().(*syscall.Stat_t)
	if !ok {
		_ = unixListener.Close()
		return nil, errors.New("cannot identify created socket")
	}
	return &Listener{Listener: unixListener, path: socketPath, device: uint64(stat.Dev), inode: uint64(stat.Ino)}, nil
}

// Cleanup closes the listener and removes only the same socket inode it created.
func (l *Listener) Cleanup() error {
	closeErr := l.Close()
	if errors.Is(closeErr, net.ErrClosed) {
		closeErr = nil
	}
	info, err := os.Lstat(l.path)
	if errors.Is(err, os.ErrNotExist) {
		return closeErr
	}
	if err != nil {
		return errors.Join(closeErr, err)
	}
	stat, ok := info.Sys().(*syscall.Stat_t)
	if !ok || info.Mode()&os.ModeSocket == 0 || uint64(stat.Dev) != l.device || uint64(stat.Ino) != l.inode {
		return errors.Join(closeErr, errors.New("device socket path changed; refusing cleanup"))
	}
	return errors.Join(closeErr, os.Remove(l.path))
}

// NewServer constructs the Unix-only health and fail-closed device server.
// No request header is accepted as authenticated device identity.
func NewServer(checker *health.Checker, readHeaderTimeout, readTimeout, idleTimeout time.Duration) *http.Server {
	mux := http.NewServeMux()
	mux.HandleFunc("/health/live", checker.Live)
	mux.HandleFunc("/health/ready", checker.Ready)
	mux.HandleFunc("/v1/", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			w.Header().Set("Allow", http.MethodGet)
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		http.Error(w, "device API unavailable until certificate-aware edge identity mapping is implemented", http.StatusServiceUnavailable)
	})
	return &http.Server{Handler: http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		r.Body = http.MaxBytesReader(w, r.Body, 1024)
		if r.ContentLength != 0 {
			http.Error(w, "request body not allowed", http.StatusRequestEntityTooLarge)
			return
		}
		mux.ServeHTTP(w, r)
	}), ReadHeaderTimeout: readHeaderTimeout, ReadTimeout: readTimeout, WriteTimeout: readTimeout, IdleTimeout: idleTimeout, MaxHeaderBytes: 16 << 10}
}
