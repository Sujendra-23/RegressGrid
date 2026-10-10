export type Status = 'queued' | 'running' | 'passed' | 'failed' | 'cancelled';
export interface Configuration {
  compiler: string;
  build_type: string;
}
export interface Metrics {
  build_seconds: number;
  test_seconds: number;
  peak_memory_bytes: number;
  cpu_seconds: number;
  warnings: number;
  tests_passed: number;
  tests_failed: number;
  binary_bytes: number;
  latency_ms: number;
  throughput: number;
  cache_hit: boolean;
}
export interface Artifact {
  name: string;
  sha256: string;
  size: number;
}
export interface Job extends Configuration {
  id: string;
  build_id: string;
  status: Status;
  worker_id?: string;
  attempt: number;
  started_at?: string;
  finished_at?: string;
  metrics: Metrics;
  error?: string;
  failure_signature?: string;
  artifacts: Artifact[];
}
export interface Change {
  job_id: string;
  metric: string;
  baseline: number;
  candidate: number;
  percent: number;
  regression: boolean;
}
export interface Build {
  id: string;
  repository: string;
  commit: string;
  configurations: Configuration[];
  baseline_id?: string;
  timeout_seconds: number;
  plugin: string;
  status: Status;
  created_at: string;
  job_ids: string[];
  jobs: Job[];
  comparison: { baseline_id?: string; status: string; changes: Change[]; reason?: string };
}
export interface Worker {
  id: string;
  compilers: string[];
  slots: number;
  platform: string;
  verilator: boolean;
  last_seen: string;
  healthy: boolean;
  active_jobs: number;
}
export interface Release {
  id: string;
  build_id: string;
  version: string;
  created_at: string;
}
export interface GridState {
  builds: Build[];
  workers: Worker[];
  releases: Release[];
  regression_threshold: number;
}
