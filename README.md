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
