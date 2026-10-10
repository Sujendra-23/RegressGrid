#include "regressgrid/executor.hpp"
#include "regressgrid/process.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>
#include <regex>
#include <stdexcept>
#include <string>

namespace rg {
namespace pb = regressgrid::v1;
namespace fs = std::filesystem;
namespace {
struct Workspace {
  fs::path path;
  ~Workspace() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};
bool safe_id(const std::string &s) {
  return std::regex_match(s, std::regex("[a-zA-Z0-9_]+"));
}
std::string failure(const ProcessResult &r, const std::string &step) {
  if (r.cancelled) {
    return "job cancelled";
  }
  if (r.timed_out) {
    return "job exceeded execution timeout";
  }
  auto text = r.tail;
  std::string target_failure;
  for (std::string line; !text.empty();) {
    auto pos = text.find('\n');
    line = text.substr(0, pos);
    if (line.find("error:") != std::string::npos ||
        line.find("vector failed:") != std::string::npos)
      return line;
    if (line.find("FAILED:") != std::string::npos && target_failure.empty())
      target_failure = line;
    if (pos == std::string::npos)
      break;
    text.erase(0, pos + 1);
  }
  if (!target_failure.empty())
    return target_failure;
  if (r.tail.size() > 4000) {
    return step + " failed: " + r.tail.substr(r.tail.size() - 4000);
  }
  return step + " failed: " + r.tail;
}
} // namespace
std::pair<unsigned, unsigned> read_test_counts(const fs::path &path) {
  std::ifstream f(path);
  if (!f)
    return {0, 0};
  f.seekg(0, std::ios::end);
  const auto size = f.tellg();
  if (size < 0 || size > (16 << 20))
    throw std::runtime_error("CTest report exceeds 16 MiB");
  f.seekg(0);
  std::string xml(static_cast<std::size_t>(size), '\0');
  if (!f.read(xml.data(), static_cast<std::streamsize>(xml.size())))
    throw std::runtime_error("cannot read CTest report");
  unsigned passed = 0, failed = 0;
  std::size_t cursor = 0;
  while ((cursor = xml.find("<testcase ", cursor)) != std::string::npos) {
    auto header_end = xml.find('>', cursor);
    if (header_end == std::string::npos)
      throw std::runtime_error("invalid CTest report");
    auto end = xml[header_end - 1] == '/' ? header_end
                                          : xml.find("</testcase>", header_end);
    if (end == std::string::npos)
      throw std::runtime_error("invalid CTest report");
    auto body = xml.substr(cursor, end - cursor);
    if (body.find("<failure") != std::string::npos ||
        body.find("<skipped") != std::string::npos ||
        body.find("status=\"notrun\"") != std::string::npos)
      ++failed;
    else
      ++passed;
    cursor = end + 1;
  }
  return {passed, failed};
}
void execute(const pb::Assignment &job, const fs::path &root,
             std::stop_token stop, const EventSink &emit, unsigned parallel) {
  pb::WorkerEvent event;
  event.set_job_id(job.job_id());
  event.set_lease(job.lease());
  pb::Result final;
  final.set_status("failed");
  auto *metrics = final.mutable_metrics();
  try {
    if (!safe_id(job.job_id()) || !safe_id(job.lease()))
      throw std::runtime_error("invalid job identifier");
    Workspace workspace{root / (job.job_id() + "_" + job.lease())};
    fs::create_directories(workspace.path);
    const auto source = workspace.path / "source",
               build = workspace.path / "build",
               install = workspace.path / "install";
    auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds(job.timeout_seconds());
    std::string line_buffer;
    auto log = [&](const std::string &stream, const std::string &text) {
      event.set_log("[" + stream + "] " + text);
      emit(event);
      event.clear_log();
      line_buffer += text;
      std::size_t pos = 0;
      while ((pos = line_buffer.find('\n')) != std::string::npos) {
        auto line = line_buffer.substr(0, pos);
        if (line.find("warning:") != std::string::npos)
          metrics->set_warnings(metrics->warnings() + 1);
        line_buffer.erase(0, pos + 1);
      }
      if (line_buffer.size() > 65536)
        line_buffer.erase(0, line_buffer.size() - 1024);
    };
    auto run = [&](const std::vector<std::string> &args, const fs::path &cwd,
                   const std::string &step) {
      log("stage", step + "\n");
      auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
      auto result = run_process(
          args, cwd, std::max(remaining, std::chrono::milliseconds(0)), stop,
          log);
      metrics->set_peak_memory_bytes(
          std::max(metrics->peak_memory_bytes(), result.peak_memory_bytes));
      metrics->set_cpu_seconds(metrics->cpu_seconds() + result.cpu_seconds);
      if (step == "configure" || step == "build")
        metrics->set_build_seconds(metrics->build_seconds() + result.seconds);
      if (result.exit_code != 0 || result.cancelled || result.timed_out) {
        final.set_error(failure(result, step));
        if (result.cancelled)
          final.set_status("cancelled");
        if (step == "clone" || step == "fetch")
          final.set_infrastructure_error(!result.cancelled &&
                                         !result.timed_out);
        throw final;
      }
      return result;
    };
    run({"git", "clone", "--no-checkout", "--", job.repository(),
         source.string()},
        workspace.path, "clone");
    run({"git", "-C", source.string(), "fetch", "--depth=1", "origin",
         job.commit()},
        workspace.path, "fetch");
    run({"git", "-C", source.string(), "checkout", "--detach", "FETCH_HEAD"},
        workspace.path, "checkout");
    const bool clang = job.compiler() == "clang";
    const auto cc = clang ? "clang" : "gcc", cxx = clang ? "clang++" : "g++";
    std::vector<std::string> configure = {
        "cmake",
        "-S",
        source.string(),
        "-B",
        build.string(),
        "-G",
        "Ninja",
        "-DCMAKE_BUILD_TYPE=" + job.build_type(),
        "-DCMAKE_C_COMPILER=" + std::string(cc),
        "-DCMAKE_CXX_COMPILER=" + std::string(cxx),
        "-DCMAKE_INSTALL_PREFIX=" + install.string(),
        "-DBUILD_TESTING=ON"};
    if (job.plugin() == "verilator")
      configure.emplace_back("-DRG_VERILATOR=ON");
    run(configure, workspace.path, "configure");
    run({"cmake", "--build", build.string(), "--parallel",
         std::to_string(parallel)},
        workspace.path, "build");
    auto upload = [&](const fs::path &path, const std::string &name) {
      auto size = fs::file_size(path);
      if (size > (256ULL << 20))
        throw std::runtime_error("artifact exceeds 256 MiB");
      auto sum = sha256_file(path);
      std::ifstream f(path, std::ios::binary);
      std::array<char, 65536> buf{};
      while (f) {
        if (stop.stop_requested()) {
          final.set_status("cancelled");
          final.set_error("job cancelled during upload");
          throw final;
        }
        if (std::chrono::steady_clock::now() > deadline)
          throw std::runtime_error(
              "job exceeded execution timeout during upload");
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        if (f.gcount() > 0) {
          auto *a = event.mutable_artifact();
          a->set_name(name);
          a->set_data(buf.data(), static_cast<std::size_t>(f.gcount()));
          emit(event);
          event.clear_artifact();
        }
      }
      if (!f.eof())
        throw std::runtime_error("artifact read failed");
      auto *a = event.mutable_artifact();
      a->set_name(name);
      a->set_complete(true);
      a->set_sha256(sum);
      emit(event);
      event.clear_artifact();
    };
    auto upload_reports = [&] {
      if (fs::exists(build / "test-results.xml"))
        upload(build / "test-results.xml", "test-results.xml");
      if (job.plugin() == "verilator") {
        for (const auto &e : fs::directory_iterator(build))
          if (e.path().extension() == ".vcd")
            upload(e.path(), e.path().filename().string());
      }
    };
    auto test_started = std::chrono::steady_clock::now();
    try {
      run({"ctest", "--test-dir", build.string(), "--output-on-failure",
           "--no-tests=error", "--output-junit",
           (build / "test-results.xml").string()},
          workspace.path, "tests");
    } catch (const pb::Result &) {
      auto [passed, failed] = read_test_counts(build / "test-results.xml");
      metrics->set_tests_passed(passed);
      metrics->set_tests_failed(failed);
      metrics->set_test_seconds(
          std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                        test_started)
              .count());
      if (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline)
        upload_reports();
      throw;
    }
    auto [passed, failed] = read_test_counts(build / "test-results.xml");
    metrics->set_tests_passed(passed);
    metrics->set_tests_failed(failed);
    metrics->set_test_seconds(
        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                      test_started)
            .count());
    if (passed == 0)
      throw std::runtime_error("project must register at least one CTest test");
    auto custom = build / "regressgrid-metrics.json";
    if (fs::exists(custom)) {
      std::ifstream f(custom);
      auto json = nlohmann::json::parse(f);
      metrics->set_latency_ms(json.value("latency_ms", 0.0));
      metrics->set_throughput(json.value("throughput", 0.0));
      if (!std::isfinite(metrics->latency_ms()) ||
          !std::isfinite(metrics->throughput()) || metrics->latency_ms() < 0 ||
          metrics->throughput() < 0)
        throw std::runtime_error(
            "benchmark metrics must be finite and nonnegative");
    }
    upload_reports();
    run({"cmake", "--install", build.string()}, workspace.path, "install");
    if (!fs::exists(install))
      throw std::runtime_error("project must provide CMake install rules");
    for (auto &entry : fs::recursive_directory_iterator(install)) {
      if (entry.is_symlink())
        throw std::runtime_error("artifact symlinks are unsupported");
      if (entry.is_regular_file())
        metrics->set_binary_bytes(metrics->binary_bytes() + entry.file_size());
    }
    auto archive = workspace.path / "artifacts.tar.gz";
    run({"tar", "-czf", archive.string(), "-C", install.string(), "."},
        workspace.path, "package");
    upload(archive, "artifacts.tar.gz");
    if (fs::exists(custom))
      upload(custom, "regressgrid-metrics.json");
    final.set_status("passed");
  } catch (const pb::Result &result) {
    final.set_status(result.status());
    final.set_error(result.error());
    final.set_infrastructure_error(result.infrastructure_error());
  } catch (const std::exception &ex) {
    final.set_error(ex.what());
  }
  event.clear_log();
  event.clear_artifact();
  *event.mutable_result() = final;
  emit(event);
}
} // namespace rg
