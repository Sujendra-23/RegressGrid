#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace rg {
using LogSink = std::function<void(const std::string &, const std::string &)>;
struct ProcessResult {
  int exit_code = -1;
  bool cancelled = false;
  bool timed_out = false;
  double seconds = 0;
  double cpu_seconds = 0;
  std::uint64_t peak_memory_bytes = 0;
  std::string tail;
};
// argv is passed directly to posix_spawnp: no shell expansion.
ProcessResult run_process(const std::vector<std::string> &argv,
                          const std::filesystem::path &cwd,
                          std::chrono::milliseconds timeout,
                          const std::stop_token &stop, const LogSink &log);
std::string sha256_file(const std::filesystem::path &path);
bool executable_available(const std::string &name);
} // namespace rg
