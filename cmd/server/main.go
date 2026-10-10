package main

import (
	"context"
	"errors"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials"
	"log/slog"
	"net"
	"net/http"
	"os"
	"os/signal"
	pb "regressgrid/gen/gridv1"
	"regressgrid/internal/control"
	"regressgrid/internal/store"
	"strconv"
	"strings"
	"syscall"
	"time"
)

func env(k, d string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return d
}
func main() {
	if err := run(); err != nil {
		slog.Error("server stopped", "error", err)
		os.Exit(1)
	}
}
func run() error {
	ctx, cancel := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer cancel()
	httpAddr := env("HTTP_ADDR", "127.0.0.1:8080")
	grpcAddr := env("GRPC_ADDR", "127.0.0.1:9090")
	apiToken := os.Getenv("API_TOKEN")
	workerToken := os.Getenv("WORKER_TOKEN")
	for _, pair := range [][2]string{{httpAddr, apiToken}, {grpcAddr, workerToken}} {
		host, _, err := net.SplitHostPort(pair[0])
		if err != nil {
			return err
		}
		if pair[1] == "" && host != "127.0.0.1" && host != "localhost" && host != "::1" {
			return errors.New("non-loopback listeners require API_TOKEN and WORKER_TOKEN")
		}
	}
	s, err := store.Open(ctx, env("DATA_DIR", ".local/data"), os.Getenv("DATABASE_URL"), os.Getenv("REDIS_URL"))
	if err != nil {
		return err
	}
	defer s.Close()
	c, err := control.New(ctx, s)
	if err != nil {
		return err
	}
	c.WorkerToken = workerToken
	if v := os.Getenv("REGRESSION_THRESHOLD"); v != "" {
		f, err := strconv.ParseFloat(v, 64)
		if err != nil || f < 0 || f > 1000 {
			return errors.New("invalid REGRESSION_THRESHOLD")
		}
		c.RegressionThreshold = f
	}
	opts := []grpc.ServerOption{grpc.MaxRecvMsgSize(128 << 10)}
	cert, key := os.Getenv("TLS_CERT"), os.Getenv("TLS_KEY")
	if cert != "" || key != "" {
		creds, err := credentials.NewServerTLSFromFile(cert, key)
		if err != nil {
			return err
		}
		opts = append(opts, grpc.Creds(creds))
	}
	gs := grpc.NewServer(opts...)
	pb.RegisterCoordinatorServer(gs, c)
	listener, err := net.Listen("tcp", grpcAddr)
	if err != nil {
		return err
	}
	hs := &http.Server{Addr: httpAddr, Handler: c.Handler(apiToken, env("WEB_DIR", "web/dist")), ReadHeaderTimeout: 5 * time.Second, IdleTimeout: 60 * time.Second}
	errs := make(chan error, 2)
	go func() { errs <- gs.Serve(listener) }()
	go func() {
		if cert != "" {
			errs <- hs.ListenAndServeTLS(cert, key)
		} else {
			errs <- hs.ListenAndServe()
		}
	}()
	go c.Run(ctx)
	slog.Info("RegressGrid ready", "http", httpAddr, "grpc", grpcAddr, "postgres", s.DB != nil, "redis", s.Redis != nil, "auth", apiToken != "", "tls", strings.TrimSpace(cert) != "")
	select {
	case <-ctx.Done():
	case err = <-errs:
		cancel()
	}
	shutdownCtx, stop := context.WithTimeout(context.Background(), 5*time.Second)
	defer stop()
	_ = hs.Shutdown(shutdownCtx)
	gs.Stop()
	if errors.Is(err, http.ErrServerClosed) {
		return nil
	}
	return err
}
