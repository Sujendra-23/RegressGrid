#pragma once

#include "regressgrid/benchmark.hpp"
#include <utility>

namespace rg::bench {
// Policy creation throws on failure. A successfully created handle has exactly
// one owner; cleanup never throws during stack unwinding.
template <class Policy> class UniqueResource {
public:
  using Handle = typename Policy::Handle;
  template <class... Args>
  explicit UniqueResource(Args &&...args) : handle_(Policy::create(std::forward<Args>(args)...)) {}
  ~UniqueResource() { reset(); }
  UniqueResource(const UniqueResource &) = delete;
  UniqueResource &operator=(const UniqueResource &) = delete;
  UniqueResource(UniqueResource &&other) noexcept : handle_(std::exchange(other.handle_, Handle{})) {}
  UniqueResource &operator=(UniqueResource &&other) noexcept {
    if (this != &other) {
      reset();
      handle_ = std::exchange(other.handle_, Handle{});
    }
    return *this;
  }
  Handle get() const noexcept { return handle_; }
private:
  Handle handle_{};
  void reset() noexcept {
    if (handle_ != Handle{}) Policy::destroy(std::exchange(handle_, Handle{}));
  }
};

template <class Runtime> struct StreamPolicy {
  using Handle = typename Runtime::Stream;
  static Handle create() { return Runtime::create_stream(); }
  static void destroy(Handle stream) noexcept { Runtime::destroy_stream(stream); }
};
template <class Runtime> struct EventPolicy {
  using Handle = typename Runtime::Event;
  static Handle create() { return Runtime::create_event(); }
  static void destroy(Handle event) noexcept { Runtime::destroy_event(event); }
};
template <class T, class Runtime> struct BufferPolicy {
  using Handle = T *;
  static Handle create(std::size_t elements) {
    return static_cast<T *>(Runtime::allocate(byte_count<T>(elements)));
  }
  static void destroy(Handle pointer) noexcept { Runtime::free(pointer); }
};
template <class Runtime> using Stream = UniqueResource<StreamPolicy<Runtime>>;
template <class T, class Runtime> using DeviceBuffer = UniqueResource<BufferPolicy<T, Runtime>>;

template <class Runtime> class EventTimer {
  typename Runtime::Stream stream_; // Borrowed: owner must outlive this timer.
  UniqueResource<EventPolicy<Runtime>> start_, stop_;
public:
  explicit EventTimer(typename Runtime::Stream stream) : stream_(stream) {}
  EventTimer(const EventTimer &) = delete;
  EventTimer &operator=(const EventTimer &) = delete;
  void synchronize() { Runtime::synchronize_stream(stream_); }
  void start() { Runtime::record(start_.get(), stream_); }
  double stop() {
    Runtime::record(stop_.get(), stream_);
    Runtime::synchronize_event(stop_.get());
    return Runtime::elapsed_ms(start_.get(), stop_.get());
  }
};
} // namespace rg::bench
