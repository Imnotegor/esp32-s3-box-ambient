// Package admin provides the authenticated, read-only administration UI.
package admin

import (
	"context"
	"crypto/sha256"
	"crypto/subtle"
	"embed"
	"errors"
	"fmt"
	"html/template"
	"net/http"
	"os"
	"strconv"
	"strings"
	"time"
	"unicode"

	"github.com/egormikhaylov/esp32-s3-box-ambient/box3-ota/internal/health"
	"github.com/egormikhaylov/esp32-s3-box-ambient/box3-ota/internal/model"
)

// Store is the read-only query surface available to admin handlers.
type Store interface {
	ListDevices(context.Context, int, int) ([]model.Device, error)
	ListTargetRefs(context.Context, int) ([]string, error)
	Job(context.Context, string) (model.Job, error)
	JobEvents(context.Context, string, int) ([]model.Event, error)
}

// Options define exact request trust boundaries and HTTP timeouts.
type Options struct {
	Listen                                      string
	AllowedHosts, AllowedOrigins                []string
	TokenFile                                   string
	ReadHeaderTimeout, ReadTimeout, IdleTimeout time.Duration
}

//go:embed templates/*.html
var templateFiles embed.FS

type handler struct {
	store          Store
	health         *health.Checker
	hosts, origins map[string]struct{}
	tokenDigest    [32]byte
	templates      *template.Template
}

// NewServer constructs the loopback/control HTTP server. Binding is done by the caller.
func NewServer(store Store, checker *health.Checker, opts Options) (*http.Server, error) {
	token, err := os.ReadFile(opts.TokenFile)
	if err != nil {
		return nil, fmt.Errorf("read admin token: %w", err)
	}
	token = []byte(strings.TrimSpace(string(token)))
	if len(token) < 32 || len(token) > 256 || strings.IndexFunc(string(token), unicode.IsSpace) >= 0 {
		return nil, errors.New("admin token must contain 32 to 256 non-whitespace bytes")
	}
	tmpl, err := template.ParseFS(templateFiles, "templates/*.html")
	if err != nil {
		return nil, fmt.Errorf("parse admin templates: %w", err)
	}
	h := &handler{store: store, health: checker, hosts: set(opts.AllowedHosts), origins: set(opts.AllowedOrigins), tokenDigest: sha256.Sum256(token), templates: tmpl}
	return &http.Server{Addr: opts.Listen, Handler: h, ReadHeaderTimeout: opts.ReadHeaderTimeout, ReadTimeout: opts.ReadTimeout, WriteTimeout: opts.ReadTimeout, IdleTimeout: opts.IdleTimeout, MaxHeaderBytes: 16 << 10}, nil
}
func set(values []string) map[string]struct{} {
	out := make(map[string]struct{}, len(values))
	for _, v := range values {
		out[v] = struct{}{}
	}
	return out
}
func (h *handler) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	securityHeaders(w)
	if _, ok := h.hosts[r.Host]; !ok {
		http.Error(w, "forbidden", http.StatusForbidden)
		return
	}
	if r.Header.Get("Forwarded") != "" || r.Header.Get("X-Forwarded-For") != "" || r.Header.Get("X-Forwarded-Host") != "" || r.Header.Get("X-Forwarded-Proto") != "" {
		http.Error(w, "forbidden", http.StatusForbidden)
		return
	}
	if origin := r.Header.Get("Origin"); origin != "" {
		if _, ok := h.origins[origin]; !ok {
			http.Error(w, "forbidden", http.StatusForbidden)
			return
		}
	}
	r.Body = http.MaxBytesReader(w, r.Body, 1024)
	if r.ContentLength != 0 {
		http.Error(w, "request body not allowed", http.StatusRequestEntityTooLarge)
		return
	}
	if r.URL.Path == "/health/live" {
		h.health.Live(w, r)
		return
	}
	if r.URL.Path == "/health/ready" {
		h.health.Ready(w, r)
		return
	}
	if !h.authorized(r) {
		w.Header().Set("WWW-Authenticate", `Bearer realm="box3-ota"`)
		http.Error(w, "unauthorized", http.StatusUnauthorized)
		return
	}
	if r.Method != http.MethodGet {
		w.Header().Set("Allow", http.MethodGet)
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	switch {
	case r.URL.Path == "/admin/":
		http.Redirect(w, r, "/admin/devices", http.StatusSeeOther)
	case r.URL.Path == "/admin/devices":
		h.devices(w, r)
	case r.URL.Path == "/admin/releases":
		h.releases(w, r)
	case strings.HasPrefix(r.URL.Path, "/admin/jobs/"):
		h.job(w, r)
	default:
		http.NotFound(w, r)
	}
}
func (h *handler) authorized(r *http.Request) bool {
	value := r.Header.Get("Authorization")
	if !strings.HasPrefix(value, "Bearer ") || strings.Count(value, " ") != 1 {
		return false
	}
	got := sha256.Sum256([]byte(strings.TrimPrefix(value, "Bearer ")))
	return subtle.ConstantTimeCompare(got[:], h.tokenDigest[:]) == 1
}
func (h *handler) devices(w http.ResponseWriter, r *http.Request) {
	limit, offset, ok := pagination(r)
	if !ok {
		http.Error(w, "bad pagination", http.StatusBadRequest)
		return
	}
	items, err := h.store.ListDevices(r.Context(), limit, offset)
	if err != nil {
		http.Error(w, "unavailable", http.StatusServiceUnavailable)
		return
	}
	h.render(w, "devices.html", map[string]any{"Devices": items, "Limit": limit, "Offset": offset})
}
func (h *handler) releases(w http.ResponseWriter, r *http.Request) {
	refs, err := h.store.ListTargetRefs(r.Context(), 50)
	if err != nil {
		http.Error(w, "unavailable", http.StatusServiceUnavailable)
		return
	}
	h.render(w, "releases.html", map[string]any{"References": refs})
}
func (h *handler) job(w http.ResponseWriter, r *http.Request) {
	id := strings.TrimPrefix(r.URL.Path, "/admin/jobs/")
	if id == "" || strings.Contains(id, "/") {
		http.NotFound(w, r)
		return
	}
	job, err := h.store.Job(r.Context(), id)
	if err != nil {
		http.NotFound(w, r)
		return
	}
	events, err := h.store.JobEvents(r.Context(), id, 100)
	if err != nil {
		http.Error(w, "unavailable", http.StatusServiceUnavailable)
		return
	}
	h.render(w, "job.html", map[string]any{"Job": job, "Events": events})
}
func (h *handler) render(w http.ResponseWriter, name string, data any) {
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	if err := h.templates.ExecuteTemplate(w, name, data); err != nil {
		return
	}
}
func pagination(r *http.Request) (int, int, bool) {
	limit, offset := 50, 0
	var err error
	if raw := r.URL.Query().Get("limit"); raw != "" {
		limit, err = strconv.Atoi(raw)
		if err != nil {
			return 0, 0, false
		}
	}
	if raw := r.URL.Query().Get("offset"); raw != "" {
		offset, err = strconv.Atoi(raw)
		if err != nil {
			return 0, 0, false
		}
	}
	return limit, offset, limit >= 1 && limit <= 100 && offset >= 0 && offset <= 100000
}
func securityHeaders(w http.ResponseWriter) {
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'")
	w.Header().Set("Referrer-Policy", "no-referrer")
	w.Header().Set("X-Content-Type-Options", "nosniff")
	w.Header().Set("X-Frame-Options", "DENY")
}
