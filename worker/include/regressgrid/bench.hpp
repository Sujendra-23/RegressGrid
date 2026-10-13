#pragma once
// Small benchmark toolkit for C++ workloads that run under RegressGrid.
//
// measure() times a callable with a steady clock after a warm-up, summarize()
// turns the samples into percentiles, and write_metrics() emits the
// regressgrid-metrics.json file the worker uploads and the control plane gates
// on (latency_ms and throughput, plus extra percentile fields).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace rg::bench {
using Clock = std::chrono::steady_clock;

// Keeps the compiler from deleting the computation that produced `value`.
template <typename T> inline void do_not_optimize(const T &value) {
#if defined(__GNUC__) || defined(__clang__)
  const void *p = std::addressof(value);
  asm volatile("" : : "r"(p) : "memory");
#else
  static volatile const void *sink;
  sink = std::addressof(value);
#endif
}

// RAII timer: appends the scope's duration in nanoseconds to `sink` when it is
// destroyed, unless cancel() was called. It never allocates in the destructor,
// so the sink must have reserved room, otherwise the sample is dropped.
class ScopedTimer {
public:
  explicit ScopedTimer(std::vector<double> &sink)
      : sink_(&sink), start_(Clock::now()) {}
  ScopedTimer(const ScopedTimer &) = delete;
  ScopedTimer &operator=(const ScopedTimer &) = delete;
  ~ScopedTimer() {
    if (sink_ && sink_->size() < sink_->capacity())
      sink_->push_back(elapsed_ns());
  }
  double elapsed_ns() const {
    return std::chrono::duration<double, std::nano>(Clock::now() - start_)
        .count();
  }
  void cancel() { sink_ = nullptr; }

private:
  std::vector<double> *sink_;
  Clock::time_point start_;
};

struct Options {
  std::size_t warmup = 5;
  std::size_t iterations = 50;
};

// Runs `fn` `warmup` times untimed, then `iterations` times timed. Returns one
// duration in nanoseconds per timed call.
template <std::invocable Fn>
std::vector<double> measure(Fn &&fn, const Options &options = {}) {
  if (options.iterations == 0)
    throw std::invalid_argument("bench: iterations must be at least 1");
  using Result = std::invoke_result_t<Fn &>;
  auto run_once = [&] {
    if constexpr (std::is_void_v<Result>) {
      fn();
    } else {
      auto result = fn();
      do_not_optimize(result);
    }
  };
  for (std::size_t i = 0; i < options.warmup; ++i)
    run_once();
  std::vector<double> samples;
  samples.reserve(options.iterations);
  for (std::size_t i = 0; i < options.iterations; ++i) {
    ScopedTimer timer(samples);
    run_once();
  }
  return samples;
}

struct Stats {
  std::size_t n = 0;
  double min = 0, p50 = 0, p95 = 0, p99 = 0, max = 0, mean = 0, stddev = 0;
};

// Nearest-rank percentile of an ascending, non-empty vector (p in 0..100).
inline double percentile(const std::vector<double> &sorted, double p) {
  if (sorted.empty())
    throw std::invalid_argument("bench: percentile of an empty sample set");
  if (!(p >= 0.0 && p <= 100.0))
    throw std::invalid_argument("bench: percentile must be within 0..100");
  const auto rank =
      static_cast<std::size_t>(std::ceil(p / 100.0 * static_cast<double>(sorted.size())));
  return sorted[rank == 0 ? 0 : std::min(rank, sorted.size()) - 1];
}

// Summary statistics; stddev is the population standard deviation.
inline Stats summarize(std::vector<double> samples) {
  if (samples.empty())
    throw std::invalid_argument("bench: no samples to summarize");
  for (double s : samples)
    if (!std::isfinite(s) || s < 0.0)
      throw std::invalid_argument("bench: samples must be finite and non-negative");
  std::sort(samples.begin(), samples.end());
  Stats st;
  st.n = samples.size();
  st.min = samples.front();
  st.max = samples.back();
  st.p50 = percentile(samples, 50);
  st.p95 = percentile(samples, 95);
  st.p99 = percentile(samples, 99);
  st.mean = std::accumulate(samples.begin(), samples.end(), 0.0) /
            static_cast<double>(st.n);
  double var = 0.0;
  for (double s : samples)
    var += (s - st.mean) * (s - st.mean);
  st.stddev = std::sqrt(var / static_cast<double>(st.n));
  return st;
}

// JSON in the format the worker reads: latency_ms is the median, throughput is
// calls per second at that median, extra fields carry the tail.
inline std::string metrics_json(const Stats &st) {
  const double latency_ms = st.p50 / 1e6;
  if (!(latency_ms > 0.0))
    throw std::invalid_argument("bench: median latency is zero, cannot derive throughput");
  char buf[256];
  std::snprintf(buf, sizeof buf,
                "{\"latency_ms\":%.9g,\"throughput\":%.9g,\"p95_ms\":%.9g,"
                "\"p99_ms\":%.9g,\"stddev_ms\":%.9g,\"samples\":%zu}\n",
                latency_ms, 1000.0 / latency_ms, st.p95 / 1e6, st.p99 / 1e6,
                st.stddev / 1e6, st.n);
  return buf;
}

// Writes metrics_json(st) to `path`. Returns false if the file cannot be written.
inline bool write_metrics(const std::string &path, const Stats &st) {
  const std::string json = metrics_json(st);
  std::ofstream f(path, std::ios::trunc);
  f << json;
  return static_cast<bool>(f);
}
} // namespace rg::bench
