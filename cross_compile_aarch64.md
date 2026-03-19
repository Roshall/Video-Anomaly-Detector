# Cross-Compilation Guide: BXC_VideoAnalyzer for Phytium FT2000+ (aarch64 / openEuler 22.04)

**Host:** x86\_64 Docker container — Ubuntu 24.04
**Target:** aarch64 — Phytium FT2000+ running openEuler 22.04
**Compiler:** GCC 13 (`aarch64-linux-gnu-gcc-13` / `aarch64-linux-gnu-g++-13`)
**Custom sysroot:** `/opt/aarch64-sysroot` — all third-party libraries built from source and installed here
**ONNX Runtime:** official prebuilt aarch64 CPU binary (no NVIDIA GPU on FT2000+)

---

## Why GCC 13?

The project uses several C++20 features whose standard library implementations require GCC 13+:

| Feature | Minimum GCC |
|---|---|
| `std::format` | **13** |
| `std::jthread` | 11 |
| `std::counting_semaphore` | 11 |
| `std::ranges::any_of` / `ranges::transform` | 10 |

openEuler 22.04 ships with GCC 10 at its runtime ABI level (`libstdc++.so.6` GLIBCXX_3.4.28).
A binary compiled with GCC 13 requires `GLIBCXX_3.4.31`.
Because the target system cannot be guaranteed to have a new enough runtime, `libstdc++.so.6`
and `libgcc_s.so.1` from the GCC 13 cross-toolchain are **bundled** alongside the binary
at deploy time (handled automatically by the patched `CMakeLists.txt`).

Ubuntu 24.04 ships GCC 13 cross-compilers natively; no third-party PPA is required.

---

## Dependency Build Order

| # | Library | Version | Build method |
|---|---|---|---|
| 1 | alsa-lib | latest | autotools cross-compile |
| 2 | x264 | latest stable | `./configure --host=…` |
| 3a | libmp3lame | 3.100 | autotools cross-compile |
| 3b | libopus | 1.4 | autotools cross-compile |
| 3c | x265 | latest | cmake cross-compile *(optional)* |
| 4 | FFmpeg | 6.0 | `./configure --enable-cross-compile` |
| 5 | FreeType2 | 2.13.2 | autotools cross-compile |
| 6 | libcurl | 7.83.0 | autotools cross-compile |
| 7 | libevent | 2.1.12 | cmake + toolchain |
| 8 | nlohmann\_json | 3.11.3 | cmake + toolchain (header-only) |
| 9 | readerwriterqueue | latest | cmake + toolchain (header-only) |
| 10 | concurrentqueue | latest | cmake + toolchain (header-only) |
| 11 | spdlog | 1.14.1 | cmake + toolchain |
| 12 | Eigen3 | 3.4.0 | cmake + toolchain (header-only) |
| 13 | OpenCV | 4.12.0 + contrib | cmake + toolchain |
| 14 | miniaudio | latest | manual cross-compile to `.so` |
| 15 | BS thread-pool | 5.0.0 | header-only copy |
| 16 | ONNX Runtime | 1.23.0 | prebuilt aarch64 CPU binary |

---

## Phase 0 — Patches Applied to `CMakeLists.txt`

The following three changes have already been applied to `CMakeLists.txt`.
They are documented here for reference.

### 0.1 — `-march=native` conditional fix (was lines 14–15)

`-march=native` tells the compiler to probe and optimise for the CPU of the machine
**running the compiler** — the x86\_64 Docker host. With `aarch64-linux-gnu-g++` this
either silently produces wrong x86 tuning flags or errors outright. It also directly
conflicts with `-march=armv8.2-a` that the existing aarch64 architecture block already
adds via `add_compile_options`.

**Before:**
```cmake
SET(CMAKE_CXX_FLAGS_DEBUG   "$ENV{CXXFLAGS} -O0 -Wall -g2 -ggdb -fopenmp-simd -march=native")
SET(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS} -O3 -Wall -fopenmp-simd -march=native")
```

**After:**
```cmake
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    SET(CMAKE_CXX_FLAGS_DEBUG   "$ENV{CXXFLAGS} -O0 -Wall -g2 -ggdb -fopenmp-simd")
    SET(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS} -O3 -Wall -fopenmp-simd")
else()
    SET(CMAKE_CXX_FLAGS_DEBUG   "$ENV{CXXFLAGS} -O0 -Wall -g2 -ggdb -fopenmp-simd -march=native")
    SET(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS} -O3 -Wall -fopenmp-simd -march=native")
endif()
```

