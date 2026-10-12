#include "regressgrid/log_pipe.hpp"
#include "regressgrid/spsc_ring.hpp"
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
using namespace std::chrono_literals;

TEST(SpscRing, FillsToCapacityThenRejectsAndPreservesFifoOrder) {
  rg::SpscRing<int, 8> ring;
  EXPECT_TRUE(ring.empty());
  int out = -1;
  EXPECT_FALSE(ring.try_pop(out));
  for (int i = 0; i < 8; ++i) {
    int v = i;
    EXPECT_TRUE(ring.try_push(std::move(v)));
  }
  int extra = 99;
  EXPECT_FALSE(ring.try_push(std::move(extra))); // full: exactly Capacity items
  EXPECT_EQ(extra, 99);                          // rejected item is untouched
  EXPECT_EQ(ring.size(), 8u);
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(ring.try_pop(out));
    EXPECT_EQ(out, i);
  }
  EXPECT_FALSE(ring.try_pop(out));
}

TEST(SpscRing, WrapsAroundManyTimes) {
  rg::SpscRing<std::uint64_t, 4> ring;
  std::uint64_t next_in = 0, next_out = 0, out = 0;
  for (int round = 0; round < 10000; ++round) {
    for (int k = 0; k < 3; ++k) {
      std::uint64_t v = next_in++;
      ASSERT_TRUE(ring.try_push(std::move(v)));
    }
    for (int k = 0; k < 3; ++k) {
      ASSERT_TRUE(ring.try_pop(out));
      ASSERT_EQ(out, next_out++);
    }
  }
}

TEST(SpscRing, TwoThreadsTransferEveryItemInOrder) {
  constexpr std::uint64_t kItems = 2'000'000;
  rg::SpscRing<std::uint64_t, 1024> ring;
  std::atomic<bool> in_order{true};
  std::uint64_t received = 0;
  std::thread consumer([&] {
    std::uint64_t v = 0, expect = 0;
    while (expect < kItems) {
      if (ring.try_pop(v)) {
        if (v != expect++)
          in_order = false;
      } else {
        std::this_thread::yield();
      }
    }
    received = expect;
  });
  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kItems;) {
      std::uint64_t v = i;
      if (ring.try_push(std::move(v)))
        ++i;
      else
        std::this_thread::yield();
    }
  });
  producer.join();
  consumer.join();
  EXPECT_TRUE(in_order);
  EXPECT_EQ(received, kItems);
  EXPECT_TRUE(ring.empty());
}

TEST(SpscRing, MovesNonTrivialPayloads) {
  rg::SpscRing<std::string, 4> ring;
  std::string a(1000, 'x');
  EXPECT_TRUE(ring.try_push(std::move(a)));
  std::string out;
  EXPECT_TRUE(ring.try_pop(out));
  EXPECT_EQ(out, std::string(1000, 'x'));
}

TEST(LogPipe, DeliversEverythingInOrderAndSplitsLargeWrites) {
  std::mutex m;
  std::vector<std::pair<std::string, std::string>> seen;
  {
    rg::LogPipe pipe([&](const std::string &s, const std::string &t) {
      std::lock_guard lock(m);
      seen.emplace_back(s, t);
    });
    for (int i = 0; i < 2000; ++i)
      pipe.push(i % 2 ? "stderr" : "stdout", "line " + std::to_string(i) + "\n");
    pipe.push("stdout", std::string(3 * rg::LogPipe::kChunkBytes + 17, 'z'));
    pipe.flush();
    std::lock_guard lock(m);
    ASSERT_EQ(seen.size(), 2000u + 4u); // large write split into 4 chunks
    for (int i = 0; i < 2000; ++i) {
      EXPECT_EQ(seen[i].first, i % 2 ? "stderr" : "stdout");
      EXPECT_EQ(seen[i].second, "line " + std::to_string(i) + "\n");
    }
    std::string joined;
    for (std::size_t i = 2000; i < seen.size(); ++i)
      joined += seen[i].second;
    EXPECT_EQ(joined, std::string(3 * rg::LogPipe::kChunkBytes + 17, 'z'));
  }
}

TEST(LogPipe, SlowSinkAppliesBackpressureWithoutLoss) {
  std::atomic<int> count{0};
  rg::LogPipe pipe([&](const std::string &, const std::string &) {
    if (count.load() % 64 == 0)
      std::this_thread::sleep_for(1ms); // slower than the producer
    ++count;
  });
  const int n = 5 * static_cast<int>(rg::LogPipe::kSlots);
  for (int i = 0; i < n; ++i)
    pipe.push("stdout", "x");
  pipe.flush();
  EXPECT_EQ(count.load(), n);
  EXPECT_GT(pipe.full_waits(), 0u); // the ring really filled up
}

TEST(LogPipe, DestructorDrainsPendingChunks) {
  std::atomic<int> count{0};
  {
    rg::LogPipe pipe([&](const std::string &, const std::string &) { ++count; });
    for (int i = 0; i < 100; ++i)
      pipe.push("stdout", "y");
  }
  EXPECT_EQ(count.load(), 100);
}

TEST(LogPipe, SinkFailureSurfacesOnProducerThread) {
  rg::LogPipe pipe([](const std::string &, const std::string &) {
    throw std::runtime_error("coordinator disconnected");
  });
  pipe.push("stdout", "a");
  EXPECT_THROW(
      {
        for (int i = 0; i < 100000; ++i) {
          pipe.push("stdout", "b");
          std::this_thread::sleep_for(10us);
        }
      },
      std::runtime_error);
  EXPECT_THROW(pipe.flush(), std::runtime_error);
}
