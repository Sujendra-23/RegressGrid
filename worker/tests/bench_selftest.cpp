// Self-test for rg::bench. A plain executable (no GoogleTest) so it also runs
// where only a compiler is available; registered with CTest as bench.selftest.
#include "regressgrid/bench.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
int failures = 0;
#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                              \
    }                                                                          \
  } while (0)
#define CHECK_THROWS(expr)                                                     \
  do {                                                                         \
    bool threw = false;                                                        \
    try {                                                                      \
      (void)(expr);                                                            \
    } catch (const std::invalid_argument &) {                                  \
      threw = true;                                                            \
    }                                                                          \
    CHECK(threw);                                                              \
  } while (0)

std::vector<double> one_to_hundred() {
  std::vector<double> v(100);
  for (int i = 0; i < 100; ++i) v[static_cast<std::size_t>(i)] = i + 1;
  return v;
}

void percentile_uses_nearest_rank() {
  const auto v = one_to_hundred();
  CHECK(rg::bench::percentile(v, 0) == 1);
  CHECK(rg::bench::percentile(v, 50) == 50);
  CHECK(rg::bench::percentile(v, 95) == 95);
  CHECK(rg::bench::percentile(v, 99) == 99);
  CHECK(rg::bench::percentile(v, 100) == 100);
  CHECK(rg::bench::percentile({7.0}, 99) == 7.0);
  CHECK_THROWS(rg::bench::percentile({}, 50));
  CHECK_THROWS(rg::bench::percentile(v, 101));
  CHECK_THROWS(rg::bench::percentile(v, std::nan("")));
}

void summarize_computes_known_values() {
  auto v = one_to_hundred();
  std::vector<double> shuffled(v.rbegin(), v.rend()); // order must not matter
  const auto st = rg::bench::summarize(shuffled);
  CHECK(st.n == 100);
  CHECK(st.min == 1 && st.max == 100);
  CHECK(st.p50 == 50 && st.p95 == 95 && st.p99 == 99);
  CHECK(std::fabs(st.mean - 50.5) < 1e-12);
  CHECK(std::fabs(st.stddev - std::sqrt((100.0 * 100.0 - 1.0) / 12.0)) < 1e-9);
  CHECK_THROWS(rg::bench::summarize({}));
  CHECK_THROWS(rg::bench::summarize({1.0, std::numeric_limits<double>::infinity()}));
  CHECK_THROWS(rg::bench::summarize({1.0, std::nan("")}));
  CHECK_THROWS(rg::bench::summarize({1.0, -2.0}));
}

void scoped_timer_records_once_and_can_cancel() {
  std::vector<double> sink;
  sink.reserve(2);
  { rg::bench::ScopedTimer t(sink); }
  CHECK(sink.size() == 1 && sink[0] >= 0.0);
  {
    rg::bench::ScopedTimer t(sink);
    t.cancel();
  }
  CHECK(sink.size() == 1);
  std::vector<double> full;
  full.reserve(1);
  full.push_back(0.0);
  { rg::bench::ScopedTimer t(full); }
  CHECK(full.size() == 1 && full.capacity() == 1); // no room, so no allocation
}

void measure_runs_warmup_plus_iterations() {
  int calls = 0;
  const auto samples = rg::bench::measure([&] { ++calls; }, {.warmup = 3, .iterations = 7});
  CHECK(calls == 10);
  CHECK(samples.size() == 7);
  for (double s : samples) CHECK(s >= 0.0 && std::isfinite(s));
  long long total = 0;
  const auto valued = rg::bench::measure([&] { return ++total; }, {.warmup = 0, .iterations = 4});
  CHECK(valued.size() == 4 && total == 4);
  CHECK_THROWS(rg::bench::measure([] {}, {.warmup = 1, .iterations = 0}));
}

void metrics_json_has_the_fields_the_worker_reads() {
  rg::bench::Stats st;
  st.n = 10;
  st.p50 = 2e6; // 2 ms
  st.p95 = 3e6;
  st.p99 = 4e6;
  st.stddev = 5e5;
  const std::string json = rg::bench::metrics_json(st);
  CHECK(json.find("\"latency_ms\":2,") != std::string::npos);
  CHECK(json.find("\"throughput\":500,") != std::string::npos);
  CHECK(json.find("\"p95_ms\":3,") != std::string::npos);
  CHECK(json.find("\"p99_ms\":4,") != std::string::npos);
  CHECK(json.find("\"samples\":10}") != std::string::npos);
  CHECK(json.back() == '\n');
  rg::bench::Stats zero;
  CHECK_THROWS(rg::bench::metrics_json(zero));
}

void write_metrics_round_trips_and_reports_failure() {
  const auto dir = std::filesystem::temp_directory_path();
  const auto path = (dir / "rg-bench-selftest-metrics.json").string();
  rg::bench::Stats st;
  st.n = 3;
  st.p50 = 1e6;
  CHECK(rg::bench::write_metrics(path, st));
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  CHECK(ss.str() == rg::bench::metrics_json(st));
  std::filesystem::remove(path);
  CHECK(!rg::bench::write_metrics((dir / "no-such-dir-rg" / "m.json").string(), st));
}
} // namespace

int main() {
  try {
    percentile_uses_nearest_rank();
    summarize_computes_known_values();
    scoped_timer_records_once_and_can_cancel();
    measure_runs_warmup_plus_iterations();
    metrics_json_has_the_fields_the_worker_reads();
    write_metrics_round_trips_and_reports_failure();
  } catch (const std::exception &e) {
    std::fprintf(stderr, "unexpected exception: %s\n", e.what());
    return 1;
  }
  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::puts("bench selftest passed");
  return 0;
}
