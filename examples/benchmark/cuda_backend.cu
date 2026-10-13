#include "regressgrid/cuda_resources.hpp"
#include <cuda_runtime.h>

namespace rg::bench {
namespace {
void check(cudaError_t error, const char *operation) {
  if (error != cudaSuccess)
    throw std::runtime_error(std::string(operation) + ": " + cudaGetErrorString(error));
}

struct CudaRuntime {
  using Stream = cudaStream_t;
  using Event = cudaEvent_t;
  static Stream create_stream() {
    Stream stream{};
    check(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), "cudaStreamCreateWithFlags");
    return stream;
  }
  static void destroy_stream(Stream stream) noexcept { (void)cudaStreamDestroy(stream); }
  static Event create_event() {
    Event event{};
    check(cudaEventCreate(&event), "cudaEventCreate");
    return event;
  }
  static void destroy_event(Event event) noexcept { (void)cudaEventDestroy(event); }
  static void *allocate(std::size_t bytes) {
    void *pointer{};
    check(cudaMalloc(&pointer, bytes), "cudaMalloc");
    return pointer;
  }
  static void free(void *pointer) noexcept { (void)cudaFree(pointer); }
  static void record(Event event, Stream stream) { check(cudaEventRecord(event, stream), "cudaEventRecord"); }
  static void synchronize_stream(Stream stream) { check(cudaStreamSynchronize(stream), "cudaStreamSynchronize"); }
  static void synchronize_event(Event event) { check(cudaEventSynchronize(event), "cudaEventSynchronize"); }
  static double elapsed_ms(Event start, Event stop) {
    float ms{};
    check(cudaEventElapsedTime(&ms, start, stop), "cudaEventElapsedTime");
    return ms;
  }
};

template <class T>
__global__ void vector_add(const T *a, const T *b, T *out, std::size_t elements) {
  // Grid-stride indexing works beyond one block and avoids grid dimension limits.
  const auto stride = static_cast<std::size_t>(blockDim.x) * gridDim.x;
  for (auto i = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
       i < elements; i += stride)
    out[i] = a[i] + b[i];
}

template <class T> Result run(const Options &options) {
  validate(options);
  Vectors<T> host(options.elements);
  const auto bytes = byte_count<T>(options.elements);
  Stream<CudaRuntime> stream;
  DeviceBuffer<T, CudaRuntime> a(options.elements), b(options.elements), out(options.elements);
  EventTimer<CudaRuntime> timer(stream.get());
  check(cudaMemcpyAsync(a.get(), host.a.data(), bytes, cudaMemcpyHostToDevice, stream.get()), "copy a");
  check(cudaMemcpyAsync(b.get(), host.b.data(), bytes, cudaMemcpyHostToDevice, stream.get()), "copy b");
  check(cudaMemcpyAsync(out.get(), host.out.data(), bytes, cudaMemcpyHostToDevice, stream.get()), "initialize output");
  // Transfers and allocation are excluded; warmups use the measured stream.
  timer.synchronize();
  constexpr unsigned threads = 256;
  const auto blocks = static_cast<unsigned>(std::min<std::size_t>(
      65535, 1 + (options.elements - 1) / threads));
  auto samples = measure(options, [&] {
    vector_add<<<blocks, threads, 0, stream.get()>>>(a.get(), b.get(), out.get(), options.elements);
    check(cudaGetLastError(), "vector_add launch");
  }, timer);
  check(cudaMemcpyAsync(host.out.data(), out.get(), bytes, cudaMemcpyDeviceToHost, stream.get()), "copy output");
  timer.synchronize();
  host.verify();
  return summarize(options.elements, std::move(samples));
}
} // namespace

Result run_cuda(const Options &options) {
  return options.scalar == "float" ? run<float>(options) : run<double>(options);
}
} // namespace rg::bench