`CMAKE_SYSTEM_PROCESSOR` is set by the toolchain file before `CMakeLists.txt` is
processed, so this check resolves correctly at configure time.  The explicit
`-march=armv8.2-a` (Phytium FT2000+ is ARMv8.2-A) continues to be added by the
existing architecture block further down.

### 0.2 — ONNX Runtime path (was lines 28–31)

The x64 GPU prebuilt is not available for aarch64, and the FT2000+ has no NVIDIA GPU.
The aarch64 CPU prebuilt uses a different tarball name.

**Before:**
```cmake
list(APPEND CMAKE_PREFIX_PATH
        "${CMAKE_SOURCE_DIR}/3rdparty"
        "${CMAKE_SOURCE_DIR}/3rdparty/onnxruntime-linux-x64-gpu-1.23.0"
)
```

**After:**
```cmake
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    list(APPEND CMAKE_PREFIX_PATH
            "${CMAKE_SOURCE_DIR}/3rdparty"
            "${CMAKE_SOURCE_DIR}/3rdparty/onnxruntime-linux-aarch64-1.23.0"
    )
else()
    list(APPEND CMAKE_PREFIX_PATH
            "${CMAKE_SOURCE_DIR}/3rdparty"
            "${CMAKE_SOURCE_DIR}/3rdparty/onnxruntime-linux-x64-gpu-1.23.0"
    )
endif()
```

### 0.3 — `THIRDPARTY_LIB_DIRS` and GCC runtime bundling (new block before `install()`)

Two additions before the install block:

1. **`THIRDPARTY_LIB_DIRS`** — was referenced but never defined. Now set to
   `${CROSS_INSTALL_PREFIX}/lib` (aarch64) or `3rdparty/lib` (x86\_64). The variable
   is consumed by the `RUNTIME_DEPENDENCIES … DIRECTORIES` clause that tells CMake's
   dependency scanner where to find our custom `.so` files.

2. **Runtime library bundling** — an `install(PROGRAMS …)` loop that locates
   `libstdc++.so.6` and `libgcc_s.so.1` from `CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES`
   (automatically populated by CMake from the chosen compiler) and installs them into
   `lib/` alongside the binary. The symlink is resolved to the real versioned file but
   installed under the plain SONAME name that the dynamic linker expects.

Both `libstdc++.so.*` and `libgcc_s.so.*` are kept in `PRE_EXCLUDE_REGEXES` so the
`RUNTIME_DEPENDENCIES` scanner skips them; the explicit `install(PROGRAMS)` handles them.

---

## Phase 1 — Docker Environment

### 1.1 — Dockerfile

Ubuntu 24.04 is used because:
- GCC 13 cross-compilers are available without any third-party PPA.
- glibc 2.39 (build host) is newer than openEuler 22.04's glibc 2.34, which is
  irrelevant for cross-compilation — the cross-toolchain's internal sysroot determines
  the target ABI, not the host's glibc.

```dockerfile
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

ARG HTTP_PROXY
ARG HTTPS_PROXY
ENV HTTP_PROXY=${HTTP_PROXY}
ENV HTTPS_PROXY=${HTTPS_PROXY}

RUN apt-get update && apt-get install -y \
    # GCC 13 cross-compiler (full C++20: std::format, jthread, semaphore …)
    gcc-13-aarch64-linux-gnu \
    g++-13-aarch64-linux-gnu \
    binutils-aarch64-linux-gnu \
    # Build system tools
    cmake \
    ninja-build \
    make \
    autoconf \
    automake \
    libtool \
    pkg-config \
    # Assemblers needed by x264 and FFmpeg
    nasm \
    yasm \
    # General utilities
    git \
    wget \
    tar \
    xz-utils \
    python3 \
    && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /opt/aarch64-sysroot

ENV SYSROOT=/opt/aarch64-sysroot
```

Build the image and start a container, mounting both the project and the sysroot
(sysroot mounted from the host so it survives container restarts):

```bash
docker build --network host \
  --build-arg HTTP_PROXY=http://127.0.0.1:7890 \
  --build-arg HTTPS_PROXY=http://127.0.0.1:7890 \
  -t cross-builder .

docker run --rm -it \
    --network host -p 2222:22 \
    -v /home/cw/project/video_analysis_manager/VideoAnomalyWarningSystem:/project \
    -v /home/cw/project/video_analysis_manager/aarch64-sysroot:/opt/aarch64-sysroot \
    cross-builder bash
```

