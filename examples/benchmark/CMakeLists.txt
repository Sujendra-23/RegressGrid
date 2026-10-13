cmake_minimum_required(VERSION 3.22)
project(regressgrid_benchmark LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(RG_CUDA "Build the CUDA vector benchmark (requires toolkit and GPU to run)" OFF)
option(RG_SANITIZERS "Enable host AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
include(CTest)

add_library(benchmark_options INTERFACE)
target_include_directories(benchmark_options INTERFACE include)
target_compile_options(benchmark_options INTERFACE
  $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic;-Werror>)
if(RG_SANITIZERS)
  if(RG_CUDA)
    message(FATAL_ERROR "Use a CPU build for host sanitizers; use compute-sanitizer for CUDA")
  endif()
  target_compile_options(benchmark_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(benchmark_options INTERFACE -fsanitize=address,undefined)
endif()

add_executable(regressgrid-bench main.cpp)
target_link_libraries(regressgrid-bench PRIVATE benchmark_options)
if(RG_CUDA)
  enable_language(CUDA)
  find_package(CUDAToolkit REQUIRED)
  target_sources(regressgrid-bench PRIVATE cuda_backend.cu)
  target_link_libraries(regressgrid-bench PRIVATE CUDA::cudart)
  target_compile_definitions(regressgrid-bench PRIVATE RG_CUDA=1)
  set_target_properties(regressgrid-bench PROPERTIES CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON)
endif()

if(BUILD_TESTING)
  add_executable(benchmark-tests tests/benchmark_test.cpp)
  target_link_libraries(benchmark-tests PRIVATE benchmark_options)
  add_test(NAME benchmark.unit COMMAND benchmark-tests)
  add_test(NAME benchmark.cli COMMAND ${CMAKE_COMMAND}
    -DBENCH=$<TARGET_FILE:regressgrid-bench> -DHAS_CUDA=${RG_CUDA}
    -DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-test
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_test.cmake)
  set_tests_properties(benchmark.unit benchmark.cli PROPERTIES TIMEOUT 30)
  foreach(scalar float double)
    # Separate files allow parallel CTest execution without metrics races.
    if(scalar STREQUAL "float")
      # The host test also fulfills the submitted-repository metrics contract.
      set(workload --elements 1048576 --warmup 5 --iterations 30)
      set(metrics ${CMAKE_CURRENT_BINARY_DIR}/regressgrid-metrics.json)
    else()
      set(workload --elements 1025 --warmup 1 --iterations 3)
      set(metrics ${CMAKE_CURRENT_BINARY_DIR}/cpu-${scalar}.json)
    endif()
    add_test(NAME benchmark.cpu.${scalar} COMMAND regressgrid-bench
      --type ${scalar} ${workload} --output ${metrics})
    set_tests_properties(benchmark.cpu.${scalar} PROPERTIES TIMEOUT 30)
    if(RG_CUDA)
      add_test(NAME benchmark.cuda.${scalar} COMMAND regressgrid-bench
        --backend cuda --type ${scalar} --elements 1025 --warmup 1 --iterations 3
        --output ${CMAKE_CURRENT_BINARY_DIR}/cuda-${scalar}.json)
      set_tests_properties(benchmark.cuda.${scalar} PROPERTIES
        LABELS gpu TIMEOUT 60 RESOURCE_LOCK regressgrid_gpu)
    endif()
  endforeach()
endif()
install(TARGETS regressgrid-bench RUNTIME DESTINATION bin)
