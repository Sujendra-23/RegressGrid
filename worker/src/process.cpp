#include "regressgrid/process.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <memory>
#include <openssl/evp.h>
#include <poll.h>
#include <spawn.h>
#include <sstream>
#include <stdexcept>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
namespace rg {
namespace {
class FD {
  int fd_ = -1;

public:
  explicit FD(int fd = -1) : fd_(fd) {}
  ~FD() { close(); }
  FD(const FD &) = delete;
  FD &operator=(const FD &) = delete;
  int get() const { return fd_; }
  void close() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }
};
class Child {
  pid_t pid_;

public:
  bool reaped = false;
  explicit Child(pid_t pid) : pid_(pid) {}
  ~Child() {
    if (!reaped) {
      ::kill(-pid_, SIGKILL);
      while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {
      }
    }
  }
};
std::uint64_t group_memory(pid_t group) {
#ifdef __linux__
  std::uint64_t memory = 0;
  std::error_code ec;
  for (const auto &entry : std::filesystem::directory_iterator("/proc", ec)) {
    auto name = entry.path().filename().string();
    if (name.empty() ||
        !std::all_of(name.begin(), name.end(),
                     [](unsigned char c) { return c >= '0' && c <= '9'; }))
      continue;
    std::ifstream file(entry.path() / "stat");
    std::string line;
    std::getline(file, line);
    auto end = line.rfind(')');
    if (end == std::string::npos)
      continue;
    std::istringstream fields(line.substr(end + 2));
    std::vector<std::string> values;
    for (std::string value; fields >> value;)
      values.push_back(value);
    try {
      if (values.size() > 21 && std::stol(values[2]) == group) {
        auto rss = std::stoll(values[21]);
        if (rss > 0)
          memory += static_cast<std::uint64_t>(rss) *
                    static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
      }
    } catch (const std::exception &) {
      // The process can exit between enumeration and reading /proc/PID/stat.
      continue;
    }
  }
  return memory;
#else
  (void)group;
  return 0;
#endif
}
struct SpawnActions {
  posix_spawn_file_actions_t value{};
  SpawnActions() {
    if (posix_spawn_file_actions_init(&value) != 0)
      throw std::runtime_error("spawn actions failed");
  }
  ~SpawnActions() { posix_spawn_file_actions_destroy(&value); }
};
struct SpawnAttrs {
  posix_spawnattr_t value{};
  SpawnAttrs() {
    if (posix_spawnattr_init(&value) != 0)
      throw std::runtime_error("spawn attributes failed");
  }
  ~SpawnAttrs() { posix_spawnattr_destroy(&value); }
};
void check(int code) {
  if (code != 0)
    throw std::runtime_error(std::string("process setup: ") +
                             std::strerror(code));
}
} // namespace
ProcessResult run_process(const std::vector<std::string> &argv,
                          const std::filesystem::path &cwd,
                          std::chrono::milliseconds timeout,
                          const std::stop_token &stop, const LogSink &log) {
  if (argv.empty())
    throw std::invalid_argument("empty process argv");
  auto started = std::chrono::steady_clock::now();
  ProcessResult result;
  if (stop.stop_requested()) {
    result.cancelled = true;
    return result;
  }
  int out[2], err[2];
  if (::pipe(out) < 0)
    throw std::runtime_error("stdout pipe failed");
  FD out_read(out[0]), out_write(out[1]);
  if (::pipe(err) < 0)
    throw std::runtime_error("stderr pipe failed");
  FD err_read(err[0]), err_write(err[1]);
  for (int fd : {out[0], out[1], err[0], err[1]})
    if (::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
      throw std::runtime_error("cloexec failed");
  for (int fd : {out[0], err[0]})
    if (::fcntl(fd, F_SETFL, O_NONBLOCK) < 0)
      throw std::runtime_error("nonblocking pipe failed");
  SpawnActions actions;
  SpawnAttrs attrs;
  check(
      posix_spawn_file_actions_adddup2(&actions.value, out[1], STDOUT_FILENO));
  check(
      posix_spawn_file_actions_adddup2(&actions.value, err[1], STDERR_FILENO));
  check(posix_spawn_file_actions_addclose(&actions.value, out[0]));
  check(posix_spawn_file_actions_addclose(&actions.value, err[0]));
  check(posix_spawn_file_actions_addclose(&actions.value, out[1]));
  check(posix_spawn_file_actions_addclose(&actions.value, err[1]));
#if defined(__APPLE__) &&                                                      \
    __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ >= 260000
  check(posix_spawn_file_actions_addchdir(&actions.value, cwd.c_str()));
#else
  check(posix_spawn_file_actions_addchdir_np(&actions.value, cwd.c_str()));
#endif
  check(posix_spawnattr_setflags(&attrs.value, POSIX_SPAWN_SETPGROUP));
  check(posix_spawnattr_setpgroup(&attrs.value, 0));
  std::vector<char *> args;
  args.reserve(argv.size() + 1);
  for (const auto &arg : argv)
    args.push_back(const_cast<char *>(arg.c_str()));
  args.push_back(nullptr);
  pid_t pid = 0;
  check(posix_spawnp(&pid, args.front(), &actions.value, &attrs.value,
                     args.data(), environ));
  Child child(pid);
  out_write.close();
  err_write.close();
  bool killed = false;
  int status = 0;
  rusage usage{};
  std::array<char, 8192> buffer{};
  auto sampled_at = started - std::chrono::milliseconds(100);
  while (!child.reaped || out_read.get() >= 0 || err_read.get() >= 0) {
    auto elapsed = std::chrono::steady_clock::now() - started;
    if (!killed && (stop.stop_requested() || elapsed >= timeout)) {
      result.cancelled = stop.stop_requested();
      result.timed_out = !result.cancelled;
      ::kill(-pid, SIGKILL);
      killed = true;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - sampled_at >= std::chrono::milliseconds(100)) {
      result.peak_memory_bytes =
          std::max(result.peak_memory_bytes, group_memory(pid));
      sampled_at = now;
    }
    pollfd fds[2] = {{out_read.get(), POLLIN, 0}, {err_read.get(), POLLIN, 0}};
    int polled = ::poll(fds, 2, 50);
    if (polled < 0 && errno != EINTR)
      throw std::runtime_error("poll failed");
    for (int i = 0; i < 2; ++i) {
      auto &fd = i == 0 ? out_read : err_read;
      if (fd.get() < 0)
        continue;
      if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
        // Drain bounded batches: fast output must not trigger a /proc scan per
        // 8 KiB, while cancellation still gets a turn between batches.
        for (unsigned batch = 0; batch < 32 && fd.get() >= 0; ++batch) {
          auto n = ::read(fd.get(), buffer.data(), buffer.size());
          if (n > 0) {
            std::string text(buffer.data(), static_cast<std::size_t>(n));
            log(i == 0 ? "stdout" : "stderr", text);
            result.tail += text;
            if (result.tail.size() > 262144)
              result.tail.erase(0, result.tail.size() - 262144);
          } else if (n == 0) {
            fd.close();
          } else if (errno != EAGAIN && errno != EINTR) {
            throw std::runtime_error("pipe read failed");
          } else {
            break;
          }
        }
      }
    }
    if (!child.reaped) {
      auto done = ::wait4(pid, &status, WNOHANG, &usage);
      if (done == pid)
        child.reaped = true;
      else if (done < 0 && errno != EINTR)
        throw std::runtime_error("wait4 failed");
    }
  }
  // Remove orphan descendants that outlive the direct process and close its
  // pipes.
  ::kill(-pid, SIGKILL);
  result.exit_code = WIFEXITED(status)
                         ? WEXITSTATUS(status)
                         : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
  result.seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
          .count();
  result.cpu_seconds =
      static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
      static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) /
          1e6;
