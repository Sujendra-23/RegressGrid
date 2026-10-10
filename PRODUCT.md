# RegressGrid

<!-- impeccable:product-schema 1 -->

## Platform
web

## Stack
The supplied brief specifies React/TypeScript, Go, C++20, gRPC, PostgreSQL, Redis, CMake, and Ninja. Local Docker Compose is the initial deployment assumption; a deployment-target question remains open.

## Users
Engineers submitting C++ repositories, investigating matrix failures and regressions, and publishing validated artifacts.

## Product Purpose
Execute repository commits across compiler/build configurations, expose live execution evidence, compare against a passed baseline, and promote verified artifacts.

## Operating Context
Internal build and regression infrastructure. A run consists of independently scheduled configuration jobs. Workers report capabilities and heartbeats.

## Capabilities and Constraints
Infrastructure-first scope: compact dashboard, real data, no invented demo metrics. Release promotion requires all jobs to pass and any baseline comparison to pass. AI triage is optional and deferred. Optional Verilator hardware regression example is included.

## Brand Commitments
RegressGrid. Clear operational language. The brief explicitly says not to spend excessive time on visual design.

## Evidence on Hand
The supplied project brief; a local C++ test repository and integration fixtures. No customer or production-scale evidence is supplied.

## Product Principles
Reliable execution before presentation. Preserve failure evidence. Show configuration-specific results. Never disguise missing data as a measurement.
