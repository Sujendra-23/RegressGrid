# RegressGrid

A distributed C++ build, regression, and release platform. Submit a repository
commit and compiler matrix, follow real worker output, compare measurements to
a passed baseline, and promote checksum-verified artifacts.

The execution worker is C++20: a `std::jthread` pool, POSIX process groups,
nonblocking output capture, cancellation, timeouts, resource accounting, RAII
cleanup, and SHA-256 streaming uploads. Go handles scheduling, authenticated
REST/gRPC, durable state, worker leases, retries, regression gates, and releases.
React/TypeScript provides the dashboard.

## Start the full stack

Requires Docker Desktop or Docker Engine with Compose.

```sh
docker compose up --build -d --scale worker=2
```

Open **http://localhost:8080**. Choose **Connection**, enter `local-api-token`,
and connect. The default stack starts PostgreSQL, Redis, two C++ workers,
Prometheus, Grafana, and local Git demo repositories. Jobs are real builds;
the dashboard does not seed fake execution data.

Submit the included four-configuration example with the CLI:

```sh
go run ./cmd/rg -token local-api-token submit examples/submission.json
go run ./cmd/rg -token local-api-token status
```

Or use the dashboard with repository `file:///demo/cpp`, commit `baseline`,
and GCC/Clang × Debug/Release. A passing build provides a binary archive,
JUnit test report, and measured benchmark JSON. To demonstrate a regression,
submit commit `regression` in the same repository and select the first build
as its baseline. The candidate's controlled workload takes longer and blocks
release promotion.

For the optional hardware plugin, submit `file:///demo/hardware`, commit
`baseline`, and the **Verilator / CMake** plugin. The worker compiles a
SystemVerilog ALU into C++, executes vector suites, and uploads VCD waveforms.

Other local endpoints:

| Service | URL | Local login |
| --- | --- | --- |
| Dashboard / API | http://localhost:8080 | `local-api-token` |
| Prometheus | http://localhost:9091 | — |
| Grafana | http://localhost:3000 | `admin` / `local-grafana-password` |

The Grafana dashboard is provisioned automatically. Stop the stack with
`docker compose down`; named volumes preserve builds, artifacts, and demo repos.

Local credentials can be overridden using `GRID_API_TOKEN`,
`GRID_WORKER_TOKEN`, `GRID_DB_PASSWORD`, and `GRID_GRAFANA_PASSWORD` in the shell.
The existing workspace `.env` is not modified. Ports bind to loopback. If the defaults are occupied, override the host ports:

```sh
GRID_HTTP_PORT=8088 GRID_GRAFANA_PORT=3008 GRID_PROMETHEUS_PORT=9098 \
  docker compose up -d --scale worker=2
```

For that configuration use `http://localhost:8088` and pass
`-server http://localhost:8088` to the CLI or `GRID_URL=http://localhost:8088`
to the Compose end-to-end test.

## Build and run natively

Requires Go ≥1.24, Node ≥22, a C++20 compiler, CMake ≥3.22, Ninja, Git,
gRPC/protobuf, OpenSSL, GoogleTest, and nlohmann-json.

```sh
# macOS dependencies
HOMEBREW_NO_AUTO_UPDATE=1 brew install grpc googletest ninja nlohmann-json

# Ubuntu 24.04 dependencies
sudo apt-get install build-essential clang cmake ninja-build libgrpc++-dev \
  protobuf-compiler-grpc libprotobuf-dev libssl-dev libgtest-dev nlohmann-json3-dev

make build
make test
make dev
```

Native development binds HTTP to `127.0.0.1:8080` and gRPC to
`127.0.0.1:9090`, uses a durable file under `.local/data`, and permits local
access without a token. Set `DATABASE_URL` and `REDIS_URL` to use real services.
Use `npm run dev --prefix web` for dashboard hot reload; Vite proxies the API.
Workers report actual compiler capabilities. macOS's `g++` alias for Clang is
not reported as GCC; use the Docker workers for the GCC/Clang matrix.

If an installed macOS SDK and Xcode linker disagree, explicitly select a
compatible SDK, for example:

```sh
export SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk
export MACOSX_DEPLOYMENT_TARGET=26.0
cmake -S worker -B build/worker -G Ninja \
  -DCMAKE_PREFIX_PATH=/opt/homebrew -DCMAKE_OSX_SYSROOT="$SDKROOT" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET"
```

## Repository contract

A submitted repository must have a root `CMakeLists.txt`, register at least
one CTest test, and define CMake install rules. Builds run with Ninja,
`BUILD_TESTING=ON`, the selected compiler/build type, and a private install prefix.
Private repository authentication, submodules, and Git LFS are not implemented.

Tests may write `regressgrid-metrics.json` into the build directory:

```json
{"latency_ms":18.2,"throughput":54.95}
```

These must be measured values; the example measures a controlled workload.
Other metrics come from actual execution. Missing optional benchmark values
are shown as unavailable. Release promotion requires all configurations to
pass, verified artifacts, and a passing comparison when a baseline is selected.
The default regression threshold is 10%; override with `REGRESSION_THRESHOLD`.

