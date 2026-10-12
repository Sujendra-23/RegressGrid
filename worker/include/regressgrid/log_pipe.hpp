#pragma once
#include "regressgrid/spsc_ring.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace rg {
// Streams build/test output from the thread that runs the job to a dedicated
// sender thread through a lock-free SPSC ring, so reading a child's pipes is
// decoupled from the (possibly slow) gRPC write. The ring is the only shared
// state on the hot path. Order is preserved; a full ring applies backpressure
// to the producer (it spins, then yields, then sleeps) rather than dropping.
//
// If the sink throws (e.g. the coordinator disconnected), the sender thread
// records the exception, discards the remaining chunks, and the next push() or
// flush() rethrows it on the producing thread, as a synchronous sink would.
//
// Threading contract: push() and flush() are called only by the producing
// thread. The sink runs only on the sender thread, except that the producer may
// emit directly *after* flush() returns, because flush() waits until the sender
// is idle (the same pattern the worker uses for artifact and result events).
// The rings are cache-line aligned on purpose, so the default padding check does
// not apply.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
class LogPipe {
public:
  static constexpr std::size_t kChunkBytes = 8192;
  static constexpr std::size_t kSlots = 128;
  static constexpr std::size_t kStreamBytes = 16;
  using Sink = std::function<void(const std::string &stream,
                                  const std::string &text)>;

  explicit LogPipe(Sink sink);
  LogPipe(const LogPipe &) = delete;
  LogPipe &operator=(const LogPipe &) = delete;
  ~LogPipe(); // drains everything already pushed, then joins

  void push(std::string_view stream, std::string_view text);
  // Returns once every chunk pushed so far has been handed to the sink and the
  // sink call has returned.
  void flush();

  std::uint64_t pushed() const { return produced_; }
  std::uint64_t full_waits() const { return full_waits_; }

private:
  struct Chunk {
    std::array<char, kStreamBytes> stream{};
    std::uint16_t stream_len = 0;
    std::uint32_t len = 0;
    std::array<char, kChunkBytes> data;
  };
  void run(const std::stop_token &stop);

  Sink sink_;
  SpscRing<Chunk *, kSlots> ring_;
  // Chunks are recycled through a second ring (consumer -> producer), so the
  // steady state allocates nothing.
  SpscRing<Chunk *, kSlots> free_;
  std::vector<Chunk> storage_;
  std::uint64_t produced_ = 0;               // producer thread only
  std::uint64_t full_waits_ = 0;             // producer thread only
  std::atomic<std::uint64_t> consumed_{0};   // written by the sender thread
  std::atomic<bool> failed_{false};          // sink threw; error_ is valid
  std::exception_ptr error_;
  std::jthread sender_;
};
} // namespace rg
