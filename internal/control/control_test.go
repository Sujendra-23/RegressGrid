package control

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	pb "regressgrid/gen/gridv1"
	"regressgrid/internal/store"
	"strings"
	"testing"
	"time"
)

func fixture(t *testing.T) *Control {
	t.Helper()
	s, err := store.Open(context.Background(), t.TempDir(), "", "")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(s.Close)
	c, err := New(context.Background(), s)
	if err != nil {
		t.Fatal(err)
	}
	return c
}
func request() Submission {
	return Submission{Repository: "https://github.com/example/kernel.git", Commit: "main", Configurations: []Config{{"gcc", "Debug"}, {"clang", "Release"}}}
}
func register(c *Control, id string, compilers []string, active, slots int) {
	c.state.Workers[id] = &Worker{ID: id, Compilers: compilers, ActiveJobs: active, Slots: slots, Healthy: true, LastSeen: time.Now().UTC()}
	c.peers[id] = &peer{make(chan *pb.CoordinatorEvent, 64)}
}
func TestValidation(t *testing.T) {
	cases := []struct {
		name string
		edit func(*Submission)
	}{{"shell ref", func(s *Submission) { s.Commit = "main; rm -rf /" }}, {"option ref", func(s *Submission) { s.Commit = "--upload-pack=x" }}, {"duplicate", func(s *Submission) { s.Configurations = append(s.Configurations, s.Configurations[0]) }}, {"compiler", func(s *Submission) { s.Configurations[0].Compiler = "msvc" }}, {"timeout", func(s *Submission) { s.TimeoutSeconds = 7201 }}, {"url", func(s *Submission) { s.Repository = "ssh://example" }}, {"plugin", func(s *Submission) { s.Plugin = "unknown" }}}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			c := fixture(t)
			s := request()
			tc.edit(&s)
			if _, err := c.Submit(s); err == nil {
				t.Fatal("invalid request accepted")
			}
		})
	}
}
func TestLeastLoadedCapabilityScheduling(t *testing.T) {
	c := fixture(t)
	register(c, "a-gcc", []string{"gcc"}, 0, 1)
	register(c, "b-clang", []string{"clang"}, 0, 1)
	register(c, "c-offline", []string{"gcc", "clang"}, 0, 4)
	c.state.Workers["c-offline"].LastSeen = time.Now().Add(-time.Minute)
	b, err := c.Submit(request())
	if err != nil {
		t.Fatal(err)
	}
	c.schedule(context.Background())
	for _, jid := range b.JobIDs {
		j := c.state.Jobs[jid]
		if j.Status != "running" {
			t.Fatal(j.Status)
		}
		want := "a-gcc"
		if j.Compiler == "clang" {
			want = "b-clang"
		}
		if j.WorkerID != want {
			t.Fatalf("got %s want %s", j.WorkerID, want)
		}
	}
	if c.state.Workers["c-offline"].Healthy {
		t.Fatal("expired heartbeat healthy")
	}
}
func TestLeaseFencesStaleResults(t *testing.T) {
	c := fixture(t)
	register(c, "worker", []string{"gcc", "clang"}, 0, 2)
	b, _ := c.Submit(request())
	c.schedule(context.Background())
	j := c.state.Jobs[b.JobIDs[0]]
	lease := j.Lease
	if err := c.event("worker", &pb.WorkerEvent{JobId: j.ID, Lease: "stale", Result: &pb.Result{Status: "passed"}}); err != nil {
		t.Fatal(err)
	}
	if j.Status != "running" {
		t.Fatal("stale result accepted")
	}
	if err := c.event("worker", &pb.WorkerEvent{JobId: j.ID, Lease: lease, Result: &pb.Result{Status: "passed", Metrics: &pb.Metrics{TestsPassed: 3}}}); err != nil {
		t.Fatal(err)
	}
	if j.Status != "passed" {
		t.Fatal("valid result rejected")
	}
}
func TestHeartbeatLossRetriesThenFails(t *testing.T) {
	c := fixture(t)
	register(c, "worker", []string{"gcc", "clang"}, 0, 2)
	b, _ := c.Submit(request())
	for attempt := 1; attempt <= 3; attempt++ {
		c.state.Workers["worker"].Healthy = true
		c.state.Workers["worker"].LastSeen = time.Now()
		c.schedule(context.Background())
		for _, jid := range b.JobIDs {
			if c.state.Jobs[jid].Attempt != attempt {
				t.Fatalf("bad attempt %d", c.state.Jobs[jid].Attempt)
			}
		}
		c.state.Workers["worker"].LastSeen = time.Now().Add(-time.Minute)
		c.schedule(context.Background())
	}
	if c.state.Builds[b.ID].Status != "failed" {
		t.Fatal("retry limit not enforced")
	}
}
func finish(c *Control, bid string, latency float64) {
	for _, jid := range c.state.Builds[bid].JobIDs {
		j := c.state.Jobs[jid]
		j.Status = "passed"
		j.Metrics.LatencyMS = latency
		j.Artifacts = []Artifact{{"artifacts.tar.gz", "abc", 3}}
	}
	c.aggregate()
}
func TestBaselineRegressionBlocksPromotion(t *testing.T) {
	c := fixture(t)
	s := request()
	baseline, _ := c.Submit(s)
	finish(c, baseline.ID, 18.2)
	s.BaselineID = baseline.ID
	candidate, err := c.Submit(s)
	if err != nil {
		t.Fatal(err)
	}
	finish(c, candidate.ID, 22.7)
	d := c.detail(candidate.ID)
	if d.Comparison.Status != "regression" {
		t.Fatalf("got %s", d.Comparison.Status)
	}
	if pct := d.Comparison.Changes[0].Percent; pct < 24 || pct > 25 {
		t.Fatal(pct)
	}
	r := httptest.NewRequest("POST", "/api/builds/"+candidate.ID+"/promote", strings.NewReader(`{"version":"v1"}`))
	w := httptest.NewRecorder()
	c.Handler("", "").ServeHTTP(w, r)
	if w.Code != 409 {
		t.Fatalf("promotion returned %d", w.Code)
	}
}
func TestMissingMatrixBaselineIsIncomplete(t *testing.T) {
	c := fixture(t)
	s := request()
	s.Configurations = s.Configurations[:1]
	base, _ := c.Submit(s)
	finish(c, base.ID, 10)
	s = request()
	s.BaselineID = base.ID
	b, _ := c.Submit(s)
	finish(c, b.ID, 10)
	if c.detail(b.ID).Comparison.Status != "incomplete" {
		t.Fatal("unmatched configuration accepted")
	}
}
func TestArtifactChecksumAndLeaseIsolation(t *testing.T) {
	c := fixture(t)
	register(c, "worker", []string{"gcc", "clang"}, 0, 2)
	b, _ := c.Submit(request())
	c.schedule(context.Background())
	j := c.state.Jobs[b.JobIDs[0]]
	chunk := &pb.WorkerEvent{JobId: j.ID, Lease: j.Lease, Artifact: &pb.ArtifactChunk{Name: "artifact.tgz", Data: []byte("abc")}}
	if err := c.event("worker", chunk); err != nil {
		t.Fatal(err)
	}
	chunk.Artifact.Data = nil
	chunk.Artifact.Complete = true
	chunk.Artifact.Sha256 = "wrong"
	if c.event("worker", chunk) == nil {
		t.Fatal("bad checksum accepted")
	}
	sum := sha256.Sum256([]byte("abc"))
	chunk.Artifact.Sha256 = hex.EncodeToString(sum[:])
	if err := c.event("worker", chunk); err != nil {
		t.Fatal(err)
	}
	if len(j.Artifacts) != 1 {
		t.Fatal("verified artifact missing")
	}
	chunk.Artifact.Name = "../escape"
	if c.event("worker", chunk) == nil {
		t.Fatal("traversal accepted")
	}
}
func TestPersistenceRestartRequeuesAndHidesLease(t *testing.T) {
	c := fixture(t)
	register(c, "worker", []string{"gcc", "clang"}, 0, 2)
	b, _ := c.Submit(request())
	c.schedule(context.Background())
	lease := c.state.Jobs[b.JobIDs[0]].Lease
	c2, err := New(context.Background(), c.store)
	if err != nil {
		t.Fatal(err)
	}
	if c2.state.Jobs[b.JobIDs[0]].Status != "queued" {
		t.Fatal("restart left stranded job")
	}
	w := httptest.NewRecorder()
	c.Handler("", "").ServeHTTP(w, httptest.NewRequest("GET", "/api/state", nil))
	if strings.Contains(w.Body.String(), lease) {
		t.Fatal("lease leaked through HTTP")
	}
}
func TestAuthenticationCancelAndDuplicateRelease(t *testing.T) {
	c := fixture(t)
	b, _ := c.Submit(request())
	handler := c.Handler("secret", "")
	w := httptest.NewRecorder()
	handler.ServeHTTP(w, httptest.NewRequest("GET", "/api/state", nil))
	if w.Code != http.StatusUnauthorized {
		t.Fatal(w.Code)
	}
	r := httptest.NewRequest("POST", "/api/builds/"+b.ID+"/cancel", nil)
	r.Header.Set("Authorization", "Bearer secret")
	w = httptest.NewRecorder()
	handler.ServeHTTP(w, r)
	if w.Code != 200 || c.state.Builds[b.ID].Status != "cancelled" {
		t.Fatal(w.Code)
	}
	b, _ = c.Submit(request())
	finish(c, b.ID, 10)
	for i := 0; i < 2; i++ {
		r = httptest.NewRequest("POST", "/api/builds/"+b.ID+"/promote", strings.NewReader(`{"version":"v1"}`))
		r.Header.Set("Authorization", "Bearer secret")
		w = httptest.NewRecorder()
		handler.ServeHTTP(w, r)
		expected := 201
		if i == 1 {
			expected = 409
		}
		if w.Code != expected {
			t.Fatalf("got %d: %s", w.Code, w.Body)
		}
	}
}
func TestFailureSignatureNormalization(t *testing.T) {
	a := Signature("/tmp/run_a/source/src/main.cpp:12:8: error: bad operand 0xabcdef")
	b := Signature("/tmp/run_b/source/src/main.cpp:42:9: error: bad operand 0x123456")
	if a != b {
		t.Fatalf("%q != %q", a, b)
	}
}
func TestPersistenceFailureRollsBack(t *testing.T) {
	c := fixture(t)
	bad := filepath.Join(t.TempDir(), "file")
	if err := os.WriteFile(bad, []byte("x"), 0600); err != nil {
		t.Fatal(err)
	}
	c.store.Dir = bad
	if _, err := c.Submit(request()); err == nil {
		t.Fatal("write failure accepted")
	}
	if len(c.state.Builds) != 0 {
		t.Fatal("failed transaction changed state")
	}
}
func TestMetricsAndJSON(t *testing.T) {
	c := fixture(t)
	b, _ := c.Submit(request())
	w := httptest.NewRecorder()
	c.Handler("", "").ServeHTTP(w, httptest.NewRequest("GET", "/metrics", nil))
	if !strings.Contains(w.Body.String(), "regressgrid_queue_depth 2") {
		t.Fatal(w.Body)
	}
	encoded, _ := json.Marshal(c.detail(b.ID))
	if !json.Valid(encoded) {
		t.Fatal("invalid dashboard payload")
	}
}
