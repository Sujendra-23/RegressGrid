#!/bin/sh
set -eu
for example in cpp hardware; do
  if [ ! -d "/demo/$example/.git" ]; then
    mkdir -p "/demo/$example"
    cp -R "/examples/$example/." "/demo/$example/"
    git -C "/demo/$example" init -b main
    git -C "/demo/$example" add .
    git -C "/demo/$example" -c user.name=RegressGrid -c user.email=demo@regressgrid.local commit -m 'Baseline fixture'
    git -C "/demo/$example" tag baseline
  fi
done
if ! git -C /demo/cpp rev-parse regression >/dev/null 2>&1; then
  git -C /demo/cpp checkout -b regression
  sed -i 's/#define RG_LATENCY_MS 18.2/#define RG_LATENCY_MS 22.7/' /demo/cpp/main.cpp
  git -C /demo/cpp add .
  git -C /demo/cpp -c user.name=RegressGrid -c user.email=demo@regressgrid.local commit -m 'Introduce measured latency regression'
  git -C /demo/cpp checkout main
fi
for fixture in broken slow; do
  if ! git -C /demo/cpp rev-parse "$fixture" >/dev/null 2>&1; then
    git -C /demo/cpp checkout -b "$fixture" baseline
    if [ "$fixture" = broken ]; then
      { printf '#error deliberate regression fixture\n'; cat /demo/cpp/main.cpp; } > /demo/cpp/main.tmp
      mv /demo/cpp/main.tmp /demo/cpp/main.cpp
    else
      sed -i 's/#define RG_LATENCY_MS 18.2/#define RG_LATENCY_MS 30000/' /demo/cpp/main.cpp
    fi
    git -C /demo/cpp add .
    git -C /demo/cpp -c user.name=RegressGrid -c user.email=demo@regressgrid.local commit -m "$fixture fixture"
    git -C /demo/cpp checkout main
  fi
done
if ! git -C /demo/hardware rev-parse broken >/dev/null 2>&1; then
  git -C /demo/hardware checkout -b broken baseline
  sed -i "s/result = a + b;/result = a + b + 8'd1;/" /demo/hardware/alu.sv
  git -C /demo/hardware add .
  git -C /demo/hardware -c user.name=RegressGrid -c user.email=demo@regressgrid.local commit -m 'Inject failing ALU vectors'
  git -C /demo/hardware checkout main
fi
