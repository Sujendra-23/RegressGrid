package control

import (
	"crypto/rand"
	"encoding/hex"
	"regexp"
	"strings"
	"time"
)

type Config struct {
	Compiler  string `json:"compiler"`
	BuildType string `json:"build_type"`
}
type Submission struct {
	Repository     string   `json:"repository"`
	Commit         string   `json:"commit"`
	Configurations []Config `json:"configurations"`
	BaselineID     string   `json:"baseline_id,omitempty"`
	TimeoutSeconds int      `json:"timeout_seconds"`
	Plugin         string   `json:"plugin"`
}
type Metrics struct {
	BuildSeconds    float64 `json:"build_seconds"`
	TestSeconds     float64 `json:"test_seconds"`
	PeakMemoryBytes uint64  `json:"peak_memory_bytes"`
	CPUSeconds      float64 `json:"cpu_seconds"`
	Warnings        uint32  `json:"warnings"`
	TestsPassed     uint32  `json:"tests_passed"`
	TestsFailed     uint32  `json:"tests_failed"`
	BinaryBytes     uint64  `json:"binary_bytes"`
	LatencyMS       float64 `json:"latency_ms"`
	Throughput      float64 `json:"throughput"`
	CacheHit        bool    `json:"cache_hit"`
}
type Artifact struct {
	Name   string `json:"name"`
	SHA256 string `json:"sha256"`
	Size   int64  `json:"size"`
}
type Job struct {
	ID      string `json:"id"`
	BuildID string `json:"build_id"`
	Config
	Status           string     `json:"status"`
	WorkerID         string     `json:"worker_id,omitempty"`
	Lease            string     `json:"-"`
	Attempt          int        `json:"attempt"`
	StartedAt        *time.Time `json:"started_at,omitempty"`
	FinishedAt       *time.Time `json:"finished_at,omitempty"`
	Metrics          Metrics    `json:"metrics"`
	Error            string     `json:"error,omitempty"`
	FailureSignature string     `json:"failure_signature,omitempty"`
	Artifacts        []Artifact `json:"artifacts"`
}

// Lease is persisted separately from the API representation.
type PersistedJob struct {
	Job
	SavedLease string `json:"lease"`
}
type Build struct {
	ID string `json:"id"`
	Submission
	Status    string    `json:"status"`
	CreatedAt time.Time `json:"created_at"`
	JobIDs    []string  `json:"job_ids"`
}
type Worker struct {
	ID         string    `json:"id"`
	Compilers  []string  `json:"compilers"`
	Slots      int       `json:"slots"`
	Platform   string    `json:"platform"`
	Verilator  bool      `json:"verilator"`
	LastSeen   time.Time `json:"last_seen"`
	Healthy    bool      `json:"healthy"`
	ActiveJobs int       `json:"active_jobs"`
}
type Release struct {
	ID        string    `json:"id"`
	BuildID   string    `json:"build_id"`
	Version   string    `json:"version"`
	CreatedAt time.Time `json:"created_at"`
}
type State struct {
	Builds     map[string]*Build        `json:"builds"`
	Jobs       map[string]*PersistedJob `json:"jobs"`
	Workers    map[string]*Worker       `json:"workers"`
	Releases   []Release                `json:"releases"`
	Heartbeats uint64                   `json:"heartbeats"`
}
type Change struct {
	JobID      string  `json:"job_id"`
	Metric     string  `json:"metric"`
	Baseline   float64 `json:"baseline"`
	Candidate  float64 `json:"candidate"`
	Percent    float64 `json:"percent"`
	Regression bool    `json:"regression"`
}
type Comparison struct {
	BaselineID string   `json:"baseline_id"`
	Status     string   `json:"status"`
	Changes    []Change `json:"changes"`
	Reason     string   `json:"reason,omitempty"`
}
type BuildDetail struct {
	*Build
	Jobs       []Job      `json:"jobs"`
	Comparison Comparison `json:"comparison"`
}

func id(prefix string) string {
	b := make([]byte, 8)
	if _, err := rand.Read(b); err != nil {
		panic(err)
	}
	return prefix + hex.EncodeToString(b)
}

var addressRE = regexp.MustCompile(`0x[0-9a-fA-F]+`)
var pathRE = regexp.MustCompile(`(?:/[^\s:]+)+/(?:source|build)/`)
var lineRE = regexp.MustCompile(`:\d+(?::\d+)?`)
var spacesRE = regexp.MustCompile(`\s+`)

func Signature(s string) string {
	s = strings.ReplaceAll(s, "#error ", "")
	for _, line := range strings.Split(s, "\n") {
		if strings.Contains(line, "error:") || strings.Contains(line, "FAILED:") || strings.Contains(line, "failed") {
			s = line
			break
		}
	}
	s = pathRE.ReplaceAllString(s, "")
	s = addressRE.ReplaceAllString(s, "0xADDR")
	s = lineRE.ReplaceAllString(s, ":LINE")
	s = strings.TrimSpace(spacesRE.ReplaceAllString(s, " "))
	if len(s) > 300 {
		s = s[:300]
	}
	return s
}
func newState() State {
	return State{Builds: map[string]*Build{}, Jobs: map[string]*PersistedJob{}, Workers: map[string]*Worker{}, Releases: []Release{}}
}
func terminal(s string) bool { return s == "passed" || s == "failed" || s == "cancelled" }
