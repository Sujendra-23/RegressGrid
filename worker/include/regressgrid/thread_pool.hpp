#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace rg {
class ThreadPool {
  std::mutex mutex_;
  std::condition_variable_any ready_;
  std::deque<std::function<void()>> tasks_;
  std::vector<std::jthread> threads_;
  bool closing_ = false;

public:
  explicit ThreadPool(unsigned count) {
    if (count == 0)
      throw std::invalid_argument("thread count must be positive");
    for (unsigned i = 0; i < count; ++i)
      threads_.emplace_back([this](const std::stop_token &stop) {
        while (true) {
          std::function<void()> task;
          {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, stop,
                        [this] { return !tasks_.empty() || closing_; });
            if ((closing_ || stop.stop_requested()) && tasks_.empty())
              return;
            if (tasks_.empty())
              continue;
            task = std::move(tasks_.front());
            tasks_.pop_front();
          }
          task();
        }
      });
  }
  ThreadPool(const ThreadPool &) = delete;
  ThreadPool &operator=(const ThreadPool &) = delete;
  void submit(std::function<void()> task) {
    {
      std::lock_guard lock(mutex_);
      if (closing_)
        throw std::runtime_error("pool is closing");
      tasks_.push_back(std::move(task));
    }
    ready_.notify_one();
  }
  void shutdown() {
    {
      std::lock_guard lock(mutex_);
      closing_ = true;
    }
    ready_.notify_all();
    for (auto &thread : threads_)
      if (thread.joinable())
        thread.join();
  }
  ~ThreadPool() { shutdown(); }
};
} // namespace rg
