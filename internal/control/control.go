package control

import (
	"context"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"regexp"
	pb "regressgrid/gen/gridv1"
	"regressgrid/internal/store"
	"sort"
	"strings"
	"sync"
	"time"
)

type peer struct{ ch chan *pb.CoordinatorEvent }
type Control struct {
	pb.UnimplementedCoordinatorServer
	mu                  sync.Mutex
	state               State
	store               *store.Store
	peers               map[string]*peer
	WorkerToken         string
	RegressionThreshold float64
	healthyStorage      bool
}

func New(ctx context.Context, s *store.Store) (*Control, error) {
	c := &Control{store: s, state: newState(), peers: map[string]*peer{}, RegressionThreshold: 10, healthyStorage: true}
	b, err := s.Load(ctx)
	if err != nil {
		return nil, err
	}
	if len(b) > 0 {
		if err = json.Unmarshal(b, &c.state); err != nil {
			return nil, err
		}
	}
	for _, w := range c.state.Workers {
		w.Healthy = false
		w.ActiveJobs = 0
	}
	for _, j := range c.state.Jobs {
		j.Lease = j.SavedLease
		if j.Status == "running" {
			c.requeue(j, "control plane restarted")
		}
	}
	c.aggregate()
	if err = s.Save(ctx, c.state); err != nil {
		return nil, err
	}
	return c, nil
}

// All mutations are persisted before dispatch/acknowledgment. Roll back on failure.
func (c *Control) mutate(fn func() error) error {
	backup, _ := json.Marshal(c.state)
	if err := fn(); err != nil {
		c.state = newState()
		_ = json.Unmarshal(backup, &c.state)
		c.restoreLeases()
		return err
	}
	c.aggregate()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := c.store.Save(ctx, c.state); err != nil {
		c.state = newState()
		_ = json.Unmarshal(backup, &c.state)
		c.restoreLeases()
		c.healthyStorage = false
		return err
	}
	c.healthyStorage = true
	return nil
}
func (c *Control) restoreLeases() {
	for _, j := range c.state.Jobs {
		j.Lease = j.SavedLease
	}
}
func (c *Control) aggregate() {
	for _, w := range c.state.Workers {
		w.ActiveJobs = 0
	}
	for _, j := range c.state.Jobs {
		j.SavedLease = j.Lease
		if j.Status == "running" {
			if w := c.state.Workers[j.WorkerID]; w != nil {
				w.ActiveJobs++
			}
		}
	}
	for _, b := range c.state.Builds {
		b.Status = "passed"
		running, queued, failed, cancelled := false, false, false, false
		for _, jid := range b.JobIDs {
			switch c.state.Jobs[jid].Status {
			case "running":
				running = true
			case "queued":
				queued = true
			case "failed":
				failed = true
			case "cancelled":
				cancelled = true
			}
		}
		switch {
		case running:
			b.Status = "running"
		case queued:
			b.Status = "queued"
		case failed:
			b.Status = "failed"
		case cancelled:
			b.Status = "cancelled"
		}
	}
}
func (c *Control) requeue(j *PersistedJob, reason string) {
	j.Lease = ""
	j.Error = reason
	j.Artifacts = []Artifact{}
	if j.Attempt >= 3 {
		j.Status = "failed"
		j.FailureSignature = Signature(reason)
		t := time.Now().UTC()
		j.FinishedAt = &t
	} else {
		j.Status = "queued"
		j.WorkerID = ""
		j.StartedAt = nil
		j.FinishedAt = nil
	}
}

var commitRE = regexp.MustCompile(`^[a-zA-Z0-9][a-zA-Z0-9._/\-]{0,127}$`)

