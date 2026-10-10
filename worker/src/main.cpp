#include "grid.grpc.pb.h"
#include "regressgrid/executor.hpp"
#include "regressgrid/process.hpp"
#include "regressgrid/thread_pool.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <grpcpp/grpcpp.h>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace grid = regressgrid::v1;
namespace {
std::atomic<bool> stopping{false};
static_assert(std::atomic<bool>::is_always_lock_free);
void on_signal(int) { stopping.store(true, std::memory_order_relaxed); }
std::string env(const char *name, const std::string &fallback) {
  const char *v = std::getenv(name);
  return v == nullptr ? fallback : v;
}
struct Limits {
  unsigned fallback;
  unsigned maximum;
};
unsigned number(const char *name, Limits limits) {
  const auto s = env(name, std::to_string(limits.fallback));
  std::size_t consumed = 0;
  auto n = std::stoul(s, &consumed);
  if (consumed != s.size() || n == 0 || n > limits.maximum)
    throw std::runtime_error(std::string("invalid ") + name);
  return static_cast<unsigned>(n);
}
} // namespace
int main() {
  try {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);
    std::array<char, 256> host{};
    if (::gethostname(host.data(), host.size() - 1) != 0)
      throw std::runtime_error("gethostname failed");
    const auto wid = env("WORKER_ID", host.data()),
               target = env("CONTROL_ADDR", "127.0.0.1:9090"),
               token = env("WORKER_TOKEN", "");
    auto root = std::filesystem::absolute(env("WORK_DIR", ".local/work"));
    std::filesystem::create_directories(root);
    const unsigned slots = number("WORKER_SLOTS", {2, 64}),
                   parallel = number("BUILD_PARALLELISM", {2, 64});
    std::shared_ptr<grpc::ChannelCredentials> creds =
        grpc::InsecureChannelCredentials();
    if (env("TLS_ROOT", "") != "") {
      std::ifstream f(env("TLS_ROOT", ""));
      if (!f)
        throw std::runtime_error("cannot open TLS_ROOT");
      grpc::SslCredentialsOptions options;
      options.pem_root_certs =
          std::string((std::istreambuf_iterator<char>(f)), {});
      creds = grpc::SslCredentials(options);
    }
    grpc::ChannelArguments args;
    args.SetMaxReceiveMessageSize(128 << 10);
    auto stub = grid::Coordinator::NewStub(
        grpc::CreateCustomChannel(target, creds, args));
    while (!stopping) {
      grpc::ClientContext ctx;
      // Shutdown must cancel a blocked transport write as well as an idle read.
      std::jthread shutdown_watch([&](const std::stop_token &stop) {
        while (!stop.stop_requested()) {
          if (stopping.load(std::memory_order_relaxed)) {
            ctx.TryCancel();
            return;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
      });
      if (!token.empty())
        ctx.AddMetadata("authorization", "Bearer " + token);
      auto stream = stub->Work(&ctx);
      std::mutex write_mutex, active_mutex;
      struct Active {
        std::string lease;
        std::stop_source stop;
      };
      std::map<std::string, Active> active;
      auto emit = [&](const grid::WorkerEvent &e) {
        std::lock_guard lock(write_mutex);
        if (!stream->Write(e))
          throw std::runtime_error("coordinator disconnected");
      };
      grid::WorkerEvent hello;
      auto *h = hello.mutable_hello();
      h->set_id(wid);
      h->set_slots(slots);
#ifdef __linux__
      h->set_platform("linux");
#else
      h->set_platform("macos");
#endif
      if (rg::executable_available("g++") && rg::executable_available("gcc")) {
        auto version =
            rg::run_process({"g++", "--version"}, root, std::chrono::seconds(2),
                            {}, [](const auto &, const auto &) {});
        if (version.exit_code == 0 &&
            version.tail.find("clang") == std::string::npos)
          h->add_compilers("gcc");
      }
      if (rg::executable_available("clang++") &&
          rg::executable_available("clang"))
        h->add_compilers("clang");
      h->set_verilator(rg::executable_available("verilator"));
      try {
        emit(hello);
      } catch (const std::exception &ex) {
        std::cerr << ex.what() << '\n';
        ctx.TryCancel();
        stream->Finish();
        if (!stopping)
          std::this_thread::sleep_for(std::chrono::seconds(1));
        continue;
      }
      std::cout << "worker " << wid << " connected; slots=" << slots << '\n'
                << std::flush;
      rg::ThreadPool pool(slots);
      std::jthread heartbeat([&](const std::stop_token &stop) {
        unsigned ticks = 0;
        while (!stop.stop_requested()) {
          if (stopping) {
            ctx.TryCancel();
            return;
          }
          if (++ticks >= 30) {
            ticks = 0;
            grid::WorkerEvent e;
            e.set_heartbeat(true);
            try {
              emit(e);
            } catch (const std::exception &) {
              ctx.TryCancel();
              return;
            }
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
      });
      grid::CoordinatorEvent received;
      while (stream->Read(&received)) {
        if (received.has_assignment()) {
          const auto &job = received.assignment();
          std::stop_token stop;
          {
            std::lock_guard lock(active_mutex);
            if (active.contains(job.job_id()))
              continue;
            if (active.size() >= slots) {
              ctx.TryCancel();
              break;
            }
            auto [it, inserted] =
                active.emplace(job.job_id(), Active{job.lease(), {}});
            (void)inserted;
            stop = it->second.stop.get_token();
          }
          pool.submit([&, job, stop] {
            try {
              rg::execute(job, root, stop, emit, parallel);
            } catch (const std::exception &ex) {
              std::cerr << "job transport failed: " << ex.what() << '\n';
              ctx.TryCancel();
            }
            std::lock_guard lock(active_mutex);
            active.erase(job.job_id());
          });
        } else if (!received.cancel_job_id().empty()) {
          std::lock_guard lock(active_mutex);
          auto it = active.find(received.cancel_job_id());
          if (it != active.end() && it->second.lease == received.cancel_lease())
            it->second.stop.request_stop();
        }
      }
      {
        std::lock_guard lock(active_mutex);
        for (auto &[jid, a] : active) {
          (void)jid;
          a.stop.request_stop();
        }
      }
      ctx.TryCancel();
      heartbeat.request_stop();
      heartbeat.join();
      pool.shutdown();
      stream->WritesDone();
      auto status = stream->Finish();
      if (!status.ok())
        std::cerr << "stream closed: " << status.error_message() << '\n';
      if (!stopping)
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    std::cout << "worker stopped gracefully\n";
    return 0;
  } catch (const std::exception &ex) {
    std::cerr << ex.what() << '\n';
    return 1;
  }
}