### 1.2 — Shell Environment Variables

Set these once per shell session inside the container.
They are referenced throughout Phases 2–4.

```bash
export SYSROOT=/opt/aarch64-sysroot
export CC=aarch64-linux-gnu-gcc-13
export CXX=aarch64-linux-gnu-g++-13
export AR=aarch64-linux-gnu-ar
export RANLIB=aarch64-linux-gnu-ranlib
export STRIP=aarch64-linux-gnu-strip
export TOOLCHAIN=/project/Analyzer/aarch64-toolchain.cmake
```

---

## Phase 2 — CMake Toolchain File

The file `Analyzer/aarch64-toolchain.cmake` has already been created alongside this
guide. Its content is reproduced here for reference:

```cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc-13)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++-13)
set(CMAKE_AR           aarch64-linux-gnu-ar)
set(CMAKE_RANLIB       aarch64-linux-gnu-ranlib)
set(CMAKE_STRIP        aarch64-linux-gnu-strip)

set(CROSS_INSTALL_PREFIX /opt/aarch64-sysroot)
list(APPEND CMAKE_PREFIX_PATH    ${CROSS_INSTALL_PREFIX})
list(APPEND CMAKE_FIND_ROOT_PATH ${CROSS_INSTALL_PREFIX})

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
```

> **Why `CMAKE_SYSROOT` is not set:**
> `CMAKE_SYSROOT` is passed to GCC as `--sysroot`.  It tells GCC where to find the
> C runtime startup files (`crt1.o`, `crti.o`) and core system libraries (`libc`,
> `libm`, `libstdc++`). These are already provided by the cross-toolchain at
> `/usr/aarch64-linux-gnu/`.  Redirecting `CMAKE_SYSROOT` to `/opt/aarch64-sysroot`
> would immediately break the link step:
> `cannot find crt1.o: No such file or directory`
>
> `CMAKE_FIND_ROOT_PATH` only affects CMake's own `find_*` functions — not GCC itself.
> It is the correct knob for pointing CMake at our third-party dependencies.

### 2.1 — pkg-config Environment Variables

CMake's `pkg_check_modules()` invokes the **host** `pkg-config` binary.  Without
redirection it searches `/usr/lib/pkgconfig` and returns x86\_64 metadata — completely
wrong for the cross build.  These three variables must be exported **before every
`cmake` invocation** in Phases 3 and 4:

```bash
export PKG_CONFIG_DIR=
export PKG_CONFIG_LIBDIR=${SYSROOT}/lib/pkgconfig:${SYSROOT}/share/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=${SYSROOT}
```

- `PKG_CONFIG_DIR=` — clears the default path; host packages are not visible.
- `PKG_CONFIG_LIBDIR` — the exclusive search list; points only at our sysroot.
- `PKG_CONFIG_SYSROOT_DIR` — prefixes all paths returned by pkg-config with the
  sysroot, producing correct absolute paths.

Set these now and keep them for the duration of the session.

---

## Phase 3 — Build All Third-Party Libraries

Create a scratch directory for source tarballs:

```bash
mkdir -p /build/src && cd /build/src
```

### 3.1 — alsa-lib (ALSA)

Required on aarch64 for audio output.  Must be built first because miniaudio links
against it.

```bash
cd /build/src
git clone https://github.com/alsa-project/alsa-lib.git
cd alsa-lib
autoreconf -fiv
./configure \
    --host=aarch64-linux-gnu \
    --prefix=${SYSROOT} \
    --disable-python \
    --with-pic
make -j$(nproc)
make install
```

Verify:
```bash
ls ${SYSROOT}/lib/libasound.so*
ls ${SYSROOT}/lib/pkgconfig/alsa.pc
```

### 3.2 — x264

Provides H.264 encoding for FFmpeg.

```bash
cd /build/src
git clone https://code.videolan.org/videolan/x264.git
cd x264
./configure \
    --host=aarch64-linux-gnu \
    --cross-prefix=aarch64-linux-gnu- \
    --prefix=${SYSROOT} \
    --enable-shared \
    --enable-pic \
    --disable-cli
make -j$(nproc)
make install
```

### 3.3 — FFmpeg prerequisites

These must be installed to `${SYSROOT}` before configuring FFmpeg.
 3.4 — FFmpeg 6.0

