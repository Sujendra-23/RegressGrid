#include "regressgrid/executor.hpp"
#include "regressgrid/process.hpp"
#include "regressgrid/thread_pool.hpp"
#include <atomic>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <thread>
#include <unistd.h>
using namespace std::chrono_literals;
TEST(CTestReport, CountsJUnitPassedFailedAndSkipped) {
  auto path = std::filesystem::temp_directory_path() /
              ("rg-report-" + std::to_string(getpid()));
  {
    std::ofstream f(path);
    f << R"(<testsuite><testcase name="ok" status="run"><system-out>passed</system-out></testcase><testcase name="broken"><failure>bad vector</failure></testcase><testcase name="disabled"><skipped/></testcase></testsuite>)";
  }
  const auto [passed, failed] = rg::read_test_counts(path);
  EXPECT_EQ(passed, 1u);
  EXPECT_EQ(failed, 2u);
  std::filesystem::remove(path);
}
TEST(CTestReport, LongFailureOutputDoesNotUseRecursiveRegex) {
  auto path = std::filesystem::temp_directory_path() /
              ("rg-long-report-" + std::to_string(getpid()));
  {
    std::ofstream f(path);
    f << "<testsuite><testcase name=\"failed\"><failure>"
      << std::string(500000, 'x') << "</failure></testcase></testsuite>";
  }
  EXPECT_EQ(rg::read_test_counts(path).second, 1u);
  std::filesystem::remove(path);
}
TEST(Process, StreamsBothPipesAndPreservesExitCode) {
  std::string out, err;
  auto r = rg::run_process(
      {"/bin/sh", "-c", "printf hello; printf problem >&2; exit 7"}, "/tmp", 2s,
      {}, [&](auto stream, auto text) {
        (stream == "stdout" ? out : err) += text;
      });
  EXPECT_EQ(r.exit_code, 7);
  EXPECT_EQ(out, "hello");
  EXPECT_EQ(err, "problem");
}
TEST(Process, ArgumentsAreNotShellExpanded) {
  std::string out;
  auto r =
      rg::run_process({"/bin/echo", "$(touch /tmp/rg-should-not-exist)"},
                      "/tmp", 2s, {}, [&](auto, auto text) { out += text; });
  EXPECT_EQ(r.exit_code, 0);
  EXPECT_EQ(out, "$(touch /tmp/rg-should-not-exist)\n");
}
TEST(Process, TimeoutKillsDescendantsHoldingPipes) {
  auto start = std::chrono::steady_clock::now();
  auto r = rg::run_process({"/bin/sh", "-c", "sleep 30 & wait"}, "/tmp", 150ms,
                           {}, [](auto, auto) {});
  EXPECT_TRUE(r.timed_out);
  EXPECT_LT(std::chrono::steady_clock::now() - start, 2s);
}
TEST(Process, CancellationStopsExecution) {
  std::stop_source source;
  std::jthread cancel([&] {
    std::this_thread::sleep_for(100ms);
    source.request_stop();
  });
  auto r = rg::run_process({"/bin/sleep", "30"}, "/tmp", 5s, source.get_token(),
                           [](auto, auto) {});
  EXPECT_TRUE(r.cancelled);
  EXPECT_FALSE(r.timed_out);
}
TEST(Process, LargeOutputDoesNotDeadlock) {
  std::size_t n = 0;
  auto r = rg::run_process(
      {"/bin/sh", "-c", "dd if=/dev/zero bs=65536 count=32 2>/dev/null"},
      "/tmp", 5s, {}, [&](auto, auto text) { n += text.size(); });
  EXPECT_EQ(r.exit_code, 0);
  EXPECT_EQ(n, 2097152u);
  EXPECT_LE(r.tail.size(), 262144u);
}
TEST(Process, MissingCommandThrows) {
  EXPECT_THROW(rg::run_process({"regressgrid-command-does-not-exist"}, "/tmp",
                               1s, {}, [](auto, auto) {}),
               std::runtime_error);
}
TEST(Process, CallbackExceptionReapsChild) {
  EXPECT_THROW(rg::run_process({"/bin/sh", "-c", "echo ready; sleep 30"},
                               "/tmp", 2s, {},
                               [](auto, auto) {
                                 throw std::runtime_error("sink disconnected");
                               }),
               std::runtime_error);
}
TEST(Checksum, KnownSHA256) {
  auto path = std::filesystem::temp_directory_path() /
              ("rg-hash-" + std::to_string(getpid()));
  {
    std::ofstream f(path);
    f << "abc";
  }
  EXPECT_EQ(rg::sha256_file(path),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  std::filesystem::remove(path);
}
TEST(ThreadPool, ExecutesConcurrentlyAndDrains) {
  rg::ThreadPool pool(2);
  std::promise<void> first, second;
  std::shared_future<void> barrier = first.get_future().share();
  pool.submit([&] {
    second.set_value();
    barrier.wait();
  });
  pool.submit([&] { first.set_value(); });
  EXPECT_EQ(second.get_future().wait_for(2s), std::future_status::ready);
  pool.shutdown();
}
