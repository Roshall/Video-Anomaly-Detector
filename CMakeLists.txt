cmake_minimum_required(VERSION 3.16.0)
project(Analyzer)

set(CMAKE_CXX_STANDARD 20)

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

find_package(Threads REQUIRED)

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

# Find lock free queues
find_package(readerwriterqueue CONFIG REQUIRED)
if(readerwriterqueue_FOUND)
    message(STATUS "readerwriterqueue found")
else()
    message(FATAL_ERROR "readerwriterqueue not found")
endif()

find_package(concurrentqueue CONFIG REQUIRED)
if(concurrentqueue_FOUND)
    message(STATUS "readerwriterqueue found")
else()
    message(FATAL_ERROR "readerwriterqueue not found")
endif()

# Find FFmpeg and curl using pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavfilter libavcodec libavformat libavutil libswscale)
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

# google test
include(FetchContent)
FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP true
)

FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(tests tests/event_test.cpp Analyzer/Core/event.cpp)
target_link_libraries(tests GTest::gtest_main)
target_include_directories(tests PRIVATE Analyzer/Core)


add_executable(yuv_test tests/yuv_test.cpp)
target_link_libraries(yuv_test PRIVATE opencv_core)
target_include_directories(yuv_test PRIVATE ${OpenCV_INCLUDE_DIRS})

add_executable(detect_test Analyzer/Core/event.cpp tests/strategic_detect_test.cpp
        tests/test_helper.cpp)
add_executable(write_test
        Analyzer/Core/video_processing/frame_recorder.cpp
        Analyzer/Core/video_processing/helper.cpp
        tests/test_helper.cpp
        tests/continuous_video_test.cpp)

target_link_libraries(write_test PRIVATE PkgConfig::FFMPEG Threads::Threads)
target_include_directories(write_test PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_include_directories(write_test PRIVATE Analyzer/Core/video_processing)

add_executable(annotator_test
        Analyzer/Core/video_processing/annotator.cpp
        Analyzer/Core/video_processing/helper.cpp
        Analyzer/Core/video_processing/frame_recorder.cpp
        tests/test_helper.cpp
        tests/annotator_test.cpp
)
target_link_libraries(annotator_test PRIVATE PkgConfig::FFMPEG opencv_core opencv_imgproc)
target_include_directories(annotator_test PRIVATE ${FFMPEG_INCLUDE_DIRS} ${OpenCV_INCLUDE_DIRS})
target_include_directories(annotator_test PRIVATE Analyzer/Core/video_processing)
target_compile_definitions(annotator_test PRIVATE
        __STDC_CONSTANT_MACROS __STDC_FORMAT_MACROS __STDC_LIMIT_MACROS)

set(source
        Analyzer/Core/Utils/img_processing.cpp
        Analyzer/Core/Utils/request.cpp
        Analyzer/Core/algorithm.cpp
        Analyzer/Core/algorithm_on_yolo.cpp
        Analyzer/Core/analyzer.cpp
        Analyzer/Core/av_pull_stream.cpp
        Analyzer/Core/av_push_stream.cpp
        Analyzer/Core/config.cpp
        Analyzer/Core/frame.cpp
        Analyzer/Core/generate_alarm_video.cpp
        Analyzer/Core/scheduler.cpp
        Analyzer/Core/server.cpp
        Analyzer/Core/worker.cpp
        Analyzer/main.cpp
        Analyzer/Core/event.cpp
        Analyzer/Core/video_processing/frame_recorder.cpp
        Analyzer/Core/video_processing/annotator.cpp
        Analyzer/Core/video_processing/annotator.h
        Analyzer/Core/video_processing/format_convertor.cpp
        Analyzer/Core/video_processing/format_convertor.h)

add_executable(Analyzer ${source})
target_link_libraries(Analyzer PRIVATE
        Threads::Threads
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
        readerwriterqueue::readerwriterqueue
        concurrentqueue::concurrentqueue
)

# Include directories (if needed beyond what find_package provides)
target_include_directories(Analyzer PRIVATE
        ${OpenCV_INCLUDE_DIRS}
        ${FFMPEG_INCLUDE_DIRS}
        ${CURL_INCLUDE_DIRS}
)

target_include_directories(Analyzer PRIVATE Analyzer/Core Analyzer/Core/video_processing)