```bash
cd /build/src
wget https://ffmpeg.org/releases/ffmpeg-6.0.tar.xz
tar -xJf ffmpeg-6.0.tar.xz && cd ffmpeg-6.0

# Point FFmpeg's own configure-time pkg-config at our sysroot
export PKG_CONFIG_PATH=${SYSROOT}/lib/pkgconfig:${SYSROOT}/share/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=${SYSROOT}
export PKG_CONFIG_LIBDIR=${SYSROOT}/usr/lib/pkgconfig:${SYSROOT}/lib/pkgconfig

./configure \
    --prefix=${SYSROOT} \
    --enable-cross-compile \
    --cross-prefix=aarch64-linux-gnu- \
    --arch=aarch64 \
    --target-os=linux \
    --cc=aarch64-linux-gnu-gcc-13 \
    --cxx=aarch64-linux-gnu-g++-13 \
    --extra-cflags="-I${SYSROOT}/include" \
    --extra-ldflags="-L${SYSROOT}/lib" \
    --enable-shared \
    --enable-pic \
    --enable-gpl \
    --enable-nonfree \
    --enable-pthreads \
    --enable-libx264 \
    --disable-doc \
    --enable-asm \
    --disable-programs
# If you skipped § 3.3.c, remove --enable-libx265

make -j$(nproc) && make install
```

Verify pkg-config files were installed:
```bash
ls ${SYSROOT}/lib/pkgconfig/libavcodec.pc
```

### 3.5 — FreeType2

```bash
cd /build/src
wget https://downloads.sourceforge.net/project/freetype/freetype2/2.13.2/freetype-2.13.2.tar.xz
tar -xJf freetype-2.13.2.tar.xz && cd freetype-2.13.2
./configure \
    --host=aarch64-linux-gnu \
    --prefix=${SYSROOT} \
    --without-harfbuzz \
    --enable-shared \
    --with-pic
make -j$(nproc) && make install
```

### 3.6 — libcurl 7.83.0

```bash
cd /build/src
wget https://curl.se/download/curl-7.83.0.tar.xz
tar -xJf curl-7.83.0.tar.xz && cd curl-7.83.0
./configure \
    --host=aarch64-linux-gnu \
    --prefix=${SYSROOT} \
    --without-ssl \
    --enable-shared \
    --with-pic
make -j$(nproc) && make install
```

### 3.7 — libevent 2.1.12

Disable OpenSSL and the test/sample targets — test binaries cannot run on the host
during cross-compilation.

```bash
cd /build/src
wget https://github.com/libevent/libevent/releases/download/release-2.1.12-stable/libevent-2.1.12-stable.tar.gz
tar -xzf libevent-2.1.12-stable.tar.gz && cd libevent-2.1.12-stable
mkdir build-aarch64 && cd build-aarch64
cmake \
    -DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN} \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=${SYSROOT} \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DEVENT__DISABLE_OPENSSL=ON \
    -DEVENT__DISABLE_TESTS=ON \
    -DEVENT__DISABLE_SAMPLES=ON \
    -DEVENT__DISABLE_BENCHMARK=ON \
    ..
make -j$(nproc) && make install
```

### 3.8 — nlohmann\_json

```bash
cd /build/src
git clone --depth 1 --branch v3.11.3 https://github.com/nlohmann/json.git
cd json && mkdir build-aarch64 && cd build-aarch64
cmake \
    -DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN} \
    -DCMAKE_INSTALL_PREFIX=${SYSROOT} \
    -DJSON_BuildTests=OFF \
    ..
make install
```

### 3.9 — readerwriterqueue

```bash
cd /build/src
git clone --depth 1 https://github.com/cameron314/readerwriterqueue.git
cd readerwriterqueue && mkdir build-aarch64 && cd build-aarch64
cmake \
    -DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN} \
    -DCMAKE_INSTALL_PREFIX=${SYSROOT} \
    ..
make install
```

### 3.10 — concurrentqueue

```bash
cd /build/src
git clone --depth 1 https://github.com/cameron314/concurrentqueue.git
cd concurrentqueue && mkdir build-aarch64 && cd build-aarch64
cmake \
    -DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN} \
    -DCMAKE_INSTALL_PREFIX=${SYSROOT} \
    ..
make install
```

### 3.11 — spdlog

The project links against the compiled (non-header-only) `spdlog::spdlog` target:

```bash
cd /build/src
git clone --depth 1 --branch v1.14.1 https://github.com/gabime/spdlog.git
cd spdlog && mkdir build-aarch64 && cd build-aarch64
cmake \
    -DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN} \
    -DCMAKE_INSTALL_PREFIX=${SYSROOT} \
    -DCMAKE_BUILD_TYPE=Release \
    -DSPDLOG_BUILD_SHARED=ON \
    -DSPDLOG_BUILD_TESTS=OFF \
    -DSPDLOG_BUILD_EXAMPLE=OFF \
    ..
make -j$(nproc) && make install
```

