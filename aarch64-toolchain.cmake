set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# ---------------------------------------------------------------------------
# Cross-compiler binaries (GCC 13 required for full C++20: std::format,
# std::jthread, std::counting_semaphore, ranges::transform, etc.)
# On Ubuntu 24.04:  apt install gcc-13-aarch64-linux-gnu g++-13-aarch64-linux-gnu
# ---------------------------------------------------------------------------
set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc-13)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++-13)
set(CMAKE_AR           aarch64-linux-gnu-ar)
set(CMAKE_RANLIB       aarch64-linux-gnu-ranlib)
set(CMAKE_STRIP        aarch64-linux-gnu-strip)

# ---------------------------------------------------------------------------
# Custom install prefix
#
# This is where all third-party libraries were cross-compiled and installed.
# It is used by CMake's find_* functions AND by CMakeLists.txt for
# THIRDPARTY_LIB_DIRS and the explicit runtime-lib bundling install() rules.
#
# NOTE: Do NOT set CMAKE_SYSROOT to this path.
#
# CMAKE_SYSROOT is passed to GCC as --sysroot; it redirects GCC's search
# for C runtime startup files (crt1.o, crti.o) and the core system libraries
# (libc, libm, libstdc++).  Those files live in the cross-toolchain's own
# internal sysroot at /usr/aarch64-linux-gnu/.  Pointing CMAKE_SYSROOT at
# our custom directory would cause an immediate link failure:
#   "cannot find crt1.o: No such file or directory"
#
# CMAKE_FIND_ROOT_PATH, on the other hand, only influences CMake's own
# find_package(), find_library(), find_path(), and find_program() calls —
# not GCC itself.  This is the correct knob for pointing CMake at our
# third-party dependencies without disturbing the toolchain's sysroot.
# ---------------------------------------------------------------------------
set(CROSS_INSTALL_PREFIX /opt/aarch64-sysroot)

list(APPEND CMAKE_PREFIX_PATH    ${CROSS_INSTALL_PREFIX})
list(APPEND CMAKE_FIND_ROOT_PATH ${CROSS_INSTALL_PREFIX})

# NEVER  — host executables (cmake, python, pkg-config …) must come from the
#          Docker host, not from the aarch64 sysroot.
# ONLY   — search only the cross-root for target libraries, headers, and
#          CMake config packages.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
