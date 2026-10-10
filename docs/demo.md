# Two-minute demonstration

Start `docker compose up -d --scale worker=2` and connect the dashboard with
`local-api-token`. The seed service creates the real Git fixture repositories.

**0:00–0:20 — Architecture.** Show the Workers tab. Two C++ workers advertise
GCC, Clang, Verilator, slots, and heartbeats. Explain that Go dispatches jobs
over authenticated gRPC and fences every attempt with a lease.

**0:20–0:50 — Run a matrix.** Submit `file:///demo/cpp`, commit `baseline`, with
four configurations. Show jobs spread across workers. Select a configuration
and follow the live clone, CMake, Ninja, and CTest output. Point to actual test
counts, resource metrics, and the measured benchmark.

**0:50–1:15 — Detect a regression.** Submit commit `regression` using the first
build as its baseline. Open Baseline comparison and select a configuration.
The longer controlled workload produces a latency regression; promotion is
disabled even though compilation and tests pass.

**1:15–1:35 — Publish evidence.** Return to the first passing build, open
Artifacts, and download its binary archive or test report. Show the SHA-256
checksum. Promote it as `demo-v1` and open Release history.

**1:35–2:00 — Hardware and observability.** Submit `file:///demo/hardware`,
commit `baseline`, with the Verilator plugin. Show the ALU vector tests and VCD
downloads. Open Grafana's RegressGrid dashboard for queue depth, failures,
utilization, and duration. Close by naming the verified retry/cancellation
tests and the deliberate single-control-plane, trusted-repository scope.

For an automated proof run:

```sh
python3 scripts/e2e.py --compose --hardware --load 4
```

This is a recording script, not a fabricated recording or production benchmark.
