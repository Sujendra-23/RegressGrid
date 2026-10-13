#include "regressgrid/benchmark.hpp"
#include <fstream>
#include <iostream>

int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
      std::cout << "Usage: regressgrid-bench [--backend cpu|cuda] [--type float|double]\n"
                   "  [--elements N] [--warmup N] [--iterations N] [--output PATH]\n";
      return 0;
    }
    std::vector<std::string_view> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    const auto options = rg::bench::parse_options(args);
    rg::bench::Result result;
    if (options.backend == "cpu") {
      result = options.scalar == "float" ? rg::bench::run_cpu<float>(options)
                                         : rg::bench::run_cpu<double>(options);
    } else {
#ifdef RG_CUDA
      result = rg::bench::run_cuda(options);
#else
      throw std::runtime_error("CUDA backend unavailable; configure with -DRG_CUDA=ON");
#endif
    }
    // Publish only after all measurements and output verification succeed.
    std::ofstream output(options.output);
    if (!output) throw std::runtime_error("cannot open metrics output: " + options.output);
    rg::bench::write_json(output, options, result);
    output.close();
    if (!output) throw std::runtime_error("cannot close metrics output: " + options.output);
    std::cout << options.backend << '/' << options.scalar << ": " << result.latency_ms
              << " ms median, " << result.throughput << " elements/s\n";
  } catch (const std::exception &error) {
    std::cerr << "benchmark: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
