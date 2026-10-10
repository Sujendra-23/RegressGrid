#pragma once
#include "grid.pb.h"
#include <filesystem>
#include <functional>
#include <stop_token>
#include <utility>
namespace rg {
std::pair<unsigned, unsigned>
read_test_counts(const std::filesystem::path &path);
using EventSink = std::function<void(const regressgrid::v1::WorkerEvent &)>;
void execute(const regressgrid::v1::Assignment &job,
             const std::filesystem::path &root, std::stop_token stop,
             const EventSink &emit, unsigned build_parallelism);
} // namespace rg
