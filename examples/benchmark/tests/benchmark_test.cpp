#include "regressgrid/cuda_resources.hpp"
#include <functional>
#include <iostream>
#include <sstream>

namespace {
using namespace rg::bench;

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
template <class Exception = std::exception, class Function>
void throws(Function &&function) {
  bool caught = false;
  try { function(); } catch (const Exception &) { caught = true; }
  require(caught, "expected exception was not thrown");
}

// Uses real heap allocations so ASan catches double frees and invalid accesses.
// Every fallible runtime operation supports deterministic failure injection.
struct FakeRuntime {
  using Stream = int *;
  using Event = int *;
  static inline int streams{}, events{}, buffers{}, calls{}, fail_at{}, next_event{};
  static inline std::vector<std::string> trace;
  static void step(std::string operation) {
    trace.push_back(std::move(operation));
    if (++calls == fail_at) throw std::runtime_error("injected CUDA failure");
  }
  static void reset(int failure = 0) {
    require(streams == 0 && events == 0 && buffers == 0, "leaked CUDA resource");
    calls = next_event = 0;
    fail_at = failure;
    trace.clear();
  }
  static Stream create_stream() {
    step("create stream");
    auto handle = new int(42);
    ++streams;
    return handle;
  }
  static void destroy_stream(Stream stream) noexcept { --streams; delete stream; }
  static Event create_event() {
    step("create event");
    auto handle = new int(++next_event);
    ++events;
    return handle;
  }
  static void destroy_event(Event event) noexcept { --events; delete event; }
  static void *allocate(std::size_t bytes) {
    step("allocate");
    auto pointer = ::operator new(bytes);
    ++buffers;
    return pointer;
  }
  static void free(void *pointer) noexcept { --buffers; ::operator delete(pointer); }
  static void record(Event event, Stream stream) {
    require(*stream == 42, "wrong stream");
    step("record " + std::to_string(*event));
  }
  static void synchronize_stream(Stream stream) {
    require(*stream == 42, "wrong stream");
    step("sync stream");
  }
  static void synchronize_event(Event event) { step("sync " + std::to_string(*event)); }
  static double elapsed_ms(Event start, Event stop) {
    require(*start == 1 && *stop == 2, "wrong timing events");
    step("elapsed");
    return 2.0;
  }
};

void options_test() {
  const std::vector<std::string_view> args{
      "--backend", "cuda", "--type", "double", "--elements", "1025",
      "--warmup", "0", "--iterations", "3", "--output", "metrics.json"};
  const auto options = parse_options(args);
  require(options.backend == "cuda" && options.scalar == "double" && options.elements == 1025 &&
          options.warmup == 0 && options.iterations == 3 && options.output == "metrics.json",
          "options parsed incorrectly");
  for (auto value : {"", "-1", "+1", "1.5", "1x", " 1", "184467440737095516160"})
    throws<std::invalid_argument>([&] { (void)parse_count(value); });
  for (const auto &bad : std::vector<std::vector<std::string_view>>{
           {"--elements"}, {"--elements", "0"}, {"--iterations", "0"}, {"--backend", "gpu"},
           {"--type", "int"}, {"--output", ""}, {"--unknown", "1"}})
    throws<std::invalid_argument>([&] { (void)parse_options(bad); });
  throws<std::length_error>([] { (void)byte_count<double>(std::numeric_limits<std::size_t>::max()); });
  throws<std::length_error>([] { (void)byte_count<float>(0); });
}

template <class T> void vectors_test() {
  for (std::size_t size : {1, 255, 256, 257, 1025}) {
    Vectors<T> vectors(size);
    throws<std::runtime_error>([&] { vectors.verify(); });
    vectors.add();
    vectors.verify();
    vectors.out.back() += T{1};
    throws<std::runtime_error>([&] { vectors.verify(); });
  }
}

void statistics_test() {
  auto result = summarize(100, {4, 1, 3, 2});
  require(result.latency_ms == 2.5 && result.min_ms == 1 && result.p95_ms == 4 &&
          result.throughput == 40000, "incorrect summary");
  require(result.samples_ms == std::vector<double>({4, 1, 3, 2}), "samples reordered");
  std::vector<double> twenty;
  for (int i = 1; i <= 20; ++i) twenty.push_back(i);
  require(summarize(100, twenty).p95_ms == 19, "incorrect nearest-rank p95");
  require(summarize(100, {3}).latency_ms == 3, "single sample median");
  require(summarize(100, {9, 1, 3}).latency_ms == 3, "odd median");
  for (double bad : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                     std::numeric_limits<double>::quiet_NaN()})
    throws<std::invalid_argument>([&] { (void)summarize(100, {bad}); });
  throws<std::invalid_argument>([] { (void)summarize(100, {}); });
  throws<std::invalid_argument>([] { (void)summarize(0, {1}); });
  throws<std::overflow_error>([] { (void)summarize(100, {std::numeric_limits<double>::denorm_min()}); });

