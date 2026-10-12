// Micro-benchmark for the lock-free SPSC ring versus a mutex + condition
// variable queue. Reports throughput (producer pushes N items, consumer pops
// them) and one-way latency (ping-pong over two queues; p50/p99/max).
// Numbers depend entirely on the machine; they are printed with the host info.
#include "regressgrid/spsc_ring.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

class MutexQueue {
  std::mutex m_;
  std::condition_variable cv_;
  std::deque<std::uint64_t> q_;

public:
  void push(std::uint64_t v) {
    {
      std::lock_guard l(m_);
      q_.push_back(v);
    }
    cv_.notify_one();
  }
  std::uint64_t pop() {
    std::unique_lock l(m_);
    cv_.wait(l, [&] { return !q_.empty(); });
    auto v = q_.front();
    q_.pop_front();
    return v;
  }
};

constexpr std::uint64_t kItems = 20'000'000;
constexpr std::uint64_t kMutexItems = 2'000'000;
constexpr int kPings = 200'000;

double seconds(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double>(b - a).count();
}
double ring_throughput() {
  static rg::SpscRing<std::uint64_t, 4096> ring;
  std::uint64_t sum = 0;
  auto t0 = Clock::now();
  std::thread consumer([&] {
    std::uint64_t v, n = 0;
    while (n < kItems)
      if (ring.try_pop(v)) { sum += v; ++n; }
  });
  for (std::uint64_t i = 0; i < kItems;) {
    std::uint64_t v = i;
    if (ring.try_push(std::move(v))) ++i;
  }
  consumer.join();
  auto t1 = Clock::now();
  if (sum != kItems * (kItems - 1) / 2) std::puts("CHECKSUM MISMATCH");
  return static_cast<double>(kItems) / seconds(t0, t1);
}
double mutex_throughput() {
  MutexQueue q;
  std::uint64_t sum = 0;
  auto t0 = Clock::now();
  std::thread consumer([&] {
    for (std::uint64_t n = 0; n < kMutexItems; ++n) sum += q.pop();
  });
  for (std::uint64_t i = 0; i < kMutexItems; ++i) q.push(i);
  consumer.join();
  auto t1 = Clock::now();
  if (sum != kMutexItems * (kMutexItems - 1) / 2) std::puts("CHECKSUM MISMATCH");
  return static_cast<double>(kMutexItems) / seconds(t0, t1);
}
void report(const char *name, std::vector<double> &ns) {
  std::sort(ns.begin(), ns.end());
  std::printf("  %-22s p50 %8.0f ns   p99 %8.0f ns   max %9.0f ns\n", name,
              ns[ns.size() / 2], ns[ns.size() * 99 / 100], ns.back());
}
void ring_latency() {
  static rg::SpscRing<std::uint64_t, 64> ping, pong;
  std::thread echo([&] {
    std::uint64_t v;
    for (int i = 0; i < kPings; ++i) {
      while (!ping.try_pop(v)) {}
      while (!pong.try_push(std::move(v))) {}
    }
  });
  std::vector<double> rtt;
  rtt.reserve(kPings);
  for (int i = 0; i < kPings; ++i) {
    auto t0 = Clock::now();
    std::uint64_t v = static_cast<std::uint64_t>(i);
    while (!ping.try_push(std::move(v))) {}
    while (!pong.try_pop(v)) {}
    rtt.push_back(std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / 2);
  }
  echo.join();
  report("SPSC ring (spin)", rtt);
}
void mutex_latency() {
  MutexQueue ping, pong;
  std::thread echo([&] {
    for (int i = 0; i < kPings / 10; ++i) pong.push(ping.pop());
  });
  std::vector<double> rtt;
  for (int i = 0; i < kPings / 10; ++i) {
    auto t0 = Clock::now();
    ping.push(static_cast<std::uint64_t>(i));
    pong.pop();
    rtt.push_back(std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / 2);
  }
  echo.join();
  report("mutex + condvar", rtt);
}
} // namespace

int main() {
  std::printf("hardware threads: %u\n", std::thread::hardware_concurrency());
#ifdef __VERSION__
  std::printf("compiler: %s\n", __VERSION__);
#endif
  std::puts("throughput (one producer, one consumer):");
  double r = ring_throughput();
  double m = mutex_throughput();
  std::printf("  SPSC ring        %8.2f Mitems/s (%llu items)\n", r / 1e6,
              static_cast<unsigned long long>(kItems));
  std::printf("  mutex + condvar  %8.2f Mitems/s (%llu items)\n", m / 1e6,
              static_cast<unsigned long long>(kMutexItems));
  std::printf("  ratio            %8.1fx\n", r / m);
  std::puts("one-way latency (half of a ping-pong round trip):");
  ring_latency();
  mutex_latency();
  return 0;
}