### 3.12 — Eigen3

```bash
cd /build/src
git clone --depth 1 --branch 3.4.0 https://gitlab.com/libeigen/eigen.git
cd eigen && mkdir build-aarch64 && cd build-aarch64
cmake \
    -DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN} \
    -DCMAKE_INSTALL_PREFIX=${SYSROOT} \
    -DBUILD_TESTING=OFF \
    ..
make install
```

Verify `find_path(EIGEN_INCLUDE_DIR NAMES Eigen/Dense PATH_SUFFIXES include/eigen3)` will resolve:
```bash
ls ${SYSROOT}/include/eigen3/Eigen/Dense
```

### 3.13 — OpenCV 4.12.0 + contrib

The most complex dependency.  Key flag notes:

| Flag | Reason |
|---|---|
| `OPENCV_FORCE_3RDPARTY_BUILD=ON` | Build OpenCV's own libpng/libjpeg/zlib rather than accidentally picking up x86\_64 host versions |
| `ENABLE_NEON=ON` + `CPU_BASELINE=NEON` | FT2000+ is ARMv8.2-A; NEON is mandatory and activates SIMD-accelerated image processing |
| `WITH_CUDA=OFF` | No NVIDIA GPU on FT2000+ |
| `WITH_TBB=OFF`, `WITH_IPP=OFF` | Intel-specific, not applicable |

Ensure pkg-config variables are still exported (from § 2.1), then:

```bash
cd /build/src
wget https://github.com/opencv/opencv/archive/4.12.0.tar.gz -O opencv-4.12.0.tar.gz
wget https://github.com/opencv/opencv_contrib/archive/4.12.0.tar.gz -O opencv_contrib-4.12.0.tar.gz
tar -xzf opencv-4.12.0.tar.gz
tar -xzf opencv_contrib-4.12.0.tar.gz

cd opencv-4.12.0 && mkdir build-aarch64 && cd build-aarch64

cmake -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN} \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=${SYSROOT} \
    -DOPENCV_EXTRA_MODULES_PATH=/project/Analyzer-lib/opencv_contrib-4.12.0/modules \
    -DOPENCV_GENERATE_PKGCONFIG=ON \
    -DOPENCV_FORCE_3RDPARTY_BUILD=ON \
    -DWITH_TBB=OFF \
    -DWITH_IPP=OFF \
    -DENABLE_FAST_MATH=1 \
    -DWITH_FFMPEG=ON \
    -DWITH_CUDA=OFF \
    -DWITH_OPENCL=OFF \
    -DWITH_TIFF=OFF \
    -DBUILD_TESTS=OFF \
    -DBUILD_PERF_TESTS=OFF \
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_opencv_python3=OFF \
    -DINSTALL_C_EXAMPLES=OFF \
    -DCMAKE_TLS_VERIFY=0 \
    -DENABLE_NEON=ON \
    -DCPU_BASELINE=NEON \
    -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-rpath-link,${SYSROOT}/lib -Wl,-rpath-link,${SYSROOT}/usr/lib" \
    -DCMAKE_EXE_LINKER_FLAGS="-Wl,-rpath-link,${SYSROOT}/lib -Wl,-rpath-link,${SYSROOT}/usr/lib" \
    -DCMAKE_MODULE_LINKER_FLAGS="-Wl,-rpath-link,${SYSROOT}/lib -Wl,-rpath-link,${SYSROOT}/usr/lib" \
    ..

make -j$(nproc) && make install
```

Verify:
```bash
ls ${SYSROOT}/lib/pkgconfig/opencv4.pc
```

### 3.14 — miniaudio

miniaudio is a single-header library.  However, `CMakeLists.txt` requires it as a
compiled shared library via `find_library(MINIAUDIO_LIBRARY NAMES miniaudio PATH_SUFFIXES lib)`.
It must be compiled manually.

The library is built with `-DMA_ENABLE_ALSA` to match `add_definitions(-DMA_ENABLE_ALSA)`
in the aarch64 architecture block of `CMakeLists.txt`.

```bash
cd /build/src
mkdir miniaudio-build && cd miniaudio-build
wget https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h
```

Create the single implementation translation unit:

```bash
cat > miniaudio.c << 'EOF'
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
EOF
```

