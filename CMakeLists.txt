cmake_minimum_required(VERSION 3.16.0)
project(Analyzer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17 -pthread ")

set(CMAKE_BUILD_TYPE DEBUG)
SET(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} -O0 -Wall -g2 -ggdb")
SET(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS} -O3 -Wall")

set(ARCH_X86_64 "x86_64")
set(ARCH_AARCH64 "aarch64")
message(STATUS "System architecture: ${CMAKE_SYSTEM_PROCESSOR}")

list(APPEND CMAKE_PREFIX_PATH
    "${CMAKE_SOURCE_DIR}/3rdparty"
    "${CMAKE_SOURCE_DIR}/3rdparty/onnxruntime-linux-x64-1.22.0"
)

# Find OpenCV
find_package(OpenCV REQUIRED)
if(OpenCV_FOUND)
    message(STATUS "OpenCV found: ${OpenCV_VERSION}")
else()
    message(FATAL_ERROR "OpenCV not found")
endif()

# Find libevent
find_package(Libevent REQUIRED)
if(Libevent_FOUND)
    message(STATUS "libevent found")
else()
    message(FATAL_ERROR "libevent not found")
endif()

# Find jsoncpp
find_package(jsoncpp CONFIG REQUIRED)
if(jsoncpp_FOUND)
    message(STATUS "jsoncpp found")
else()
    message(FATAL_ERROR "jsoncpp not found")
endif()

# Find FFmpeg and curl using pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavformat libavutil libswscale)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)

# Find ONNX Runtime (prebuilt, no CMake config)
find_path(ONNXRUNTIME_INCLUDE_DIR NAMES onnxruntime_c_api.h PATH_SUFFIXES include)
find_library(ONNXRUNTIME_LIBRARY NAMES onnxruntime PATH_SUFFIXES lib)
if(ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIBRARY)
    message(STATUS "ONNX Runtime found: ${ONNXRUNTIME_LIBRARY}")
else()
    message(FATAL_ERROR "ONNX Runtime not found")
endif()

# Create a library target for ONNX Runtime to simplify linking
add_library(onnxruntime UNKNOWN IMPORTED)
set_target_properties(onnxruntime PROPERTIES
        IMPORTED_LOCATION "${ONNXRUNTIME_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${ONNXRUNTIME_INCLUDE_DIR}"
)

set(source
        Analyzer/Core/Utils/CalcuIOU.cpp
        Analyzer/Core/Utils/Request.cpp
        Analyzer/Core/Algorithm.cpp
        Analyzer/Core/AlgorithmOnYolo.cpp
        Analyzer/Core/Analyzer.cpp
        Analyzer/Core/AvPullStream.cpp
        Analyzer/Core/AvPushStream.cpp
        Analyzer/Core/Config.cpp
        Analyzer/Core/Frame.cpp
        Analyzer/Core/GenerateAlarmVideo.cpp
        Analyzer/Core/Scheduler.cpp
        Analyzer/Core/Server.cpp
        Analyzer/Core/Worker.cpp
        Analyzer/main.cpp
        Analyzer/Core/Analyzer.cpp)

add_executable(Analyzer ${source})
target_link_libraries(Analyzer PRIVATE
        opencv_core
        opencv_imgproc
        opencv_dnn
        opencv_imgcodecs
        libevent::core
        libevent::pthreads
        libevent::extra
        jsoncpp_lib
        PkgConfig::FFMPEG
        PkgConfig::CURL
        onnxruntime
)

# Include directories (if needed beyond what find_package provides)
target_include_directories(Analyzer PRIVATE
        ${OpenCV_INCLUDE_DIRS}
        ${FFMPEG_INCLUDE_DIRS}
        ${CURL_INCLUDE_DIRS}
)