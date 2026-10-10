# Validation evidence

Validated locally on October 9, 2026. These are development checks, not a
production-scale certification. Real Git fixtures and C++ execution were used;
dashboard values were not seeded.

| Check | Result | Scope |
| --- | --- | --- |
| Go | Passed | `go test -race ./...` and `go vet ./...` |
| React / TypeScript | Passed | Type checking and Vite production build |
| C++ GoogleTest | 11 passed | Process output, arguments, timeout, cancellation, exception cleanup, SHA-256, concurrency, JUnit parsing |
| ASan + UBSan | 11 passed | Linux worker tests, leak detection enabled |
| clang-tidy | Passed | Worker implementation and public headers, warnings treated as errors |
| Valgrind | 0 errors | SHA-256 and thread-pool tests; no definitely/indirectly/possibly lost allocations |
| Native end-to-end | Passed | Two C++ workers, Clang Debug/Release, real builds, logs, checksums, regression/release gates, failure grouping, timeout, cancellation |
| Docker end-to-end | Passed | PostgreSQL, Redis, two Linux C++ workers, GCC/Clang × Debug/Release |
| Concurrent submissions | Passed | Four simultaneous matrix submissions (16 configuration jobs) |
| Verilator | Passed | ALU compilation, three CTest suites per configuration, VCD uploads |
| Failed hardware vectors | Passed | Incorrect ALU yields failed tests; reports and traces remain downloadable and checksum verified |
| Service recovery | Passed | PostgreSQL preserves build states/releases across replacement of the control-plane container; both workers reconnect |
| Prometheus | Passed | `up{job="regressgrid"}=1` on the running stack |
| Browser | Inspected | Desktop dashboard, comparison gate, waveform view, and 390px mobile layout |

GitHub Actions is configured with these checks. It has not been run on a remote
GitHub repository in this session. The Valgrind check is intentionally limited
to the named ownership/concurrency tests, rather than claiming a full distributed
system memory audit.

The running stack uses alternative host ports because port 8080 was occupied:

- Dashboard: http://localhost:8088 (`local-api-token` in Connection)
- Prometheus: http://localhost:9098
- Grafana: http://localhost:3008 (`admin` / `local-grafana-password`)

Captured views: [desktop](dashboard.jpg) and [mobile](dashboard-mobile.jpg).
The deliberately broken fixtures remain in release/build history as evidence.
The demo recording script is in [demo.md](demo.md); a video has not been recorded.