Cross-compile into a shared library:
mkdir build-aarch64 && cd build-aarch64

```bash
cmake -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN} \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=${SYSROOT} \
    -DCMAKE_SYSROOT=${SYSROOT} \
    -DCMAKE_C_FLAGS="-DMA_ENABLE_ALSA" \
    -DCMAKE_CXX_FLAGS="-DMA_ENABLE_ALSA" \
    -DCMAKE_EXE_LINKER_FLAGS="-lasound" \
    -DCMAKE_SHARED_LINKER_FLAGS="-lasound" \
    ..aarch64-linux-gnu-gcc-13 \
    -fPIC -shared \
    -o libminiaudio.so \
    miniaudio.c \
    -DMA_ENABLE_ALSA \
    -I${SYSROOT}/include \
    -L${SYSROOT}/lib \
    -lasound -lpthread -lm -ldl
cmake --build .
cmake --install .
```

Verify:
```bash
ls ${SYSROOT}/lib/libminiaudio.so
ls ${SYSROOT}/include/miniaudio/miniaudio.h
```

### 3.15 — BS thread-pool 5.0.0

Header-only copy; no compilation needed.

```bash
cd /build/src
wget https://github.com/bshoshany/thread-pool/archive/refs/tags/v5.0.0.tar.gz \
     -O thread-pool-5.0.0.tar.gz
tar -xzf thread-pool-5.0.0.tar.gz
mkdir -p ${SYSROOT}/include/threadpool
cp thread-pool-5.0.0/include/BS_thread_pool.hpp ${SYSROOT}/include/threadpool/
```

### 3.16 — ONNX Runtime 1.23.0 (prebuilt aarch64 CPU)

The FT2000+ has no NVIDIA GPU, so the CPU-only aarch64 binary is the correct artifact.
Extract it into the project's `3rdparty/` directory:

```bash
cd /work/Analyzer/3rdparty
wget https://github.com/microsoft/onnxruntime/releases/download/v1.23.0/onnxruntime-linux-aarch64-1.23.0.tgz
tar -xzf onnxruntime-linux-aarch64-1.23.0.tgz
```

The resulting directory structure:
```
3rdparty/onnxruntime-linux-aarch64-1.23.0/
    include/
        onnxruntime_c_api.h
        …
    lib/
        libonnxruntime.so -> libonnxruntime.so.1.23.0
        libonnxruntime.so.1.23.0
```

The Phase 0.2 patch causes `CMAKE_PREFIX_PATH` to include this directory when
`CMAKE_SYSTEM_PROCESSOR` is `aarch64` — no further configuration is needed.

---

## Phase 4 — Build the Project

Re-export environment variables if you have opened a new shell:

```bash
export SYSROOT=/opt/aarch64-sysroot
export TOOLCHAIN=/work/Analyzer/aarch64-toolchain.cmake
export PKG_CONFIG_DIR=
export PKG_CONFIG_LIBDIR=${SYSROOT}/lib/pkgconfig:${SYSROOT}/share/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=${SYSROOT}
```

Configure and build:

```bash
cd /work/Analyzer
mkdir -p build-aarch64 && cd build-aarch64

cmake \
    -DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN} \
    -DCMAKE_BUILD_TYPE=Release \
    -DUSE_CUDA=OFF \
    -DBUILD_TESTS=OFF \
    ..

make -j$(nproc)
```

**Flag notes:**

| Flag | Reason |
|---|---|
| `-DCMAKE_TOOLCHAIN_FILE` | Activates the cross-toolchain; sets `CMAKE_SYSTEM_PROCESSOR=aarch64` which triggers all Phase 0 patches |
| `-DUSE_CUDA=OFF` | FT2000+ has no NVIDIA GPU; removes the `USE_CUDA` preprocessor definition project-wide |
| `-DBUILD_TESTS=OFF` | Cross-compiled test binaries cannot execute on the x86 host; also avoids FetchContent downloading GoogleTest |

`-march=armv8.2-a` is applied automatically by `add_compile_options(-march=armv8.2-a)` in
the aarch64 architecture block — no conflict now that `-march=native` has been removed.

### 4.1 — Expected CMake configure output

