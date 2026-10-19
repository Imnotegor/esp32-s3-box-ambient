// Command box3-ota runs the stage-2 service/storage foundation.
package main

import (
	"context"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/egormikhaylov/esp32-s3-box-ambient/box3-ota/internal/app"
	"github.com/egormikhaylov/esp32-s3-box-ambient/box3-ota/internal/config"
)

func main() {
	signalCtx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()
	if err := run(signalCtx, os.Args[1:]); err != nil {
		log.Printf("box3-ota stopped: %v", err)
		os.Exit(1)
	}
}

func run(ctx context.Context, args []string) error {
	flags := flag.NewFlagSet("box3-ota", flag.ContinueOnError)
	flags.SetOutput(io.Discard)
	configPath := flags.String("config", "", "absolute path to strict JSON configuration")
	check := flags.Bool("check-ready", false, "check the running service over its Unix socket and exit")
	requireLoopback := flags.Bool("require-loopback", false, "reject wildcard binds when sharing the host network")
	if err := flags.Parse(args); err != nil {
		return fmt.Errorf("parse arguments: %w", err)
	}
	if *configPath == "" {
		return flag.ErrHelp
	}
	cfg, err := config.Load(*configPath)
	if err != nil {
		return fmt.Errorf("load configuration: %w", err)
	}
	if *requireLoopback && cfg.ContainerOnlyAllowWildcard {
		return fmt.Errorf("host-network deployment forbids wildcard admin configuration")
	}
	if *check {
		return checkReady(ctx, cfg.DeviceSocketPath)
	}
	return app.Run(ctx, cfg)
}

func checkReady(ctx context.Context, socket string) error {
	ctx, cancel := context.WithTimeout(ctx, 2*time.Second)
	defer cancel()
	transport := &http.Transport{DialContext: func(ctx context.Context, _, _ string) (net.Conn, error) {
		return (&net.Dialer{}).DialContext(ctx, "unix", socket)
	}}
	defer transport.CloseIdleConnections()
	client := &http.Client{Transport: transport, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, "http://unix/health/ready", nil)
	if err != nil {
		return fmt.Errorf("create readiness request: %w", err)
	}
	response, err := client.Do(request)
	if err != nil {
		return fmt.Errorf("probe readiness: %w", err)
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return fmt.Errorf("readiness status: %d", response.StatusCode)
	}
	return nil
}
