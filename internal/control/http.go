package control

import (
	"context"
	"crypto/subtle"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	pb "regressgrid/gen/gridv1"
	"sort"
	"strings"
	"time"
)

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(v)
}
func fail(w http.ResponseWriter, code int, err error) {
	writeJSON(w, code, map[string]string{"error": err.Error()})
}
func decode(w http.ResponseWriter, r *http.Request, v any) error {
	r.Body = http.MaxBytesReader(w, r.Body, 64<<10)
	d := json.NewDecoder(r.Body)
	d.DisallowUnknownFields()
	if err := d.Decode(v); err != nil {
		return err
	}
	if err := d.Decode(&struct{}{}); err != io.EOF {
		return errors.New("expected a single JSON object")
	}
	return nil
}
func (c *Control) Handler(token, webDir string) http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /healthz", func(w http.ResponseWriter, r *http.Request) { writeJSON(w, 200, map[string]string{"status": "ok"}) })
	mux.HandleFunc("GET /readyz", func(w http.ResponseWriter, r *http.Request) {
		c.mu.Lock()
		ok := c.healthyStorage
		c.mu.Unlock()
		if c.store.DB != nil {
			ctx, cancel := contextWithTimeout(r)
			defer cancel()
			ok = ok && c.store.DB.Ping(ctx) == nil
		}
		if !ok {
			fail(w, 503, errors.New("storage unavailable"))
			return
		}
		writeJSON(w, 200, map[string]string{"status": "ready"})
	})
	mux.HandleFunc("GET /metrics", c.metrics)
	mux.HandleFunc("GET /api/state", func(w http.ResponseWriter, r *http.Request) {
		c.mu.Lock()
		defer c.mu.Unlock()
		builds := []BuildDetail{}
		for bid := range c.state.Builds {
			builds = append(builds, c.detail(bid))
		}
		sort.Slice(builds, func(i, j int) bool { return builds[i].CreatedAt.After(builds[j].CreatedAt) })
		if len(builds) > 100 {
			builds = builds[:100]
		}
		workers := []*Worker{}
		for _, worker := range c.state.Workers {
			workers = append(workers, worker)
		}
		sort.Slice(workers, func(i, j int) bool { return workers[i].ID < workers[j].ID })
		writeJSON(w, 200, map[string]any{"builds": builds, "workers": workers, "releases": c.state.Releases, "regression_threshold": c.RegressionThreshold})
	})
	mux.HandleFunc("POST /api/builds", func(w http.ResponseWriter, r *http.Request) {
		var s Submission
		if err := decode(w, r, &s); err != nil {
			fail(w, 400, err)
			return
		}
		d, err := c.Submit(s)
		if err != nil {
			fail(w, 400, err)
			return
		}
		writeJSON(w, 201, d)
	})
	mux.HandleFunc("GET /api/builds/{id}", func(w http.ResponseWriter, r *http.Request) {
		c.mu.Lock()
		defer c.mu.Unlock()
		if c.state.Builds[r.PathValue("id")] == nil {
			fail(w, 404, errors.New("build not found"))
			return
		}
		writeJSON(w, 200, c.detail(r.PathValue("id")))
	})
	mux.HandleFunc("POST /api/builds/{id}/cancel", func(w http.ResponseWriter, r *http.Request) {
		c.mu.Lock()
		defer c.mu.Unlock()
		b := c.state.Builds[r.PathValue("id")]
		if b == nil {
			fail(w, 404, errors.New("build not found"))
			return
		}
		events := map[string][]*pb.CoordinatorEvent{}
		if err := c.mutate(func() error {
			for _, jid := range b.JobIDs {
				j := c.state.Jobs[jid]
				if !terminal(j.Status) {
					events[j.WorkerID] = append(events[j.WorkerID], &pb.CoordinatorEvent{CancelJobId: j.ID, CancelLease: j.Lease})
					j.Status = "cancelled"
					t := time.Now().UTC()
					j.FinishedAt = &t
					j.Error = "cancelled by user"
				}
			}
			return nil
		}); err != nil {
			fail(w, 503, err)
			return
		}
		for wid, es := range events {
			if p := c.peers[wid]; p != nil {
				for _, e := range es {
					select {
					case p.ch <- e:
					default:
					}
				}
			}
		}
		writeJSON(w, 200, c.detail(b.ID))
	})
	mux.HandleFunc("POST /api/builds/{id}/retry", func(w http.ResponseWriter, r *http.Request) {
		c.mu.Lock()
		b := c.state.Builds[r.PathValue("id")]
		if b == nil {
			c.mu.Unlock()
			fail(w, 404, errors.New("build not found"))
			return
		}
		s := b.Submission
		if !terminal(b.Status) {
			c.mu.Unlock()
			fail(w, 409, errors.New("finish or cancel the current build before retrying"))
			return
		}
		c.mu.Unlock()
		d, err := c.Submit(s)
		if err != nil {
			fail(w, 400, err)
			return
		}
		writeJSON(w, 201, d)
	})
	mux.HandleFunc("POST /api/builds/{id}/promote", func(w http.ResponseWriter, r *http.Request) {
		var body struct {
			Version string `json:"version"`
		}
		if err := decode(w, r, &body); err != nil {
			fail(w, 400, err)
			return
		}
		if !versionRE.MatchString(body.Version) {
			fail(w, 400, errors.New("version must be a safe label, e.g. v1.0.0"))
			return
		}
		c.mu.Lock()
		defer c.mu.Unlock()
		bid := r.PathValue("id")
		b := c.state.Builds[bid]
		if b == nil {
			fail(w, 404, errors.New("build not found"))
			return
		}
		d := c.detail(bid)
		if b.Status != "passed" || (b.BaselineID != "" && d.Comparison.Status != "passed") {
			fail(w, 409, errors.New("promotion requires a passed matrix and a passed baseline comparison"))
			return
		}
		for _, j := range d.Jobs {
			if len(j.Artifacts) == 0 {
				fail(w, 409, errors.New("all configurations must have verified artifacts"))
				return
			}
		}
		rel := Release{ID: id("r_"), BuildID: bid, Version: body.Version, CreatedAt: time.Now().UTC()}
		if err := c.mutate(func() error {
			for _, prev := range c.state.Releases {
				if prev.Version == body.Version {
					return errors.New("version already published")
				}
			}
			c.state.Releases = append(c.state.Releases, rel)
			return nil
		}); err != nil {
			fail(w, 409, err)
			return
		}
		writeJSON(w, 201, rel)
	})
	mux.HandleFunc("GET /api/jobs/{id}/logs", c.logs)
	mux.HandleFunc("GET /api/jobs/{id}/artifacts/{name}", func(w http.ResponseWriter, r *http.Request) {
		c.mu.Lock()
		j := c.state.Jobs[r.PathValue("id")]
		var art *Artifact
		lease := ""
		if j != nil && (j.Status == "passed" || j.Status == "failed") {
			lease = j.Lease
			for _, a := range j.Artifacts {
				if a.Name == r.PathValue("name") {
					copy := a
					art = &copy
					break
				}
			}
		}
		c.mu.Unlock()
		if art == nil {
			fail(w, 404, errors.New("artifact not found"))
			return
		}
		w.Header().Set("Content-Disposition", fmt.Sprintf("attachment; filename=\"%s\"", art.Name))
		w.Header().Set("X-Checksum-SHA256", art.SHA256)
		http.ServeFile(w, r, filepath.Join(c.store.Dir, "jobs", j.ID, lease, art.Name))
	})
	if webDir != "" {
		fs := http.FileServer(http.Dir(webDir))
		mux.Handle("/", fs)
	}
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("X-Content-Type-Options", "nosniff")
		w.Header().Set("Referrer-Policy", "same-origin")
		if strings.HasPrefix(r.URL.Path, "/api/") && token != "" {
			if subtle.ConstantTimeCompare([]byte(r.Header.Get("Authorization")), []byte("Bearer "+token)) != 1 {
				fail(w, 401, errors.New("enter your API token to connect"))
				return
			}
		}
		mux.ServeHTTP(w, r)
	})
}
func (c *Control) logs(w http.ResponseWriter, r *http.Request) {
	jid := r.PathValue("id")
	c.mu.Lock()
	_, exists := c.state.Jobs[jid]
	c.mu.Unlock()
	if !exists {
		fail(w, 404, errors.New("job not found"))
		return
	}
	path := filepath.Join(c.store.Dir, "jobs", jid, "logs.txt")
	if r.URL.Query().Get("stream") != "1" {
		b, err := os.ReadFile(path)
		if os.IsNotExist(err) {
			b = []byte{}
			err = nil
		}
		if err != nil {
			fail(w, 500, err)
			return
		}
		w.Header().Set("Content-Type", "text/plain; charset=utf-8")
		_, _ = w.Write(b)
		return
	}
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("X-Accel-Buffering", "no")
	flusher, ok := w.(http.Flusher)
	if !ok {
		return
	}
	ticker := time.NewTicker(500 * time.Millisecond)
	defer ticker.Stop()
	offset := int64(0)
	for {
		f, err := os.Open(path)
		if err == nil {
			_, _ = f.Seek(offset, io.SeekStart)
			buf := make([]byte, 32<<10)
			n, _ := f.Read(buf)
			f.Close()
			if n > 0 {
				offset += int64(n)
				b, _ := json.Marshal(string(buf[:n]))
				if _, err = fmt.Fprintf(w, "data: %s\n\n", b); err != nil {
					return
				}
			}
		}
		if _, err = fmt.Fprint(w, ": heartbeat\n\n"); err != nil {
			return
		}
		flusher.Flush()
		c.mu.Lock()
		finished := terminal(c.state.Jobs[jid].Status)
		c.mu.Unlock()
		if finished {
			st, err := os.Stat(path)
			if err != nil || offset >= st.Size() {
				fmt.Fprint(w, "event: complete\ndata: {}\n\n")
				flusher.Flush()
				return
			}
		}
		select {
		case <-r.Context().Done():
			return
		case <-ticker.C:
		}
	}
}
func (c *Control) metrics(w http.ResponseWriter, r *http.Request) {
	c.mu.Lock()
	defer c.mu.Unlock()
	w.Header().Set("Content-Type", "text/plain; version=0.0.4")
	counts := map[string]int{}
	cache := 0
	durationSum := 0.0
	durations := []float64{}
	for _, j := range c.state.Jobs {
		counts[j.Status]++
		if j.Metrics.CacheHit {
			cache++
		}
		if j.FinishedAt != nil && j.StartedAt != nil {
			d := j.FinishedAt.Sub(*j.StartedAt).Seconds()
			durations = append(durations, d)
			durationSum += d
		}
	}
	fmt.Fprintf(w, "# HELP regressgrid_jobs_total Configuration jobs submitted.\n# TYPE regressgrid_jobs_total counter\nregressgrid_jobs_total %d\n", len(c.state.Jobs))
	fmt.Fprint(w, "# HELP regressgrid_jobs Current jobs by status.\n# TYPE regressgrid_jobs gauge\n")
	for _, s := range []string{"queued", "running", "passed", "failed", "cancelled"} {
		fmt.Fprintf(w, "regressgrid_jobs{status=%q} %d\n", s, counts[s])
	}
	fmt.Fprintf(w, "# TYPE regressgrid_queue_depth gauge\nregressgrid_queue_depth %d\n# TYPE regressgrid_worker_heartbeats_total counter\nregressgrid_worker_heartbeats_total %d\n# TYPE regressgrid_build_cache_hits_total counter\nregressgrid_build_cache_hits_total %d\n# TYPE regressgrid_failed_jobs_total counter\nregressgrid_failed_jobs_total %d\n", counts["queued"], c.state.Heartbeats, cache, counts["failed"])
	fmt.Fprint(w, "# TYPE regressgrid_job_duration_seconds histogram\n")
	for _, bound := range []float64{1, 5, 10, 30, 60, 120, 300, 600, 1800, 7200} {
		n := 0
		for _, d := range durations {
			if d <= bound {
				n++
			}
		}
		fmt.Fprintf(w, "regressgrid_job_duration_seconds_bucket{le=\"%g\"} %d\n", bound, n)
	}
	fmt.Fprintf(w, "regressgrid_job_duration_seconds_bucket{le=\"+Inf\"} %d\nregressgrid_job_duration_seconds_sum %g\nregressgrid_job_duration_seconds_count %d\n", len(durations), durationSum, len(durations))
	for _, worker := range c.state.Workers {
		health := 0
		if worker.Healthy {
			health = 1
		}
		fmt.Fprintf(w, "regressgrid_worker_healthy{worker=%q} %d\nregressgrid_worker_active_jobs{worker=%q} %d\nregressgrid_worker_slots{worker=%q} %d\n", worker.ID, health, worker.ID, worker.ActiveJobs, worker.ID, worker.Slots)
	}
}

var versionRE = regexp.MustCompile(`^[a-zA-Z0-9][a-zA-Z0-9_.-]{0,63}$`)

func contextWithTimeout(r *http.Request) (context.Context, context.CancelFunc) {
	return context.WithTimeout(r.Context(), 2*time.Second)
}
