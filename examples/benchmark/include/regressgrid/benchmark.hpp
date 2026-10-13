#pragma once

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <limits>
#include <locale>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace rg::bench {
struct Options {
  std::string backend = "cpu";
  std::string scalar = "float";
  std::size_t elements = 1U << 20;
  std::size_t warmup = 5;
  std::size_t iterations = 30;
  std::string output = "regressgrid-metrics.json";
};

inline std::size_t parse_count(std::string_view value) {
  if (value.empty()) throw std::invalid_argument("empty count");
  std::size_t count{};
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), count);
  if (error != std::errc{} || end != value.data() + value.size())
    throw std::invalid_argument("invalid count: " + std::string(value));
  return count;
}

inline void validate(const Options &options) {
  if (options.backend != "cpu" && options.backend != "cuda")
    throw std::invalid_argument("backend must be cpu or cuda");
  if (options.scalar != "float" && options.scalar != "double")
    throw std::invalid_argument("type must be float or double");
  if (options.elements == 0 || options.iterations == 0)
    throw std::invalid_argument("elements and iterations must be positive");
  if (options.output.empty())
    throw std::invalid_argument("output path must not be empty");
}

inline Options parse_options(std::span<const std::string_view> args) {
  Options options;
  for (std::size_t i = 0; i < args.size(); i += 2) {
    if (i + 1 == args.size())
      throw std::invalid_argument("missing value for " + std::string(args[i]));
    const auto key = args[i], value = args[i + 1];
    if (key == "--backend") options.backend = value;
    else if (key == "--type") options.scalar = value;
    else if (key == "--elements") options.elements = parse_count(value);
    else if (key == "--warmup") options.warmup = parse_count(value);
    else if (key == "--iterations") options.iterations = parse_count(value);
    else if (key == "--output") options.output = value;
    else throw std::invalid_argument("unknown option: " + std::string(key));
  }
  validate(options);
  return options;
}

template <class T> std::size_t byte_count(std::size_t elements) {
  static_assert(std::is_same_v<T, float> || std::is_same_v<T, double>);
  if (elements == 0 || elements > std::numeric_limits<std::size_t>::max() / sizeof(T))
    throw std::length_error("invalid vector size");
  return elements * sizeof(T);
}

template <class T> struct Vectors {
  std::vector<T> a, b, out;
  explicit Vectors(std::size_t elements) {
    (void)byte_count<T>(elements);
    a.resize(elements);
    b.resize(elements);
    // A missing write must fail verification, including at a partial CUDA block.
    out.assign(elements, std::numeric_limits<T>::quiet_NaN());
    for (std::size_t i = 0; i < elements; ++i) {
      a[i] = static_cast<T>(i % 127) / T{4};
      b[i] = static_cast<T>(i % 63) / T{8};
    }
  }
  void add() {
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = a[i] + b[i];
    // Make each iteration's writes observable to the optimizer, without
    // reading the whole output or computing a checksum inside the timed region.
    asm volatile("" : : "g"(out.data()) : "memory");
  }
  void verify() const {
    for (std::size_t i = 0; i < out.size(); ++i)
      if (!std::isfinite(out[i]) || out[i] != a[i] + b[i])
        throw std::runtime_error("vector mismatch at element " + std::to_string(i));
  }
};

class HostTimer {
  using Clock = std::chrono::steady_clock;
  Clock::time_point start_{};
public:
  void synchronize() const noexcept {}
  void start() { start_ = Clock::now(); }
  double stop() const {
    return std::chrono::duration<double, std::milli>(Clock::now() - start_).count();
  }
};

// Timer policy: synchronize(), start(), stop() returning milliseconds.
// CUDA stop waits for the stop event, so each sample is complete before reuse.
template <class Workload, class Timer>
std::vector<double> measure(const Options &options, Workload &&workload, Timer &timer) {
  validate(options);
  std::vector<double> samples;
  samples.reserve(options.iterations);
  for (std::size_t i = 0; i < options.warmup; ++i) workload();
  timer.synchronize();
  for (std::size_t i = 0; i < options.iterations; ++i) {
    timer.start();
    workload();
    const double ms = timer.stop();
    if (!std::isfinite(ms) || ms <= 0)
      throw std::runtime_error("timer returned a nonpositive or nonfinite sample; increase workload");
    samples.push_back(ms);
  }
  return samples;
}

struct Result {
  double latency_ms{}, throughput{}, min_ms{}, p95_ms{};
  std::vector<double> samples_ms;
};

inline Result summarize(std::size_t elements, std::vector<double> samples) {
  if (elements == 0 || samples.empty()) throw std::invalid_argument("empty benchmark");
  for (double ms : samples)
    if (!std::isfinite(ms) || ms <= 0) throw std::invalid_argument("invalid sample");
  auto sorted = samples;
  std::sort(sorted.begin(), sorted.end());
  const auto n = sorted.size();
  double median = sorted[n / 2];
  if (n % 2 == 0) median = sorted[n / 2 - 1] + (median - sorted[n / 2 - 1]) / 2;
  const double throughput = static_cast<double>(elements) / (median / 1000.0);
  if (!std::isfinite(throughput)) throw std::overflow_error("throughput overflow");
  // Nearest-rank p95, without multiplying a size_t by 95.
  const auto rank = n - n / 20;
  return {median, throughput, sorted.front(), sorted[rank - 1], std::move(samples)};
}

inline void write_json(std::ostream &out, const Options &options, const Result &result) {
  out.imbue(std::locale::classic());
  out << std::setprecision(17)
      << "{\"latency_ms\":" << result.latency_ms << ",\"throughput\":" << result.throughput
      << ",\"backend\":\"" << options.backend << "\",\"type\":\"" << options.scalar
      << "\",\"elements\":" << options.elements << ",\"warmup\":" << options.warmup
      << ",\"iterations\":" << options.iterations
      << ",\"throughput_unit\":\"elements/s\",\"min_ms\":" << result.min_ms
      << ",\"p95_ms\":" << result.p95_ms << ",\"samples_ms\":[";
  for (std::size_t i = 0; i < result.samples_ms.size(); ++i) {
    if (i != 0) out << ',';
    out << result.samples_ms[i];
  }
  out << "]}\n";
  if (!out) throw std::runtime_error("cannot write benchmark metrics");
}

template <class T> Result run_cpu(const Options &options) {
  Vectors<T> vectors(options.elements);
  HostTimer timer;
  auto samples = measure(options, [&] { vectors.add(); }, timer);
  vectors.verify();
  return summarize(options.elements, std::move(samples));
}

#ifdef RG_CUDA
Result run_cuda(const Options &options);
#endif
} // namespace rg::bench