```
-- System architecture: aarch64
-- Architecture is aarch64. Checking for ALSA...
-- OpenCV found: 4.12.0
-- libevent found
-- nlohmann_json found
-- readerwriterqueue found
-- spdlog found: 1.14.1
-- Eigen found: /opt/aarch64-sysroot/include/eigen3
-- miniaudio found: /opt/aarch64-sysroot/lib/libminiaudio.so
-- ONNX Runtime found: /work/Analyzer/3rdparty/onnxruntime-linux-aarch64-1.23.0/lib/libonnxruntime.so
-- Will bundle runtime lib: /usr/lib/gcc-cross/aarch64-linux-gnu/13/libstdc++.so.6.0.32 -> lib/libstdc++.so.6
-- Will bundle runtime lib: /usr/lib/gcc-cross/aarch64-linux-gnu/13/libgcc_s.so.1 -> lib/libgcc_s.so.1
```

If a `FATAL_ERROR` appears, common causes:

| Symptom | Cause |
|---|---|
| FFmpeg / ALSA / CURL / FREETYPE not found | `PKG_CONFIG_LIBDIR` not set or pointing at wrong path |
| nlohmann\_json / spdlog / libevent not found | CMake config files missing from `${SYSROOT}/lib/cmake/` |
| miniaudio not found | `${SYSROOT}/lib/libminiaudio.so` missing |
| ONNX Runtime not found | `3rdparty/onnxruntime-linux-aarch64-1.23.0/` not extracted |
| `libstdc++.so.6` bundle message missing | GCC 13 cross-compiler not installed or `CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES` path differs |

---

## Phase 5 — Deploy to the Target

### 5.1 — Assemble the deployment package

```bash
cd /work/Analyzer/build-aarch64
cmake --install . --prefix /tmp/analyzer-aarch64
```

The `install(TARGETS Analyzer RUNTIME_DEPENDENCIES …)` block in `CMakeLists.txt`
bundles all required shared libraries alongside the binary, excluding only the core
Linux ABI (`libc`, `libm`, `libpthread`, etc.) that is present on every system.
Additionally, `libstdc++.so.6` and `libgcc_s.so.1` from the GCC 13 cross-toolchain
are bundled by the explicit `install(PROGRAMS …)` loop.

Expected layout:
```
/tmp/analyzer-aarch64/
    bin/
        Analyzer
    lib/
        libstdc++.so.6            ← GCC 13 C++ runtime (bundled)
        libgcc_s.so.1             ← GCC support library  (bundled)
        libonnxruntime.so.1.23.0
        libopencv_core.so.4.12
        libopencv_imgproc.so.4.12
        …
        libavcodec.so.60
        libavformat.so.60
        …
        libminiaudio.so
        libevent-2.1.so.7
        libspdlog.so.1.14
        …
```

`CMAKE_INSTALL_RPATH="$ORIGIN/../lib"` (set at `CMakeLists.txt:9`) causes the binary
to search `../lib` relative to its own location at runtime — no `LD_LIBRARY_PATH`
manipulation is required on the target.

### 5.2 — Transfer

```bash
tar -czf analyzer-aarch64.tar.gz -C /tmp analyzer-aarch64
scp analyzer-aarch64.tar.gz user@ft2000-host:~
```

### 5.3 — Run on the target

```bash
ssh user@ft2000-host
tar -xzf analyzer-aarch64.tar.gz -C /opt/
/opt/analyzer-aarch64/bin/Analyzer
```

If shared library errors appear, inspect the embedded RPATH:
```bash
readelf -d /opt/analyzer-aarch64/bin/Analyzer | grep RPATH
```
It should show `$ORIGIN/../lib`.

To confirm which `libstdc++` is being loaded at runtime:
```bash
ldd /opt/analyzer-aarch64/bin/Analyzer | grep stdc
```
You should see `libstdc++.so.6 => /opt/analyzer-aarch64/lib/libstdc++.so.6` —
the **bundled** copy, not the system's potentially older one.

---

## Key Notes and Pitfalls

### Why `CMAKE_SYSROOT` Is Not Set in the Toolchain File

Setting `CMAKE_SYSROOT=/opt/aarch64-sysroot` would pass `--sysroot` to GCC,
redirecting it away from the cross-toolchain's own C runtime files at
`/usr/aarch64-linux-gnu/`.  The link step immediately fails:

```
/usr/bin/aarch64-linux-gnu-ld: cannot find crt1.o: No such file or directory
```

`CMAKE_FIND_ROOT_PATH` has no effect on GCC itself — only on CMake's `find_*`
functions.  It is the correct mechanism to point CMake at our custom dependencies
without touching GCC's own sysroot.

### pkg-config and Cross-Compilation

