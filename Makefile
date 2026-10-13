.PHONY: build test dev proto e2e up down worker-tsan worker-aarch64 worker-aarch64-binaries bench-spsc bench-vector benchmark-sanitizers
build:
	go build -o build/regressgrid ./cmd/server
	go build -o build/rg ./cmd/rg
	cmake -S worker -B build/worker -G Ninja -DCMAKE_BUILD_TYPE=Debug
	cmake --build build/worker --parallel 2
	npm ci --prefix web
	npm run build --prefix web
test:
	go test -race ./...
	ctest --test-dir build/worker --output-on-failure
e2e:
	python3 scripts/e2e.py
dev:
	python3 scripts/dev.py
up:
	docker compose up --build -d --scale worker=2
down:
	docker compose down
proto:
	go install google.golang.org/protobuf/cmd/protoc-gen-go@v1.36.6
	go install google.golang.org/grpc/cmd/protoc-gen-go-grpc@v1.5.1
	PATH="$(shell go env GOPATH)/bin:$$PATH" protoc --go_out=. --go_opt=module=regressgrid --go-grpc_out=. --go-grpc_opt=module=regressgrid proto/grid.proto
worker-tsan:
	cmake -S worker -B build/tsan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DRG_TSAN=ON
	cmake --build build/tsan --parallel 2
	ctest --test-dir build/tsan --output-on-failure
# Cross-compile the worker for aarch64 and run its GoogleTest suite under qemu-user.
worker-aarch64:
	docker build --platform linux/amd64 -f Dockerfile.worker-aarch64 --target test .
worker-aarch64-binaries:
	docker build --platform linux/amd64 -f Dockerfile.worker-aarch64 --target binaries -o build/aarch64 .
bench-spsc:
	cmake -S worker -B build/bench -G Ninja -DCMAKE_BUILD_TYPE=Release
	cmake --build build/bench --target spsc-bench
	./build/bench/spsc-bench
bench-vector:
	cmake -S examples/benchmark -B build/benchmark -G Ninja -DCMAKE_BUILD_TYPE=Release
	cmake --build build/benchmark --parallel 2
	cd build/benchmark && ./regressgrid-bench
benchmark-sanitizers:
	cmake -S examples/benchmark -B build/benchmark-sanitizers -G Ninja -DCMAKE_BUILD_TYPE=Debug -DRG_SANITIZERS=ON
	cmake --build build/benchmark-sanitizers --parallel 2
	UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build/benchmark-sanitizers --output-on-failure