## Verify

```sh
go test -race ./...
go vet ./...
ctest --test-dir build/worker --output-on-failure
npm run build --prefix web

# Starts an isolated native server and two real C++ workers
python3 scripts/e2e.py --load 4

# Tests a running Docker stack with PostgreSQL/Redis and full compiler matrix
python3 scripts/e2e.py --compose --hardware --load 4
```

The end-to-end tests verify scheduling across workers, CTest results, live logs,
artifact checksums, measured latency regressions, release gating, authentication,
and concurrent submissions. Both native and Compose tests also exercise compilation failure
clustering, timeout, and cancellation. Hardware tests retain reports and traces
for failed vector suites. GitHub Actions runs Go race tests,
GoogleTest, clang-tidy, ASan/UBSan, Valgrind, frontend builds, image builds, and
the complete Docker/hardware integration test. To run sanitizers locally:

```sh
cmake -S worker -B build/sanitizers -G Ninja -DRG_SANITIZERS=ON
cmake --build build/sanitizers --parallel 2
ctest --test-dir build/sanitizers --output-on-failure
```

## aarch64 cross-build and lock-free log streaming

**Cross-build (aarch64).** `worker/cmake/aarch64-linux-gnu.cmake` is a CMake
toolchain file for `aarch64-linux-gnu-g++`; target libraries (gRPC, Protobuf,
OpenSSL, GoogleTest) come from Debian multiarch `:arm64` packages, and the
host `protoc`/`grpc_cpp_plugin` are passed via `RG_HOST_PROTOC` and
`RG_HOST_GRPC_PLUGIN`. `Dockerfile.worker-aarch64` wraps this in a build
pinned to `linux/amd64` (so it is a real cross build even from an Arm Mac,
slowly, under emulation) and a `test` target that runs the GoogleTest suite
with `qemu-aarch64` as the CMake cross-compiling emulator:

```sh
make worker-aarch64            # build + run all tests under qemu-aarch64
make worker-aarch64-binaries   # also writes the aarch64 binaries to build/aarch64/
```

Notes: the test stage skips one test, `Process.MissingCommandThrows`, because
`posix_spawnp` could not report a missing executable under user-mode emulation
in the environment used here (it was reported as skipped, not hidden). Whether
that skip is needed on a native x86-64 CI host was not checked. Passing under
qemu-user shows the arm64 build is functionally correct; it says nothing about
speed on Arm hardware and the binaries were not run on a real Arm machine.

**Log streaming (`SpscRing`, `LogPipe`).** `rg::SpscRing<T, N>` is a bounded
lock-free single-producer/single-consumer ring (acquire/release atomics,
cache-line-separated indices, cached opposite index). `rg::LogPipe` uses two of
them (full chunks one way, recycled chunks back) to move child-process output
from the thread running the job to a dedicated sender thread that makes the
gRPC `Write` calls, replacing the old synchronous `emit` per read. Order is
preserved, a full ring applies backpressure instead of dropping, a sink failure
(coordinator disconnect) is rethrown on the producing thread, and artifact/result
events are sent only after `flush()` has made the sender idle. Tests:
`tests/spsc_ring_test.cpp` (capacity, wraparound, 2M-item two-thread transfer,
backpressure, drain on destroy, error propagation) and
`tests/executor_log_test.cpp` (runs the real `execute()` against a temporary
local git repository and checks stage order, logs-before-result, and that emits
never overlap). Run the suite under ThreadSanitizer with `make worker-tsan`
(`-DRG_TSAN=ON`). `make bench-spsc` runs `bench/spsc_bench.cpp`; measured on a
10-core Docker Desktop Linux VM on an Apple-silicon Mac (aarch64, GCC 13.3,
`-O3`), two runs:

| | SPSC ring | mutex + condvar queue |
| --- | --- | --- |
| throughput | 528 M items/s (both runs) | 22.4 and 13.0 M items/s |
| one-way latency p50 / p99 | 42-62 ns / 62-146 ns | about 8.7-8.9 us / 11.3-12.3 us |

These are single-machine, spin-waiting numbers (the ring benchmark busy-waits;
the mutex queue blocks), so they show the cost gap, not what the worker gains
end to end: the real log path is bounded by gRPC. The end-to-end e2e
(`scripts/e2e.py`, needs Go, a server, and a Docker/Verilator setup) was not
run for this change.

## C++ / CUDA benchmark driver

[`examples/benchmark`](examples/benchmark) is a standalone C++20 project with
templated float/double vector addition, untimed warmups, per-iteration samples,
output verification, and median/min/p95 latency. It requires only CMake, Ninja,
and a C++ compiler for CPU builds; `make bench-vector` runs it in Release mode
and `make benchmark-sanitizers` runs its host tests under ASan/UBSan.

