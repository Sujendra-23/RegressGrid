.PHONY: build test dev proto e2e up down
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
