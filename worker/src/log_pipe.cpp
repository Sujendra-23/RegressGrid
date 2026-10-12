#include "regressgrid/log_pipe.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace rg {
namespace {
// Spin briefly (the common case: the other side is a few hundred ns away),
// then yield, then sleep, so an idle pipe costs almost no CPU.
class Backoff {
  unsigned n_ = 0;

public:
  void wait() {
    if (n_ >= 256)
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    else if (n_ >= 64)
      std::this_thread::yield();
    ++n_; // fewer than 64 attempts: just retry
  }
  void reset() { n_ = 0; }
};
} // namespace

LogPipe::LogPipe(Sink sink) : sink_(std::move(sink)), storage_(kSlots) {
  for (auto &chunk : storage_) {
    Chunk *p = &chunk;
    free_.try_push(p);
  }
  // free_ is filled before the sender thread exists, which publishes it.
  sender_ = std::jthread([this](const std::stop_token &stop) { run(stop); });
}

LogPipe::~LogPipe() {
  sender_.request_stop();
  if (sender_.joinable())
    sender_.join();
}

void LogPipe::push(std::string_view stream, std::string_view text) {
  if (failed_.load(std::memory_order_acquire))
    std::rethrow_exception(error_);
  if (text.empty())
    return;
  stream = stream.substr(0, kStreamBytes);
  Backoff backoff;
  do {
    // Take a recycled chunk; if the sender is behind, wait (backpressure).
    Chunk *chunk = nullptr;
    bool waited = false;
    while (!free_.try_pop(chunk)) {
      waited = true;
      backoff.wait();
      if (failed_.load(std::memory_order_acquire))
        std::rethrow_exception(error_);
    }
    if (waited)
      ++full_waits_;
    backoff.reset();
    const auto n = std::min(text.size(), kChunkBytes);
    std::memcpy(chunk->data.data(), text.data(), n);
    chunk->len = static_cast<std::uint32_t>(n);
    std::memcpy(chunk->stream.data(), stream.data(), stream.size());
    chunk->stream_len = static_cast<std::uint16_t>(stream.size());
    text.remove_prefix(n);
    // Cannot fail: ring_ and free_ together hold exactly kSlots chunks.
    ring_.try_push(chunk);
    ++produced_;
  } while (!text.empty());
}

void LogPipe::flush() {
  Backoff backoff;
  while (consumed_.load(std::memory_order_acquire) != produced_)
    backoff.wait();
  if (failed_.load(std::memory_order_acquire))
    std::rethrow_exception(error_);
}

void LogPipe::run(const std::stop_token &stop) {
  Backoff backoff;
  std::string stream, text;
  for (;;) {
    Chunk *chunk = nullptr;
    if (ring_.try_pop(chunk)) {
      backoff.reset();
      stream.assign(chunk->stream.data(), chunk->stream_len);
      text.assign(chunk->data.data(), chunk->len);
      free_.try_push(chunk); // data copied out: recycle now
      if (!failed_.load(std::memory_order_relaxed)) {
        try {
          sink_(stream, text);
        } catch (...) {
          error_ = std::current_exception();
          failed_.store(true, std::memory_order_release);
        }
      }
      consumed_.fetch_add(1, std::memory_order_release);
      continue;
    }
    // Only exit when asked to stop *and* nothing is left (drain on destroy).
    if (stop.stop_requested() && ring_.empty())
      return;
    backoff.wait();
  }
}
} // namespace rg