```sh
cmake -S examples/benchmark -B build/benchmark -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/benchmark
ctest --test-dir build/benchmark --output-on-failure
./build/benchmark/regressgrid-bench --type double --elements 1048576 \
  --warmup 5 --iterations 30 --output build/benchmark/regressgrid-metrics.json

# Optional NVIDIA backend: requires a C++20-capable CUDA toolkit and a GPU.
cmake -S examples/benchmark -B build/benchmark-cuda -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DRG_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=80
cmake --build build/benchmark-cuda
ctest --test-dir build/benchmark-cuda --output-on-failure
compute-sanitizer --tool memcheck --error-exitcode 1 \
  ./build/benchmark-cuda/regressgrid-bench --backend cuda --type double \
  --elements 1025 --output build/benchmark-cuda/checked.json
```

Set `CMAKE_CUDA_ARCHITECTURES` to match your GPU. CUDA uses an explicit nonblocking
stream and timing-enabled events recorded around each kernel launch, waiting for
the stop event before reading elapsed time. Allocations, transfers, verification,
and warmups are excluded. Move-only RAII owners release buffers, streams, and
events on success and exceptions. CPU uses `steady_clock` and an optimizer
barrier after each vector addition. GPU samples include effects of concurrent
GPU activity and host launch gaps; these are measurements, not performance
guarantees.

The default output is `regressgrid-metrics.json` in the current directory:
`latency_ms` is the median measured iteration and `throughput` is elements per
second derived from that median. JSON also records the backend, scalar type,
workload size, warmup/iteration counts, min/p95, and raw samples. Zero or
nonfinite timings fail with a suggestion to increase workload size. Compare
baselines using the same backend, type, size, compiler settings, and hardware.
The host suite checks input/size errors, verification, statistics, sample order,
move ownership, and cleanup after failures at every simulated CUDA operation;
real CUDA CTests exercise float/double and a partial block. Host sanitizers and
CUDA are separate builds; use NVIDIA Compute Sanitizer for device memory checks.
CTest writes CPU float metrics to the build directory's `regressgrid-metrics.json`
for worker ingestion; other smoke tests use separate files. GPU tests are labeled
`gpu`, so `ctest --test-dir build/benchmark-cuda -LE gpu` runs only host tests.
See [benchmark validation evidence](docs/validation.md#c-and-cuda-benchmark-driver)
for tested toolchains and the pod's GCC 11 sanitizer workaround.

## Configuration and API

| Variable | Native default | Purpose |
| --- | --- | --- |
| `HTTP_ADDR` | `127.0.0.1:8080` | Dashboard / REST listener |
| `GRPC_ADDR` | `127.0.0.1:9090` | Coordinator listener |
| `DATA_DIR` | `.local/data` | State fallback, logs, artifacts |
| `DATABASE_URL` | unset | PostgreSQL snapshot persistence |
| `REDIS_URL` | unset | Reconstructible queue index |
| `API_TOKEN` / `WORKER_TOKEN` | unset | Separate bearer credentials |
| `TLS_CERT` / `TLS_KEY` | unset | HTTPS and gRPC server TLS |
| `CONTROL_ADDR` | `127.0.0.1:9090` | Worker coordinator address |
| `TLS_ROOT` | unset | Worker trusted CA certificate |
| `WORKER_ID` | hostname | Unique worker identity |
| `WORKER_SLOTS` | `2` | Concurrent jobs per worker |
| `BUILD_PARALLELISM` | `2` | Ninja parallelism per job |
| `WORK_DIR` | `.local/work` | Temporary execution workspaces |
| `REGRESSION_THRESHOLD` | `10` | Relative percentage gate |

REST endpoints use `Authorization: Bearer <API_TOKEN>` when configured:

| Method | Path | Action |
| --- | --- | --- |
| GET | `/api/state` | Latest 100 builds, workers, releases |
| POST | `/api/builds` | Submit a matrix (`examples/submission.json`) |
| GET | `/api/builds/{id}` | Matrix, metrics, baseline comparison |
| POST | `/api/builds/{id}/cancel` | Cancel unfinished configurations |
| POST | `/api/builds/{id}/retry` | Create a new run preserving prior evidence |
| POST | `/api/builds/{id}/promote` | Publish `{"version":"v1.0.0"}` |
| GET | `/api/jobs/{id}/logs?stream=1` | Authenticated SSE output |
| GET | `/api/jobs/{id}/artifacts/{name}` | Verified artifact download |
| GET | `/metrics`, `/healthz`, `/readyz` | Operations endpoints |

The CLI supports `submit`, `status`, `logs`, `cancel`, `retry`, and `promote`.
Go protocol bindings are committed; `make proto` regenerates them. CMake
generates C++ bindings against the installed protobuf version.

Read [architecture and guarantees](docs/architecture.md) for state transitions,
leases, retry behavior, resource measurements, and failure recovery, or follow
the [two-minute demo](docs/demo.md).

The initial scope is one control-plane replica and multiple execution workers
for trusted repositories. Per-job security sandboxing, SSO/RBAC, statistical
benchmark gates, distributed caches, external registries, and AI triage remain
future work. This project does not claim production-scale validation or NVIDIA
affiliation.
