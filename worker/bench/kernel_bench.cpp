// Benchmark driver for small C++ kernels, built on rg::bench. Runs templated
// workloads, prints median and tail latency, and can write the
// regressgrid-metrics.json file RegressGrid's regression gate compares.
//
//   kernel-bench [--iterations N] [--warmup N] [--workload NAME --metrics PATH]
#include "regressgrid/bench.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <numeric>
#include <random>
#include <string>
#include <vector>

namespace {
template <typename T, std::uint32_t Seed> std::vector<T> random_values(std::size_t n) {
  std::mt19937 gen(Seed);
  std::vector<T> v(n);
  if constexpr (std::is_floating_point_v<T>) {
    std::uniform_real_distribution<T> d(T(0), T(1));
    for (auto &x : v) x = d(gen);
  } else {
    std::uniform_int_distribution<T> d(0, 1'000'000);
    for (auto &x : v) x = d(gen);
  }
  return v;
}

template <typename T> std::function<void()> sort_workload(std::size_t n) {
  auto input = std::make_shared<std::vector<T>>(random_values<T, 1>(n));
  return [input] {
    auto copy = *input;
    std::sort(copy.begin(), copy.end());
    rg::bench::do_not_optimize(copy.front());
  };
}

template <typename T> std::function<void()> dot_workload(std::size_t n) {
  auto a = std::make_shared<std::vector<T>>(random_values<T, 2>(n));
  auto b = std::make_shared<std::vector<T>>(random_values<T, 3>(n));
  return [a, b] {
    T r = std::inner_product(a->begin(), a->end(), b->begin(), T(0));
    rg::bench::do_not_optimize(r);
  };
}

struct Workload {
  std::string name;
  std::function<void()> fn;
};
} // namespace

int main(int argc, char **argv) {
  rg::bench::Options options;
  std::string only, metrics_path;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_value = i + 1 < argc;
    if (arg == "--iterations" && has_value) options.iterations = std::strtoull(argv[++i], nullptr, 10);
    else if (arg == "--warmup" && has_value) options.warmup = std::strtoull(argv[++i], nullptr, 10);
    else if (arg == "--workload" && has_value) only = argv[++i];
    else if (arg == "--metrics" && has_value) metrics_path = argv[++i];
    else {
      std::fprintf(stderr, "usage: %s [--iterations N] [--warmup N] [--workload NAME --metrics PATH]\n", argv[0]);
      return 2;
    }
  }
  if (!metrics_path.empty() && only.empty()) {
    std::fprintf(stderr, "--metrics needs --workload so the file describes one workload\n");
    return 2;
  }
  const std::vector<Workload> workloads = {
      {"sort_int", sort_workload<int>(100'000)},
      {"sort_double", sort_workload<double>(100'000)},
      {"dot_float", dot_workload<float>(1'000'000)},
      {"dot_double", dot_workload<double>(1'000'000)},
  };
  if (!only.empty() &&
      std::none_of(workloads.begin(), workloads.end(),
                   [&](const Workload &w) { return w.name == only; })) {
    std::fprintf(stderr, "unknown workload: %s\n", only.c_str());
    return 2;
  }
  std::printf("%-12s %10s %10s %10s %10s\n", "workload", "p50 us", "p95 us", "p99 us", "stddev us");
  for (const auto &w : workloads) {
    if (!only.empty() && w.name != only) continue;
    try {
      const auto stats = rg::bench::summarize(rg::bench::measure(w.fn, options));
      std::printf("%-12s %10.1f %10.1f %10.1f %10.1f\n", w.name.c_str(), stats.p50 / 1e3,
                  stats.p95 / 1e3, stats.p99 / 1e3, stats.stddev / 1e3);
      if (!metrics_path.empty() && !rg::bench::write_metrics(metrics_path, stats)) {
        std::fprintf(stderr, "cannot write %s\n", metrics_path.c_str());
        return 1;
      }
    } catch (const std::exception &e) {
      std::fprintf(stderr, "%s: %s\n", w.name.c_str(), e.what());
      return 1;
    }
  }
  return 0;
}
