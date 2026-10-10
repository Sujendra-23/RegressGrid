#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>
#ifndef RG_LATENCY_MS
#define RG_LATENCY_MS 18.2
#endif
int main(int argc, char **argv) {
  const std::string test = argc > 1 ? argv[1] : "add";
  std::vector<double> a{1, 2, 3}, b{4, 5, 6};
  if (test == "add") {
    for (unsigned i = 0; i < a.size(); i++)
      if (a[i] + b[i] != static_cast<double>(5 + 2 * i))
        return 1;
  }
  if (test == "dot") {
    if (std::inner_product(a.begin(), a.end(), b.begin(), 0.0) != 32)
      return 1;
  }
  if (test == "benchmark") {
    // Controlled workload for a reproducible regression demo; the reported
    // latency is measured, while RG_LATENCY_MS controls the injected workload
    // duration.
    const auto start = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(
        std::chrono::duration<double, std::milli>(RG_LATENCY_MS));
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - start)
                          .count();
    std::ofstream f("regressgrid-metrics.json");
    f << "{\"latency_ms\":" << ms << ",\"throughput\":" << 1000.0 / ms << "}\n";
    if (!f)
      return 1;
  }
  std::cout << test << " passed\n";
  return 0;
}