  Options options;
  options.elements = 100;
  options.iterations = 4;
  std::ostringstream json;
  write_json(json, options, result);
  require(json.str().find("\"latency_ms\":2.5,\"throughput\":40000") != std::string::npos &&
          json.str().find("\"samples_ms\":[4,1,3,2]") != std::string::npos, "incorrect metrics JSON");
  std::ostringstream failed;
  failed.setstate(std::ios::badbit);
  throws<std::runtime_error>([&] { write_json(failed, options, result); });
}

struct FixedTimer {
  double ms;
  int starts = 0, stops = 0, syncs = 0;
  void synchronize() { ++syncs; }
  void start() { ++starts; }
  double stop() { ++stops; return ms; }
};

void measurement_test() {
  Options options;
  options.warmup = 2;
  options.iterations = 3;
  FixedTimer timer{1.25};
  int runs = 0;
  require(measure(options, [&] { ++runs; }, timer) == std::vector<double>(3, 1.25), "incorrect samples");
  require(runs == 5 && timer.starts == 3 && timer.stops == 3 && timer.syncs == 1,
          "warmups included in timing");
  options.warmup = 0;
  runs = 0;
  (void)measure(options, [&] { ++runs; }, timer);
  require(runs == 3, "zero warmup mishandled");
  for (double bad : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                     std::numeric_limits<double>::quiet_NaN()}) {
    FixedTimer invalid{bad};
    throws<std::runtime_error>([&] { (void)measure(options, [] {}, invalid); });
  }
  options.iterations = 0;
  throws<std::invalid_argument>([&] { (void)measure(options, [] {}, timer); });
}

template <class Resource, class... Args> void moves_test(Args... args) {
  FakeRuntime::reset();
  {
    Resource first(args...);
    auto handle = first.get();
    Resource second(std::move(first));
    require(first.get() == nullptr && second.get() == handle, "move construction failed");
    Resource third(args...);
    third = std::move(second);
    require(second.get() == nullptr && third.get() == handle, "move assignment failed");
    // Exercise self-move through an alias without compiler self-move warnings.
    auto *alias = &third;
    third = std::move(*alias);
    require(third.get() == handle, "self move lost resource");
  }
  FakeRuntime::reset();
}

void fake_benchmark() {
  Stream<FakeRuntime> stream;
  DeviceBuffer<float, FakeRuntime> a(257), b(257), out(257);
  a.get()[256] = 1;
  b.get()[256] = 2;
  EventTimer<FakeRuntime> timer(stream.get());
  Options options;
  options.warmup = 1;
  options.iterations = 2;
  const auto samples = measure(options, [&] {
    FakeRuntime::step("launch");
    out.get()[256] = a.get()[256] + b.get()[256];
  }, timer);
  require(samples == std::vector<double>({2, 2}), "incorrect event duration");
  require(out.get()[256] == 3, "fake kernel failed");
}

void cuda_lifetime_test() {
  static_assert(!std::is_copy_constructible_v<Stream<FakeRuntime>>);
  static_assert(std::is_nothrow_move_constructible_v<Stream<FakeRuntime>>);
  static_assert(!std::is_copy_constructible_v<DeviceBuffer<float, FakeRuntime>>);
  moves_test<Stream<FakeRuntime>>();
  moves_test<UniqueResource<EventPolicy<FakeRuntime>>>();
  moves_test<DeviceBuffer<double, FakeRuntime>>(17);
  FakeRuntime::reset();
  fake_benchmark();
  const std::vector<std::string> expected{
      "create stream", "allocate", "allocate", "allocate", "create event", "create event",
      "launch", "sync stream", "record 1", "launch", "record 2", "sync 2", "elapsed",
      "record 1", "launch", "record 2", "sync 2", "elapsed"};
  require(FakeRuntime::trace == expected, "CUDA timing order or warmup isolation incorrect");
  // Includes partial buffer/event construction, launch, record, asynchronous
  // completion and elapsed-time errors. All previously owned resources unwind.
  for (std::size_t failure = 1; failure <= expected.size(); ++failure) {
    FakeRuntime::reset(static_cast<int>(failure));
    throws<std::runtime_error>(fake_benchmark);
    require(FakeRuntime::calls == static_cast<int>(failure), "failure not reached");
  }
  FakeRuntime::reset();
  throws<std::length_error>([] {
    DeviceBuffer<double, FakeRuntime> buffer(std::numeric_limits<std::size_t>::max());
  });
  require(FakeRuntime::calls == 0, "overflow reached allocator");
  FakeRuntime::reset();
}
} // namespace

int main() {
  const std::vector<std::pair<const char *, std::function<void()>>> tests{
      {"options", options_test}, {"float vectors", vectors_test<float>},
      {"double vectors", vectors_test<double>}, {"statistics and JSON", statistics_test},
      {"measurement", measurement_test}, {"CUDA ownership and failure injection", cuda_lifetime_test}};
  int failed = 0;
  for (const auto &[name, test] : tests) {
    try { test(); std::cout << "PASS " << name << '\n'; }
    catch (const std::exception &error) {
      ++failed;
      std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    }
  }
  return failed == 0 ? 0 : 1;
}