func validate(s *Submission) error {
	if !(strings.HasPrefix(s.Repository, "https://") || strings.HasPrefix(s.Repository, "file:///")) {
		return errors.New("repository must use https:// or file:///")
	}
	if strings.ContainsAny(s.Repository, "\n\r\x00") || len(s.Repository) > 2048 {
		return errors.New("invalid repository")
	}
	if !commitRE.MatchString(s.Commit) || strings.Contains(s.Commit, "..") {
		return errors.New("invalid commit or ref")
	}
	if len(s.Configurations) == 0 || len(s.Configurations) > 16 {
		return errors.New("choose 1–16 configurations")
	}
	seen := map[Config]bool{}
	for _, cfg := range s.Configurations {
		if (cfg.Compiler != "gcc" && cfg.Compiler != "clang") || (cfg.BuildType != "Debug" && cfg.BuildType != "Release") {
			return errors.New("supported matrix: gcc/clang × Debug/Release")
		}
		if seen[cfg] {
			return errors.New("duplicate configuration")
		}
		seen[cfg] = true
	}
	if s.TimeoutSeconds == 0 {
		s.TimeoutSeconds = 600
	}
	if s.TimeoutSeconds < 1 || s.TimeoutSeconds > 7200 {
		return errors.New("timeout must be 1–7200 seconds")
	}
	if s.Plugin == "" {
		s.Plugin = "cmake"
	}
	if s.Plugin != "cmake" && s.Plugin != "verilator" {
		return errors.New("unknown plugin")
	}
	return nil
}
func (c *Control) Submit(s Submission) (BuildDetail, error) {
	if err := validate(&s); err != nil {
		return BuildDetail{}, err
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	if s.BaselineID != "" {
		b := c.state.Builds[s.BaselineID]
		if b == nil || b.Repository != s.Repository || b.Status != "passed" {
			return BuildDetail{}, errors.New("baseline must be a passed build of the same repository")
		}
	}
	b := &Build{ID: id("b_"), Submission: s, CreatedAt: time.Now().UTC(), JobIDs: []string{}}
	err := c.mutate(func() error {
		c.state.Builds[b.ID] = b
		for _, cfg := range s.Configurations {
			j := &PersistedJob{Job: Job{ID: id("j_"), BuildID: b.ID, Config: cfg, Status: "queued", Artifacts: []Artifact{}}}
			c.state.Jobs[j.ID] = j
			b.JobIDs = append(b.JobIDs, j.ID)
		}
		return nil
	})
	if err != nil {
		return BuildDetail{}, err
	}
	return c.detail(b.ID), nil
}
func (c *Control) detail(bid string) BuildDetail {
	b := c.state.Builds[bid]
	d := BuildDetail{Build: b, Jobs: []Job{}, Comparison: Comparison{Status: "no_baseline", Changes: []Change{}}}
	for _, jid := range b.JobIDs {
		d.Jobs = append(d.Jobs, c.state.Jobs[jid].Job)
	}
	if b.BaselineID == "" {
		return d
	}
	d.Comparison.BaselineID = b.BaselineID
	d.Comparison.Status = "pending"
	if !terminal(b.Status) {
		return d
	}
	d.Comparison.Status = "passed"
	base := c.state.Builds[b.BaselineID]
	if base == nil {
		d.Comparison.Status = "incomplete"
		d.Comparison.Reason = "baseline unavailable"
		return d
	}
	for _, candidate := range d.Jobs {
		var baseline *Job
		for _, jid := range base.JobIDs {
			j := &c.state.Jobs[jid].Job
			if j.Config == candidate.Config {
				baseline = j
				break
			}
		}
		if baseline == nil || candidate.Status != "passed" {
			d.Comparison.Status = "incomplete"
			d.Comparison.Reason = "every candidate configuration must pass and match the baseline"
			continue
		}
		pairs := []struct {
			name            string
			base, candidate float64
			lower           bool
		}{{"build_seconds", baseline.Metrics.BuildSeconds, candidate.Metrics.BuildSeconds, true}, {"test_seconds", baseline.Metrics.TestSeconds, candidate.Metrics.TestSeconds, true}, {"peak_memory_bytes", float64(baseline.Metrics.PeakMemoryBytes), float64(candidate.Metrics.PeakMemoryBytes), true}, {"warnings", float64(baseline.Metrics.Warnings), float64(candidate.Metrics.Warnings), true}, {"binary_bytes", float64(baseline.Metrics.BinaryBytes), float64(candidate.Metrics.BinaryBytes), true}, {"latency_ms", baseline.Metrics.LatencyMS, candidate.Metrics.LatencyMS, true}, {"throughput", baseline.Metrics.Throughput, candidate.Metrics.Throughput, false}}
		for _, p := range pairs {
			if p.base == 0 && p.candidate == 0 {
				continue
			}
			if p.base == 0 {
				if p.name == "warnings" {
					d.Comparison.Changes = append(d.Comparison.Changes, Change{JobID: candidate.ID, Metric: p.name, Candidate: p.candidate, Regression: true})
					if d.Comparison.Status != "incomplete" {
						d.Comparison.Status = "regression"
					}
				}
				continue
			}
			pct := (p.candidate - p.base) / p.base * 100
			reg := (p.lower && pct > c.RegressionThreshold) || (!p.lower && pct < -c.RegressionThreshold)
			d.Comparison.Changes = append(d.Comparison.Changes, Change{candidate.ID, p.name, p.base, p.candidate, pct, reg})
			if reg && d.Comparison.Status != "incomplete" {
				d.Comparison.Status = "regression"
			}
		}
	}
	return d
}
func (c *Control) Run(ctx context.Context) {
	ticker := time.NewTicker(time.Second)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			c.schedule(ctx)
		}
	}
}
func (c *Control) schedule(ctx context.Context) {
	c.mu.Lock()
	defer c.mu.Unlock()
	now := time.Now().UTC()
	queued := []string{}
	for jid, j := range c.state.Jobs {
		if j.Status == "queued" {
			queued = append(queued, jid)
		}
	}
	sort.Slice(queued, func(i, j int) bool {
		a, b := c.state.Jobs[queued[i]], c.state.Jobs[queued[j]]
		x, y := c.state.Builds[a.BuildID], c.state.Builds[b.BuildID]
		if x.CreatedAt.Equal(y.CreatedAt) {
			return a.ID < b.ID
		}
		return x.CreatedAt.Before(y.CreatedAt)
	})
	if err := c.store.Queue(ctx, queued); err != nil {
		slog.Warn("redis queue unavailable; using durable queue", "error", err)
	} else if c.store.Redis != nil {
		if ids, err := c.store.QueueIDs(ctx); err == nil {
			queued = ids
		}
	}
	events := map[string][]*pb.CoordinatorEvent{}
	err := c.mutate(func() error {
		for _, w := range c.state.Workers {
			if w.Healthy && now.Sub(w.LastSeen) > 20*time.Second {
				w.Healthy = false
				for _, j := range c.state.Jobs {
					if j.WorkerID == w.ID && j.Status == "running" {
						if p := c.peers[w.ID]; p != nil {
							select {
							case p.ch <- &pb.CoordinatorEvent{CancelJobId: j.ID, CancelLease: j.Lease}:
							default:
							}
						}
						c.requeue(j, "worker heartbeat expired")
					}
				}
			}
		}
		c.aggregate()
		for _, jid := range queued {
			j := c.state.Jobs[jid]
			if j == nil || j.Status != "queued" {
				continue
			}
			b := c.state.Builds[j.BuildID]
			var best *Worker
			for _, w := range c.state.Workers {
				if !w.Healthy || now.Sub(w.LastSeen) > 20*time.Second || c.peers[w.ID] == nil || w.ActiveJobs >= w.Slots || b.Plugin == "verilator" && !w.Verilator {
					continue
				}
				supported := false
				for _, cc := range w.Compilers {
					if cc == j.Compiler {
						supported = true
					}
				}
				if supported && (best == nil || w.ActiveJobs < best.ActiveJobs || w.ActiveJobs == best.ActiveJobs && w.ID < best.ID) {
					best = w
				}
			}
			if best == nil {
				continue
			}
			j.Status = "running"
			j.WorkerID = best.ID
			j.Lease = id("lease_")
			j.Attempt++
			j.StartedAt = &now
			j.Error = ""
			j.Metrics = Metrics{}
			best.ActiveJobs++
			events[best.ID] = append(events[best.ID], &pb.CoordinatorEvent{Assignment: &pb.Assignment{JobId: j.ID, Lease: j.Lease, Repository: b.Repository, Commit: b.Commit, Compiler: j.Compiler, BuildType: j.BuildType, TimeoutSeconds: uint32(b.TimeoutSeconds), Plugin: b.Plugin}})
		}
		return nil
	})
	if err != nil {
		slog.Error("scheduler persistence failed", "error", err)
		return
	}
	for wid, list := range events {
		for _, e := range list {
			select {
			case c.peers[wid].ch <- e:
			default:
				slog.Warn("worker dispatch channel full", "worker", wid)
			}
		}
	}
}
func (c *Control) Work(stream pb.Coordinator_WorkServer) error {
	if c.WorkerToken != "" {
		md, _ := metadata.FromIncomingContext(stream.Context())
		values := md.Get("authorization")
		if len(values) != 1 || subtle.ConstantTimeCompare([]byte(values[0]), []byte("Bearer "+c.WorkerToken)) != 1 {
			return status.Error(codes.Unauthenticated, "invalid worker token")
		}
	}
	first, err := stream.Recv()
	if err != nil {
		return err
	}
	h := first.Hello
	if h == nil || !regexp.MustCompile(`^[a-zA-Z0-9_.-]{1,64}$`).MatchString(h.Id) || h.Slots < 1 || h.Slots > 64 {
		return status.Error(codes.InvalidArgument, "invalid worker hello")
	}
	p := &peer{ch: make(chan *pb.CoordinatorEvent, 64)}
	c.mu.Lock()
	if c.peers[h.Id] != nil {
		c.mu.Unlock()
		return status.Error(codes.AlreadyExists, "worker id already connected")
	}
	err = c.mutate(func() error {
		c.state.Workers[h.Id] = &Worker{ID: h.Id, Compilers: h.Compilers, Slots: int(h.Slots), Platform: h.Platform, Verilator: h.Verilator, Healthy: true, LastSeen: time.Now().UTC()}
		return nil
	})
	if err == nil {
		c.peers[h.Id] = p
	}
	c.mu.Unlock()
	if err != nil {
		return status.Error(codes.Unavailable, err.Error())
	}
	defer func() {
		c.mu.Lock()
		defer c.mu.Unlock()
		delete(c.peers, h.Id)
		if err := c.mutate(func() error {
			if w := c.state.Workers[h.Id]; w != nil {
				w.Healthy = false
			}
			for _, j := range c.state.Jobs {
				if j.WorkerID == h.Id && j.Status == "running" {
					c.requeue(j, "worker disconnected")
				}
			}
			return nil
		}); err != nil {
			slog.Error("disconnect persistence failed", "error", err)
		}
	}()
	incoming := make(chan *pb.WorkerEvent)
	recvErr := make(chan error, 1)
	done := make(chan struct{})
	defer close(done)
	go func() {
		for {
			e, err := stream.Recv()
			if err != nil {
				recvErr <- err
				return
			}
			select {
			case incoming <- e:
			case <-done:
				return
			}
		}
	}()
	for {
		select {
		case <-stream.Context().Done():
			return stream.Context().Err()
		case err := <-recvErr:
			if err == io.EOF {
				return nil
			}
			return err
		case e := <-p.ch:
			if err = stream.Send(e); err != nil {
				return err
			}
		case e := <-incoming:
			if err = c.event(h.Id, e); err != nil {
				return status.Error(codes.Internal, err.Error())
			}
		}
	}
}
func (c *Control) event(wid string, e *pb.WorkerEvent) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	if e.Heartbeat {
		return c.mutate(func() error {
			c.state.Workers[wid].LastSeen = time.Now().UTC()
			c.state.Workers[wid].Healthy = true
			c.state.Heartbeats++
			return nil
		})
	}
	j := c.state.Jobs[e.JobId]
	if j == nil || j.Status != "running" || j.WorkerID != wid || j.Lease != e.Lease {
		return nil
	}
	dir := filepath.Join(c.store.Dir, "jobs", j.ID, j.Lease)
	if err := os.MkdirAll(dir, 0750); err != nil {
		return err
	}
	if e.Log != "" {
		path := filepath.Join(c.store.Dir, "jobs", j.ID, "logs.txt")
		if len(e.Log) > 65536 {
			return errors.New("log chunk too large")
		}
		if st, err := os.Stat(path); err == nil && st.Size() > 16<<20 {
			return nil
		}
		f, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0640)
		if err != nil {
			return err
		}
		_, err = fmt.Fprintf(f, "[%s attempt=%d] %s", time.Now().UTC().Format(time.RFC3339), j.Attempt, e.Log)
		closeErr := f.Close()
		if err != nil {
			return err
		}
		return closeErr
	}
	if a := e.Artifact; a != nil {
		if !regexp.MustCompile(`^[a-zA-Z0-9_.-]{1,100}$`).MatchString(a.Name) || strings.Contains(a.Name, "..") {
			return errors.New("invalid artifact name")
		}
		if len(a.Data) > 65536 {
			return errors.New("artifact chunk too large")
		}
		path := filepath.Join(dir, a.Name+".part")
		f, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0640)
		if err != nil {
			return err
		}
		st, err := f.Stat()
		if err != nil {
			f.Close()
			return err
		}
		if st.Size()+int64(len(a.Data)) > 256<<20 {
			f.Close()
			return errors.New("artifact exceeds 256 MiB")
		}
		_, err = f.Write(a.Data)
		ce := f.Close()
		if err != nil {
			return err
		}
		if ce != nil {
			return ce
		}
		if a.Complete {
			f, err := os.Open(path)
			if err != nil {
				return err
			}
			hash := sha256.New()
			size, err := io.Copy(hash, f)
			f.Close()
			if err != nil {
				return err
			}
			sum := hex.EncodeToString(hash.Sum(nil))
			if sum != a.Sha256 {
				return errors.New("artifact checksum mismatch")
			}
			if err = os.Rename(path, filepath.Join(dir, a.Name)); err != nil {
				return err
			}
			return c.mutate(func() error { j.Artifacts = append(j.Artifacts, Artifact{a.Name, sum, size}); return nil })
		}
		return nil
	}
	if r := e.Result; r != nil {
		return c.mutate(func() error {
			if r.InfrastructureError && j.Attempt < 3 {
				c.requeue(j, r.Error)
				return nil
			}
			if r.Status != "passed" && r.Status != "failed" && r.Status != "cancelled" {
				return errors.New("invalid result status")
			}
			j.Status = r.Status
			j.Error = r.Error
			t := time.Now().UTC()
			j.FinishedAt = &t
			if r.Status == "failed" {
				j.FailureSignature = Signature(r.Error)
			}
			if m := r.Metrics; m != nil {
				j.Metrics = Metrics{m.BuildSeconds, m.TestSeconds, m.PeakMemoryBytes, m.CpuSeconds, m.Warnings, m.TestsPassed, m.TestsFailed, m.BinaryBytes, m.LatencyMs, m.Throughput, m.CacheHit}
			}
			return nil
		})
	}
	return nil
}
