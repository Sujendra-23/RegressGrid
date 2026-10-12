#pragma once
#include <atomic>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace rg {
// Bounded lock-free single-producer / single-consumer ring buffer.
//
// Exactly one thread may call try_push and exactly one (other) thread may call
// try_pop. Capacity is a power of two; indices grow monotonically and are
// masked on access, so a full ring holds Capacity items (no wasted slot).
// The producer publishes with a release store of head_, the consumer with a
// release store of tail_; each side reads the other's index with acquire and
// caches it to avoid touching the shared cache line on every operation.
template <typename T, std::size_t Capacity> class SpscRing {
  static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                "capacity must be a power of two");
  static_assert(std::is_nothrow_move_assignable_v<T>,
                "T must be nothrow move assignable");
// Fixed (not std::hardware_destructive_interference_size, whose value varies
  // with -mtune): 128 bytes covers 64-byte x86 lines and 128-byte Apple/ARM ones.
  static constexpr std::size_t kLine = 128;
  static constexpr std::size_t kMask = Capacity - 1;

  alignas(kLine) std::atomic<std::size_t> head_{0}; // next slot to write
  std::size_t cached_tail_ = 0;                     // producer's copy of tail_
  alignas(kLine) std::atomic<std::size_t> tail_{0}; // next slot to read
  std::size_t cached_head_ = 0;                     // consumer's copy of head_
  alignas(kLine) T slots_[Capacity];

public:
  SpscRing() = default;
  SpscRing(const SpscRing &) = delete;
  SpscRing &operator=(const SpscRing &) = delete;

  static constexpr std::size_t capacity() { return Capacity; }

  // Producer only. Returns false (item untouched) if the ring is full.
  bool try_push(const T &item) {
    return emplace_slot([&](T &slot) { slot = item; });
  }
  bool try_push(T &&item) {
    return emplace_slot([&](T &slot) { slot = std::move(item); });
  }
  // Consumer only. Returns false if the ring is empty.
  bool try_pop(T &out) {
    const auto tail = tail_.load(std::memory_order_relaxed);
    if (tail == cached_head_) {
      cached_head_ = head_.load(std::memory_order_acquire);
      if (tail == cached_head_)
        return false;
    }
    out = std::move(slots_[tail & kMask]);
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }
  // Approximate from any thread (exact when called by the producer or consumer
  // while the other side is quiescent).
  std::size_t size() const {
    return head_.load(std::memory_order_acquire) -
           tail_.load(std::memory_order_acquire);
  }
  bool empty() const { return size() == 0; }

private:
  template <typename Fill> bool emplace_slot(Fill &&fill) {
    const auto head = head_.load(std::memory_order_relaxed);
    if (head - cached_tail_ == Capacity) {
      cached_tail_ = tail_.load(std::memory_order_acquire);
      if (head - cached_tail_ == Capacity)
        return false;
    }
    fill(slots_[head & kMask]);
    head_.store(head + 1, std::memory_order_release);
    return true;
  }
};
} // namespace rg
