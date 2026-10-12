# CMake toolchain file: cross-compile the worker for 64-bit Arm Linux (glibc)
# from an x86-64 Debian/Ubuntu host. Target libraries (gRPC, Protobuf, OpenSSL,
# GoogleTest) come from Debian multiarch (`dpkg --add-architecture arm64`), so
# they live under /usr/lib/aarch64-linux-gnu. See Dockerfile.worker-aarch64.
#
#   cmake -S worker -B build/aarch64 -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=worker/cmake/aarch64-linux-gnu.cmake \
#     -DRG_HOST_PROTOC=/usr/bin/protoc \
#     -DRG_HOST_GRPC_PLUGIN=/usr/bin/grpc_cpp_plugin
#
# protoc and grpc_cpp_plugin run on the build machine while generating sources,
# so the host (x86-64) copies must be passed in explicitly.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(RG_TRIPLE aarch64-linux-gnu)
set(CMAKE_C_COMPILER ${RG_TRIPLE}-gcc)
set(CMAKE_CXX_COMPILER ${RG_TRIPLE}-g++)
set(CMAKE_LIBRARY_ARCHITECTURE ${RG_TRIPLE})
# gRPC's CMake config locates RE2, c-ares and others through pkg-config; point it
# at the arm64 .pc files instead of the host's.
set(ENV{PKG_CONFIG_LIBDIR} /usr/lib/${RG_TRIPLE}/pkgconfig:/usr/share/pkgconfig)
set(CMAKE_FIND_ROOT_PATH /usr/${RG_TRIPLE} /usr/lib/${RG_TRIPLE} /usr)
# Build-time programs (cmake, protoc, ...) come from the host; libraries,
# headers, and CMake packages may come from the target multiarch directories.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
# Let ctest and gtest_discover_tests run target binaries under qemu-user. The
# multiarch libc6:arm64 provides /lib/ld-linux-aarch64.so.1, so "-L /" works.
find_program(RG_QEMU_AARCH64 qemu-aarch64)
if(RG_QEMU_AARCH64)
  set(CMAKE_CROSSCOMPILING_EMULATOR ${RG_QEMU_AARCH64} -L /)
endif()