Every library found via `pkg_check_modules` — FFmpeg, ALSA, libcurl, FreeType2, and
OpenCV's internal FFmpeg detection — depends on `PKG_CONFIG_LIBDIR` pointing at the
sysroot's `.pc` files.  The three `PKG_CONFIG_*` variables must be set before every
`cmake` invocation in Phases 3 and 4.  Forgetting them is the most common source of
"library not found" errors.

### GCC 13 and `std::format`

`std::format` (used in this project) is in `<format>`, which requires GCC 13+ and the
matching `libstdc++.so.6` with ABI version `GLIBCXX_3.4.31`.  An earlier libstdc++ will
produce a link error (`undefined reference to std::__format::…`) or a runtime symbol
lookup failure.  The bundling approach resolves this: the binary always loads the GCC 13
runtime from its own `lib/` directory.

### `-march=native` and Cross-Compilers

`-march=native` probes the machine **running** the compiler. With
`aarch64-linux-gnu-g++-13` on an x86\_64 host the behaviour is version-dependent —
some GCC versions error, others silently ignore it.  Either outcome is incorrect for
an aarch64 target.  The Phase 0.1 patch removes it for aarch64; the existing
`add_compile_options(-march=armv8.2-a)` supplies the correct architecture flag.

### ONNX Runtime: GPU vs CPU

The x64 GPU prebuilt requires an NVIDIA GPU at runtime. FT2000+ has none.
The aarch64 CPU prebuilt uses only the CPU execution provider.  Combined with
`-DUSE_CUDA=OFF`, all GPU inference code paths are compiled out entirely.

### x265 Is Optional

For a surveillance analyzer that outputs H.264 HLS streams, x265 encoding is not
needed.  FFmpeg decodes H.265 input via its built-in software decoder without
requiring libx265.  Dropping `--enable-libx265` from the FFmpeg configure and
skipping § 3.3.c is the simplest path.

### FT2000+ NEON

ARMv8.2-A mandates NEON (Advanced SIMD).  `ENABLE_NEON=ON` and `CPU_BASELINE=NEON`
in the OpenCV build activate hand-written SIMD intrinsics for colour conversion,
resizing, and filtering — providing substantial throughput improvement for the video
analysis workload on the FT2000+'s cores.

---

## Quick Reference: Complete Build Sequence

```
Phase 0    CMakeLists.txt already patched (march, ONNX path, THIRDPARTY_LIB_DIRS, runtime bundling)
Phase 1    Build Docker image (Ubuntu 24.04), enter container, set env vars
Phase 2    aarch64-toolchain.cmake already created; export PKG_CONFIG_* vars
           ────────────────────────────────────────────────────────────────
Phase 3.1  alsa-lib          autotools  --host=aarch64-linux-gnu
Phase 3.2  x264              ./configure --host=aarch64-linux-gnu
Phase 3.3a libmp3lame        autotools  --host=aarch64-linux-gnu
Phase 3.3b libopus           autotools  --host=aarch64-linux-gnu
Phase 3.3c x265              cmake + toolchain (optional, 8-bit only)
Phase 3.4  FFmpeg 6.0        ./configure --enable-cross-compile (gcc-13)
Phase 3.5  FreeType2         autotools  --without-harfbuzz
Phase 3.6  libcurl 7.83.0    autotools  --without-ssl
Phase 3.7  libevent 2.1.12   cmake + toolchain
Phase 3.8  nlohmann_json     cmake + toolchain (header-only)
Phase 3.9  readerwriterqueue cmake + toolchain (header-only)
Phase 3.10 concurrentqueue   cmake + toolchain (header-only)
Phase 3.11 spdlog            cmake + toolchain
Phase 3.12 Eigen3            cmake + toolchain (header-only)
Phase 3.13 OpenCV 4.12       cmake + toolchain (ENABLE_NEON=ON)
Phase 3.14 miniaudio         aarch64-linux-gnu-gcc-13 -shared -DMA_ENABLE_ALSA
Phase 3.15 BS thread-pool    header copy only
Phase 3.16 ONNX Runtime      prebuilt tgz -> 3rdparty/onnxruntime-linux-aarch64-1.23.0/
           ────────────────────────────────────────────────────────────────
Phase 4    cmake -DUSE_CUDA=OFF -DBUILD_TESTS=OFF + make
Phase 5    cmake --install -> tar -> scp -> run on FT2000+
```

---

*Generated for BXC\_VideoAnalyzer v3.51 — host Ubuntu 24.04 x86\_64 — target openEuler 22.04 aarch64 Phytium FT2000+*
