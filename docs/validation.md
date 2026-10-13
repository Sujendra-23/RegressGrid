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

## C++ and CUDA benchmark driver

Validated on October 12–13, 2026, independently of the worker and full-stack checks
above. The new benchmark project does not change the existing regression fixtures.

| Check | Result | Environment / scope |
| --- | --- | --- |
| CPU Release CTest | 4 passed | AppleClang 21, macOS arm64; unit suite, CLI/JSON, float/double |
| Host ASan + UBSan | 4 passed | AppleClang 21, macOS arm64; leak detection unavailable on this platform |
| Host ASan + UBSan + LSan | 4 passed | GCC 11.4, Ubuntu 22.04 pod; `detect_leaks=1`, `halt_on_error=1` |
| CUDA Release CTest | 6 passed | CUDA 12.4.131, GCC 11.4, RTX 4090 (sm_89), driver 580.159.04 |
| Compute Sanitizer memcheck | 0 errors; 0 bytes leaked | Float/double, 1,025 and 1,048,579 elements, two warmups and five timed iterations per run; `--leak-check full --error-exitcode 1` |

The unit suite includes heap-backed simulated CUDA resources, ownership moves,
partial construction cleanup, and exception cleanup for all 18 fallible operations
in a two-sample run. It checks event ordering (start, launch, stop, synchronize,
elapsed), warmup isolation, input validation, allocation overflow, vector
verification, sample validation, statistics, and JSON write failures. CLI checks
also verify errors exit with status 1 and do not publish metrics.

The pod's default GCC 11 PIE ASan binaries encountered repeated
`AddressSanitizer:DEADLYSIGNAL` messages and stalled. A separate non-PIE build
passed with all sanitizer checks enabled; no sanitizer checks were disabled:

```sh
cmake -S examples/benchmark -B build/benchmark-sanitizers -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DRG_SANITIZERS=ON \
  -DCMAKE_CXX_FLAGS=-fno-pie -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build build/benchmark-sanitizers --parallel 2
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/benchmark-sanitizers --output-on-failure --parallel 2
```

These additional flags were needed for the pod validation build. CI uses the
same non-PIE sanitizer configuration and is configured to build the host suite
with GCC and Clang, with and without ASan/UBSan; that GitHub Actions matrix was
not executed in this session.
GPU sanitizer timings are instrumentation-affected and are not performance
baseline measurements.