#ifdef __APPLE__
  auto peak = static_cast<std::uint64_t>(usage.ru_maxrss);
#else
  auto peak = static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
#endif
  result.peak_memory_bytes = std::max(result.peak_memory_bytes, peak);
  return result;
}
std::string sha256_file(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    throw std::runtime_error("cannot read artifact");
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(),
                                                              EVP_MD_CTX_free);
  if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1)
    throw std::runtime_error("SHA-256 init failed");
  std::array<char, 65536> buffer{};
  while (file) {
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    if (EVP_DigestUpdate(ctx.get(), buffer.data(),
                         static_cast<std::size_t>(file.gcount())) != 1)
      throw std::runtime_error("SHA-256 update failed");
  }
  if (!file.eof())
    throw std::runtime_error("artifact read failed");
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned size = 0;
  if (EVP_DigestFinal_ex(ctx.get(), digest.data(), &size) != 1)
    throw std::runtime_error("SHA-256 final failed");
  std::ostringstream text;
  for (unsigned i = 0; i < size; ++i)
    text << std::hex << std::setw(2) << std::setfill('0')
         << static_cast<unsigned>(digest[i]);
  return text.str();
}
bool executable_available(const std::string &name) {
  const char *raw = std::getenv("PATH");
  if (raw == nullptr)
    return false;
  std::istringstream paths(raw);
  for (std::string path; std::getline(paths, path, ':');)
    if (::access((std::filesystem::path(path) / name).c_str(), X_OK) == 0)
      return true;
  return false;
}
} // namespace rg
