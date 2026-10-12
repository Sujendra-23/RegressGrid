// Drives rg::execute() end to end against a throwaway local git repository to
// check the lock-free log path in its real setting: log events arrive in order,
// every log precedes the artifact and result events, and Write()-style emits
// never overlap (the real coordinator stream is not safe for concurrent writes
// beyond main.cpp's mutex; here we assert the executor itself serialises them).
#include "regressgrid/executor.hpp"
#include "regressgrid/process.hpp"
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
namespace pb = regressgrid::v1;

namespace {
std::string sh(const std::string &cmd) {
  EXPECT_EQ(std::system((cmd + " >/dev/null 2>&1").c_str()), 0) << cmd;
  return cmd;
}
} // namespace

TEST(Executor, StreamsLogsInOrderBeforeArtifactsAndResult) {
  for (const char *tool : {"git", "cmake", "ninja", "ctest", "tar", "gcc", "g++"})
    if (!rg::executable_available(tool))
      GTEST_SKIP() << tool << " is not installed";
  const auto base =
      fs::temp_directory_path() / ("rg-exec-" + std::to_string(getpid()));
  fs::remove_all(base);
  const auto repo = base / "repo", work = base / "work";
  fs::create_directories(repo);
  fs::create_directories(work);
  {
    std::ofstream f(repo / "CMakeLists.txt");
    f << "cmake_minimum_required(VERSION 3.20)\n"
         "project(tiny NONE)\n"
         "enable_testing()\n"
         "add_test(NAME ok COMMAND ${CMAKE_COMMAND} -E echo hello)\n"
         "add_custom_target(noise ALL COMMAND ${CMAKE_COMMAND} -E echo "
         "\"file.c:1: warning: synthetic\")\n"
         "install(FILES CMakeLists.txt DESTINATION share/tiny)\n";
  }
  sh("git -C " + repo.string() + " init -q");
  sh("git -C " + repo.string() + " config user.email t@example.com");
  sh("git -C " + repo.string() + " config user.name test");
  sh("git -C " + repo.string() + " config uploadpack.allowAnySHA1InWant true");
  sh("git -C " + repo.string() + " add -A");
  sh("git -C " + repo.string() + " commit -q -m init");
  std::string commit;
  {
    sh("git -C " + repo.string() + " rev-parse HEAD > " + (base / "sha").string());
    std::ifstream f(base / "sha");
    std::getline(f, commit);
  }
  pb::Assignment job;
  job.set_job_id("job1");
  job.set_lease("lease1");
  job.set_repository(repo.string());
  job.set_commit(commit);
  job.set_compiler("gcc");
  job.set_build_type("Debug");
  job.set_timeout_seconds(120);

  std::mutex m;
  std::vector<pb::WorkerEvent> events;
  std::atomic<int> inside{0};
  std::atomic<bool> overlapped{false};
  auto emit = [&](const pb::WorkerEvent &e) {
    if (inside.fetch_add(1) != 0)
      overlapped = true;
    {
      std::lock_guard lock(m);
      events.push_back(e);
    }
    inside.fetch_sub(1);
  };
  rg::execute(job, work, std::stop_token{}, emit, 2);

  EXPECT_FALSE(overlapped) << "two emit() calls ran concurrently";
  ASSERT_FALSE(events.empty());
  const auto &last = events.back();
  ASSERT_TRUE(last.has_result());
  EXPECT_EQ(last.result().status(), "passed") << last.result().error();
  EXPECT_EQ(last.result().metrics().tests_passed(), 1u);
  EXPECT_GE(last.result().metrics().warnings(), 1u); // ninja echoes the command too
  std::string log;
  bool seen_artifact = false;
  for (std::size_t i = 0; i + 1 < events.size(); ++i) {
    const auto &e = events[i];
    EXPECT_EQ(e.job_id(), "job1");
    EXPECT_EQ(e.lease(), "lease1");
    if (e.has_artifact())
      seen_artifact = true;
    if (!e.log().empty())
      log += e.log();
  }
  EXPECT_TRUE(seen_artifact);
  std::size_t at = 0;
  for (const char *stage : {"[stage] clone", "[stage] fetch", "[stage] checkout",
                            "[stage] configure", "[stage] build", "[stage] tests",
                            "[stage] install", "[stage] package"}) {
    const auto pos = log.find(stage, at);
    ASSERT_NE(pos, std::string::npos) << stage << " missing or out of order";
    at = pos;
  }
  EXPECT_NE(log.find("warning: synthetic"), std::string::npos);
  fs::remove_all(base);
}
