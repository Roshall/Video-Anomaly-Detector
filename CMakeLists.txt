cmake_minimum_required(VERSION 3.21)
project(Analyzer)

# Make built and installed binaries look for libs next to them
set(CMAKE_SKIP_BUILD_RPATH FALSE)
set(CMAKE_BUILD_RPATH_USE_ORIGIN TRUE)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)
# When installed, binaries will search ../lib (relative to bin/)
set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")

set(CMAKE_CXX_STANDARD 20)

set(CMAKE_BUILD_TYPE DEBUG)
# -march=native is invalid for a cross-compiler: it probes the host CPU, not the target.
# For aarch64 the correct flag (-march=armv8-a) is added below in the arch block.
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    SET(CMAKE_CXX_FLAGS_DEBUG   "$ENV{CXXFLAGS} -O0 -Wall -g2 -ggdb -fopenmp-simd -march=armv8-a -mno-outline-atomics")
    SET(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS} -O3 -Wall -fopenmp-simd -march=armv8-a -mno-outline-atomics")
else()
    SET(CMAKE_CXX_FLAGS_DEBUG   "$ENV{CXXFLAGS} -O0 -Wall -g2 -ggdb -fopenmp-simd -march=native")
    SET(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS} -O3 -Wall -fopenmp-simd -march=native")
endif()

set(ARCH_X86_64 "x86_64")
set(ARCH_AARCH64 "aarch64")
message(STATUS "System architecture: ${CMAKE_SYSTEM_PROCESSOR}")

option(USE_CUDA "Enable CUDA support" ON)
if(USE_CUDA)
    add_compile_definitions(USE_CUDA)
endif()

# Huawei Ascend (Atlas 300V Pro / Ascend 310P) hardware acceleration.
#
# Default OFF: without CANN the Ascend implementations compile as stubs that
# report "unavailable" and the CPU path keeps working, so this option only has
# to be flipped on the Kylin/aarch64 target that actually has the toolkit.
#
#   cmake -DUSE_ASCEND=ON -DASCEND_TOOLKIT_HOME=/usr/local/Ascend/ascend-toolkit/latest
option(USE_ASCEND "Enable Huawei Ascend (Atlas 300V Pro) support" OFF)
if(USE_ASCEND)
    add_compile_definitions(ENABLE_ASCEND)

    if(NOT DEFINED ASCEND_TOOLKIT_HOME)
        set(ASCEND_TOOLKIT_HOME "/usr/local/Ascend/ascend-toolkit/latest")
    endif()

    if(EXISTS "${ASCEND_TOOLKIT_HOME}")
        message(STATUS "Ascend toolkit found at: ${ASCEND_TOOLKIT_HOME}")
        set(ASCEND_INCLUDE_DIR "${ASCEND_TOOLKIT_HOME}/include")
        set(ASCEND_LIB_DIR "${ASCEND_TOOLKIT_HOME}/lib64")

        find_library(ACL_LIBRARY NAMES ascendcl PATHS ${ASCEND_LIB_DIR} NO_DEFAULT_PATH)
        find_library(ACL_RUNTIME_LIBRARY NAMES runtime PATHS ${ASCEND_LIB_DIR} NO_DEFAULT_PATH)
        if(ACL_LIBRARY)
            message(STATUS "Ascend ACL library found: ${ACL_LIBRARY}")
        else()
            message(WARNING "Ascend ACL library (libascendcl) not found in ${ASCEND_LIB_DIR}")
        endif()
    else()
        message(WARNING "Ascend toolkit not found at ${ASCEND_TOOLKIT_HOME}; set ASCEND_TOOLKIT_HOME")
    endif()
endif()

option(BUILD_TESTS "Build unit tests" ON)

# Optional sanitizer build. Example: -DANALYZER_SANITIZE=thread
set(ANALYZER_SANITIZE "" CACHE STRING "Sanitizer to enable (address, thread, undefined)")
if(ANALYZER_SANITIZE)
    message(STATUS "Enabling sanitizer: ${ANALYZER_SANITIZE}")
    add_compile_options(-fsanitize=${ANALYZER_SANITIZE} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${ANALYZER_SANITIZE})
    if(ANALYZER_SANITIZE STREQUAL "thread")
        # ONNX Runtime is not TSan-instrumented; model-backed tests skip
        # under TSan to avoid third-party false positives.
        add_compile_definitions(ANALYZER_TSAN=1)
    endif()
endif()

if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    set(THIRDPARTY_DIR "${CMAKE_SOURCE_DIR}/3rdparty-aarch64")
else()
    set(THIRDPARTY_DIR "${CMAKE_SOURCE_DIR}/3rdparty")
    list(APPEND CMAKE_PREFIX_PATH
            "${CMAKE_SOURCE_DIR}/3rdparty/onnxruntime-linux-x64-gpu-1.23.0"
    )
endif()

list(APPEND CMAKE_PREFIX_PATH "${THIRDPARTY_DIR}")

find_package(Threads REQUIRED)

# Find OpenCV
find_package(OpenCV REQUIRED)
if (OpenCV_FOUND)
    message(STATUS "OpenCV found: ${OpenCV_VERSION}")
else ()
    message(FATAL_ERROR "OpenCV not found")
endif ()

# Find libevent
find_package(Libevent REQUIRED)
if (Libevent_FOUND)
    message(STATUS "libevent found")
else ()
    message(FATAL_ERROR "libevent not found")
endif ()

# Find nlohmann_json
find_package(nlohmann_json CONFIG REQUIRED)
if (nlohmann_json_FOUND)
        message(STATUS "nlohmann_json found")
else ()
        message(FATAL_ERROR "nlohmann_json not found")
endif ()

# Find lock free queues
find_package(readerwriterqueue CONFIG REQUIRED)
if (readerwriterqueue_FOUND)
    message(STATUS "readerwriterqueue found")
else ()
    message(FATAL_ERROR "readerwriterqueue not found")
endif ()

find_package(concurrentqueue CONFIG REQUIRED)
if (concurrentqueue_FOUND)
    message(STATUS "concurrentqueue found")
else ()
    message(FATAL_ERROR "concurrentqueue not found")
endif ()

# Find spdlog
find_package(spdlog REQUIRED)
if (spdlog_FOUND)
    message(STATUS "spdlog found: ${spdlog_VERSION}")
else ()
    message(FATAL_ERROR "spdlog not found")
endif ()
add_compile_definitions(SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)

# Find FFmpeg and curl using pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavfilter libavcodec libavformat libavutil libswscale)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)

if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
   # Define the path to your local ONNX Runtime
   set(ONNXRUNTIME_ROOT "${CMAKE_SOURCE_DIR}/3rdparty-aarch64/onnxruntime-linux-aarch64-1.23.0")

   # 1. Find the header file
   find_path(ONNXRUNTIME_INCLUDE_DIR 
   NAMES onnxruntime_c_api.h 
   HINTS "${ONNXRUNTIME_ROOT}"
   PATH_SUFFIXES include
   NO_CMAKE_FIND_ROOT_PATH  # Crucial: allows searching outside the CMAKE_SYSROOT
   )

   # 2. Find the library file
   find_library(ONNXRUNTIME_LIBRARY 
   NAMES onnxruntime 
   HINTS "${ONNXRUNTIME_ROOT}"
   PATH_SUFFIXES lib
   NO_CMAKE_FIND_ROOT_PATH  # Crucial: allows searching outside the CMAKE_SYSROOT
   )

   # 3. Validation and Target Creation
   if (ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIBRARY)
   message(STATUS "ONNX Runtime found at: ${ONNXRUNTIME_LIBRARY}")

   # Create an IMPORTED target so you can just use 'target_link_libraries(your_app onnxruntime)'
   if(NOT TARGET onnxruntime)
           add_library(onnxruntime SHARED IMPORTED)
           set_target_properties(onnxruntime PROPERTIES
           IMPORTED_LOCATION "${ONNXRUNTIME_LIBRARY}"
           INTERFACE_INCLUDE_DIRECTORIES "${ONNXRUNTIME_INCLUDE_DIR}"
           )
   endif()
   else ()
   message(FATAL_ERROR "ONNX Runtime not found! Checked: ${ONNXRUNTIME_ROOT}")
   endif ()
else()
   # Find ONNX Runtime (prebuilt, no CMake config)
   find_path(ONNXRUNTIME_INCLUDE_DIR NAMES onnxruntime_c_api.h PATH_SUFFIXES include)
   find_library(ONNXRUNTIME_LIBRARY NAMES onnxruntime PATH_SUFFIXES lib)
   if (ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIBRARY)
   message(STATUS "ONNX Runtime found: ${ONNXRUNTIME_LIBRARY}")
   else ()
   message(FATAL_ERROR "ONNX Runtime not found")
   endif ()
   get_filename_component(ONNXRUNTIME_LIBDIR "${ONNXRUNTIME_LIBRARY}" DIRECTORY)
   # Create a library target for ONNX Runtime to simplify linking
   add_library(onnxruntime UNKNOWN IMPORTED)
   set_target_properties(onnxruntime PROPERTIES
           IMPORTED_LOCATION "${ONNXRUNTIME_LIBRARY}"
           INTERFACE_INCLUDE_DIRECTORIES "${ONNXRUNTIME_INCLUDE_DIR}"
   )
endif()


find_path(EIGEN_INCLUDE_DIR NAMES Eigen/Dense PATH_SUFFIXES include/eigen3)
if (EIGEN_INCLUDE_DIR)
    message(STATUS "Eigen found: ${EIGEN_INCLUDE_DIR}")
else ()
    message(FATAL_ERROR "Eigen not found")
endif ()

find_path(MINIAUDIO_INCLUDE_DIR NAMES miniaudio.h
    PATH_SUFFIXES include/miniaudio
)
find_library(MINIAUDIO_LIBRARY NAMES miniaudio PATH_SUFFIXES lib)

if (MINIAUDIO_INCLUDE_DIR AND MINIAUDIO_LIBRARY)
    message(STATUS "miniaudio found: ${MINIAUDIO_LIBRARY}")
else ()
    message(FATAL_ERROR "miniaudio not found")
endif ()

message(STATUS "Detected System Processor: ${CMAKE_SYSTEM_PROCESSOR}")

# 2. Conditional Dependency Check
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    # --- Kunpeng / openEuler Specifics ---
    message(STATUS "Architecture is aarch64. Checking for ALSA...")
    
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ALSA REQUIRED alsa)
    
    set(AUDIO_LIBS ${ALSA_LIBRARIES})
    
    add_definitions(-DMA_ENABLE_ALSA)

elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    # --- Intel / Ubuntu Specifics ---
    message(STATUS "Architecture is x86_64. Using local audio stack (Pulse/ALSA)...")
    
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ALSA REQUIRED alsa)
    pkg_check_modules(PULSE libpulse-simple) # Pulse is optional/preferred on Ubuntu
    
    set(AUDIO_LIBS ${ALSA_LIBRARIES} ${PULSE_LIBRARIES})
    add_definitions(-DMA_ENABLE_PULSEAUDIO -DMA_ENABLE_ALSA)
endif()

get_filename_component(MINIAUDIO_LIBDIR "${MINIAUDIO_LIBRARY}" DIRECTORY)
add_library(miniaudio UNKNOWN IMPORTED)
set_target_properties(miniaudio PROPERTIES
        IMPORTED_LOCATION "${MINIAUDIO_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${MINIAUDIO_INCLUDE_DIR}"
)

# ---------------------------------------------------------------------------
# analyzer_core: every Analyzer/Core source except main(). The executable and
# every test link this one library, so a new source file is added in exactly
# one place.
# ---------------------------------------------------------------------------
set(analyzer_core_source
        Analyzer/Core/admission.cpp
        Analyzer/Core/alarm.cpp
        Analyzer/Core/alarm/alarm_io_dispatcher.cpp
        Analyzer/Core/alarm/alarm_pipeline.cpp
        Analyzer/Core/alarm_sound.cpp
        Analyzer/Core/algorithm.cpp
        Analyzer/Core/analyzer.cpp
        Analyzer/Core/analyzer_registration.cpp
        Analyzer/Core/av_pull_stream.cpp
        Analyzer/Core/av_push_stream.cpp
        Analyzer/Core/bunkoff_analyzer.cpp
        Analyzer/Core/combined_event_manager.cpp
        Analyzer/Core/config.cpp
        Analyzer/Core/detection_algorithm.cpp
        Analyzer/Core/detector/constant_yolo.cpp
        Analyzer/Core/detector/inference.cpp
        Analyzer/Core/detector/yolo_detector_onnx.cpp
        Analyzer/Core/detector/yolo_detector_onnx.h
        Analyzer/Core/event.cpp
        Analyzer/Core/event/event_pipeline.cpp
        Analyzer/Core/event/event_runner.cpp
        Analyzer/Core/general_analyzer.cpp
        Analyzer/Core/imaging/opencv_image_processor.cpp
        Analyzer/Core/monitor/monitor_registry.cpp
        Analyzer/Core/offline/offline_pipeline.cpp
        Analyzer/Core/scheduler.cpp
        Analyzer/Core/server.cpp
        Analyzer/Core/server_apis/api_alarm.cpp
        Analyzer/Core/server_apis/api_common.cpp
        Analyzer/Core/server_apis/api_file.cpp
        Analyzer/Core/server_apis/api_media.cpp
        Analyzer/Core/server_apis/api_monitor.cpp
        Analyzer/Core/server_apis/api_rtsp.cpp
        Analyzer/Core/server_apis/api_storage.cpp
        Analyzer/Core/server_apis/api_system.cpp
        Analyzer/Core/server_apis/api_test.cpp
        Analyzer/Core/stream/decode_loop_policy.cpp
        Analyzer/Core/stream/frame_timestamp.cpp
        Analyzer/Core/stream/stream_session.cpp
        Analyzer/Core/streamer.cpp
        Analyzer/Core/task.cpp
        Analyzer/Core/track_algorithm.cpp
        Analyzer/Core/tracker/ByteTrack/BYTETracker.cpp
        Analyzer/Core/tracker/ByteTrack/KalmanFilter.cpp
        Analyzer/Core/tracker/ByteTrack/Object.cpp
        Analyzer/Core/tracker/ByteTrack/Rect.cpp
        Analyzer/Core/tracker/ByteTrack/STrack.cpp
        Analyzer/Core/tracker/ByteTrack/lapjv.cpp
        Analyzer/Core/Utils/bas64.cpp
        Analyzer/Core/Utils/common.cpp
        Analyzer/Core/Utils/log.cpp
        Analyzer/Core/Utils/log_formatter.cpp
        Analyzer/Core/Utils/request.cpp
        Analyzer/Core/Utils/fast_rm_files.cpp
        Analyzer/Core/Utils/file_server_cb.h
        Analyzer/Core/video_processing/annotator.cpp
        Analyzer/Core/video_processing/annotator.h
        Analyzer/Core/video_processing/dynamic_annotator.cpp
        Analyzer/Core/video_processing/format_convertor.cpp
        Analyzer/Core/video_processing/format_convertor.h
        Analyzer/Core/video_processing/frame_recorder.cpp
        Analyzer/Core/video_processing/frame_walk_time.h
        Analyzer/Core/video_processing/helper.cpp
        Analyzer/Core/video_processing/hls_utils.cpp
        Analyzer/Core/video_processing/jpeg_encoder.cpp
        Analyzer/Core/video_processing/painter.h
        Analyzer/Core/video_processing/painter.cpp
        Analyzer/Core/video_processing/painter_base.h
        Analyzer/Core/video_processing/painter_primitives.h
        Analyzer/Core/video_processing/painter_shapes.cpp
        Analyzer/Core/video_processing/painter_shapes.h
        Analyzer/Core/video_processing/painter_text.cpp
        Analyzer/Core/video_processing/painter_text.h
        Analyzer/Core/video_processing/painter_types.h
        # --- 双后端抽象层：推理引擎 / 图像处理引擎解耦 + Ascend 适配 ---
        Analyzer/Core/ascend/acl_runtime.cpp
        Analyzer/Core/backend/ascend_inference.cpp
        Analyzer/Core/imaging/dvpp_image_processor.cpp
        Analyzer/Core/pipeline/ascend_decoder.cpp
        Analyzer/Core/pipeline/ffmpeg_decoder.cpp
        Analyzer/Core/pipeline/pipeline_factories.cpp
        Analyzer/Core/worker.cpp
)

add_library(analyzer_core STATIC ${analyzer_core_source})
target_link_libraries(analyzer_core PUBLIC
        Threads::Threads
        atomic
        opencv_core
        opencv_imgproc
        opencv_dnn
        opencv_imgcodecs
        opencv_videoio
        libevent::core
        libevent::pthreads
        libevent::extra
        nlohmann_json::nlohmann_json
        PkgConfig::FFMPEG
        PkgConfig::CURL
        onnxruntime
        readerwriterqueue::readerwriterqueue
        concurrentqueue::concurrentqueue
        spdlog::spdlog
        ${AUDIO_LIBS}
        miniaudio
        PkgConfig::FREETYPE
)
target_include_directories(analyzer_core PUBLIC
        ${THIRDPARTY_DIR}/include
        ${OpenCV_INCLUDE_DIRS}
        ${FFMPEG_INCLUDE_DIRS}
        ${CURL_INCLUDE_DIRS}
        ${EIGEN_INCLUDE_DIR}
        ${FREETYPE_INCLUDE_DIRS}
        Analyzer/Core
        Analyzer/Core/video_processing
        Analyzer/Core/tracker
)

# Ascend: ACL headers + libraries, only when the toolkit was actually found.
if(USE_ASCEND AND ASCEND_INCLUDE_DIR)
    target_include_directories(analyzer_core PUBLIC ${ASCEND_INCLUDE_DIR})
endif()
if(USE_ASCEND AND ACL_LIBRARY)
    target_link_libraries(analyzer_core PUBLIC ${ACL_LIBRARY})
endif()
if(USE_ASCEND AND ACL_RUNTIME_LIBRARY)
    target_link_libraries(analyzer_core PUBLIC ${ACL_RUNTIME_LIBRARY})
endif()

add_executable(Analyzer Analyzer/main.cpp)
target_link_libraries(Analyzer PRIVATE analyzer_core)
target_link_options(Analyzer PRIVATE -Wl,--disable-new-dtags)

if(BUILD_TESTS)
    # google test
    include(FetchContent)
    FetchContent_Declare(
            googletest
            URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
            DOWNLOAD_EXTRACT_TIMESTAMP true
    )
    FetchContent_MakeAvailable(googletest)

    enable_testing()

    # The bundled FFmpeg in 3rdparty/lib must be preferred over system copies
    # for indirect dependencies (e.g. libavfilter -> libswresample).
    add_link_options(-Wl,--disable-new-dtags)

    # Shared include directories and dependencies for all test targets. Every
    # test links analyzer_core, so core sources are never listed twice.
    add_library(analyzer_test_common INTERFACE)
    target_include_directories(analyzer_test_common INTERFACE
            ${CMAKE_SOURCE_DIR}/Analyzer/Core
            ${CMAKE_SOURCE_DIR}/Analyzer/Core/detector
            ${CMAKE_SOURCE_DIR}/Analyzer/Core/video_processing
            ${CMAKE_SOURCE_DIR}/Analyzer/Core/tracker
            ${THIRDPARTY_DIR}/include
            ${OpenCV_INCLUDE_DIRS}
            ${FFMPEG_INCLUDE_DIRS}
            ${FREETYPE_INCLUDE_DIRS}
            ${EIGEN_INCLUDE_DIR}
    )
    target_link_libraries(analyzer_test_common INTERFACE
            analyzer_core
            GTest::gtest_main
            Threads::Threads
            spdlog::spdlog
            nlohmann_json::nlohmann_json
    )

    # Registers a test target and its CTest entry. `manual` targets need external
    # fixtures (models, RTSP URLs or local media), so they are registered but
    # disabled by default.
    function(analyzer_add_test target)
        add_executable(${target} ${ARGN})
        target_link_libraries(${target} PRIVATE analyzer_test_common)
        add_test(NAME ${target} COMMAND $<TARGET_FILE:${target}>)
    endfunction()

    function(analyzer_add_manual_test target)
        add_executable(${target} ${ARGN})
        target_link_libraries(${target} PRIVATE analyzer_test_common)
        add_test(NAME ${target} COMMAND $<TARGET_FILE:${target}>)
        set_tests_properties(${target} PROPERTIES DISABLED TRUE)
    endfunction()

    analyzer_add_test(yuv_test tests/yuv_test.cpp)

    analyzer_add_test(config_test tests/config_test.cpp)
    analyzer_add_test(detector_pool_test tests/detector_pool_test.cpp)
    set_tests_properties(detector_pool_test PROPERTIES TIMEOUT 120)
    analyzer_add_test(stream_log_test tests/stream_log_test.cpp)
    analyzer_add_test(event_submit_test tests/event_submit_test.cpp)
    analyzer_add_test(event_pipeline_test tests/event_pipeline_test.cpp)
    analyzer_add_test(event_runner_test tests/event_runner_test.cpp)
    analyzer_add_test(frame_timestamp_test tests/frame_timestamp_test.cpp)
    analyzer_add_test(decode_loop_policy_test tests/decode_loop_policy_test.cpp)

    # Requires an RTSP URL argument.
    analyzer_add_manual_test(write_test
            tests/test_helper.cpp
            tests/continuous_video_test.cpp)

    # Requires local media files.
    analyzer_add_manual_test(write_pict_test tests/save_pict_test.cpp)

    analyzer_add_test(concurrent_save_test tests/concurrent_save_test.cpp)

    # Require local models/images or MOT fixtures.
    analyzer_add_manual_test(yolo_test tests/yolo_test.cpp)
    analyzer_add_manual_test(tracker_test tests/tracker_test.cpp)

    analyzer_add_test(log_test tests/log_test.cpp)

    # Visual tools: without arguments they print usage and exit successfully.
    analyzer_add_test(draw_text_test tests/draw_text_test.cpp)
    analyzer_add_test(dynamic_annotator_test tests/dynamic_annotator_test.cpp)

    # Combined event edge transitions and the idle pre-roll buffer.
    analyzer_add_test(combined_event_manager_test
            tests/combined_event_manager_test.cpp)
    analyzer_add_test(event_buffer_test tests/event_buffer_test.cpp)

    # JPEG encode/decode round-trip used by the idle pre-roll buffer.
    analyzer_add_test(jpeg_roundtrip_test tests/jpeg_roundtrip_test.cpp)

    # Per-analyzer mailbox semantics: live overwrite, bounded offline backlog.
    analyzer_add_test(inference_mailbox_test tests/inference_mailbox_test.cpp)

    # Bounded result wait and Stop() semantics for Analyzer::FetchRawDetection.
    analyzer_add_test(analyzer_timeout_test tests/analyzer_timeout_test.cpp)

    # Starts the HTTP server and exercises endpoints.
    analyzer_add_test(test_server tests/test_server.cpp)

    # Monitor::ParseRecognitionRegion: single-polygon and JSON-array formats.
    analyzer_add_test(monitor_roi_test tests/monitor_roi_test.cpp)

    analyzer_add_test(frame_recorder_hls_test tests/frame_recorder_hls_test.cpp)

    # Admission control and shared-inference deduplication.
    analyzer_add_test(admission_test tests/admission_test.cpp)

    # Registry snapshots keep removed contexts alive without blocking.
    analyzer_add_test(monitor_registry_test tests/monitor_registry_test.cpp)

    # Integration tests exercising the full runtime.
    analyzer_add_test(alarm_job_test tests/alarm_job_test.cpp)
    analyzer_add_test(scheduler_lifecycle_test tests/scheduler_lifecycle_test.cpp)
    analyzer_add_test(alarm_dispatcher_test tests/alarm_dispatcher_test.cpp)
    analyzer_add_test(api_async_test tests/api_async_test.cpp)

# Ascend dual-backend abstraction tests (self-contained: gtest + opencv only).
add_executable(abstraction_test
        tests/imaging/frame_test.cpp
        tests/imaging/image_processor_contract_test.cpp
        tests/imaging/opencv_image_processor_test.cpp
        tests/backend/inference_backend_test.cpp
        tests/pipeline/pipeline_factory_test.cpp
        tests/architecture/layering_test.cpp
        tests/ascend/ascend_skeleton_test.cpp
        Analyzer/Core/imaging/opencv_image_processor.cpp
        Analyzer/Core/imaging/dvpp_image_processor.cpp
        Analyzer/Core/ascend/acl_runtime.cpp
        Analyzer/Core/backend/ascend_inference.cpp
        Analyzer/Core/pipeline/ascend_decoder.cpp
        Analyzer/Core/pipeline/ffmpeg_decoder.cpp
        Analyzer/Core/pipeline/pipeline_factories.cpp
        Analyzer/Core/Utils/log.cpp
        Analyzer/Core/Utils/log_formatter.cpp)
target_link_libraries(abstraction_test PRIVATE
        GTest::gtest_main
        opencv_core
        opencv_imgproc
        Threads::Threads
        spdlog::spdlog
        PkgConfig::FFMPEG)
target_include_directories(abstraction_test PRIVATE
        ${OpenCV_INCLUDE_DIRS}
        ${FFMPEG_INCLUDE_DIRS}
        ${CMAKE_SOURCE_DIR}/Analyzer/Core)
target_compile_definitions(abstraction_test PRIVATE
        ANALYZER_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
add_test(NAME abstraction_test COMMAND $<TARGET_FILE:abstraction_test>)

# ---- 端到端内存/延迟压测（mock YOLO 推理；不接解码/Worker/Scheduler）--------
#
# 目的：在没有 NPU/ONNX 的机器上也能量化"管线本身的内存与延迟"，
# 用模拟推理替换模型计算，其余链路（Analyzer → mailbox → ByteTrack →
# EventDetector）全部是真实代码。
#
# 注意：默认构建类型是 DEBUG（见文件顶部），跑压测必须显式
#   cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
# 否则 -O0 下的延迟数据没有参考价值（bench 自身也会打印 NDEBUG 警告）。
add_library(analyzer_bench_common INTERFACE)
target_include_directories(analyzer_bench_common INTERFACE
        ${CMAKE_SOURCE_DIR}/Analyzer/Core
        ${CMAKE_SOURCE_DIR}/Analyzer/Core/video_processing
        ${CMAKE_SOURCE_DIR}/Analyzer/Core/tracker
        ${THIRDPARTY_DIR}/include
        ${THIRDPARTY_DIR}/include/readerwriterqueue
        ${OpenCV_INCLUDE_DIRS}
        ${FFMPEG_INCLUDE_DIRS}
        ${FREETYPE_INCLUDE_DIRS}
        ${EIGEN_INCLUDE_DIR}
        # analyzer.h 经 detector/constant_yolo.h 传递包含 onnxruntime_cxx_api.h。
        # 这里只取头文件目录：被测目标不编译 yolo_detector_onnx.cpp /
        # inference.cpp，因此不需要链接 libonnxruntime。
        ${ONNXRUNTIME_INCLUDE_DIR}
)
target_link_libraries(analyzer_bench_common INTERFACE
        Threads::Threads
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        opencv_core
        opencv_imgproc
        atomic
)

# 压测核心 + 被测真实链路，编译一次，被 bench 可执行文件与冒烟用例共用。
add_library(analyzer_bench_lib OBJECT
        tests/bench/mock_inference.cpp
        tests/bench/rss_sampler.cpp
        tests/bench/latency_stats.cpp
        tests/bench/bench_harness.cpp
        # 分阶段组件：YUV->BGR 与 BGR->模型张量（后者是生产代码
        # OpenCvImageProcessor），用于分阶段定位瓶颈。
        # 注：未接入生产 FormatConvertor，它依赖 libav 的 AVFrame。
        tests/bench/pipeline_stages.cpp
        Analyzer/Core/imaging/opencv_image_processor.cpp
        Analyzer/Core/analyzer.cpp
        Analyzer/Core/analyzer_registration.cpp
        Analyzer/Core/algorithm.cpp
        Analyzer/Core/detection_algorithm.cpp
        Analyzer/Core/track_algorithm.cpp
        Analyzer/Core/bunkoff_analyzer.cpp
        Analyzer/Core/general_analyzer.cpp
        Analyzer/Core/event.cpp
        Analyzer/Core/admission.cpp
        Analyzer/Core/task.cpp
        Analyzer/Core/config.cpp
        Analyzer/Core/tracker/ByteTrack/BYTETracker.cpp
        Analyzer/Core/tracker/ByteTrack/KalmanFilter.cpp
        Analyzer/Core/tracker/ByteTrack/Object.cpp
        Analyzer/Core/tracker/ByteTrack/Rect.cpp
        Analyzer/Core/tracker/ByteTrack/STrack.cpp
        Analyzer/Core/tracker/ByteTrack/lapjv.cpp
        Analyzer/Core/Utils/log.cpp
        Analyzer/Core/Utils/log_formatter.cpp)
target_link_libraries(analyzer_bench_lib PUBLIC analyzer_bench_common)

# (A) 自带 main 的独立 bench：不链接 gtest，可脱离 ctest 单独跑，
#     便于用 /usr/bin/time -v 或读 /proc/self/statm 采集 RSS。
add_executable(analyzer_pipeline_bench
        tests/bench/bench_main.cpp
        $<TARGET_OBJECTS:analyzer_bench_lib>)
target_link_libraries(analyzer_pipeline_bench PRIVATE analyzer_bench_common)

# (B) 短跑版冒烟用例，进 ctest 防回归（阈值故意放宽）。
add_executable(analyzer_bench_smoke_test
        tests/bench/bench_smoke_test.cpp
        tests/bench/pipeline_stages_test.cpp
        $<TARGET_OBJECTS:analyzer_bench_lib>)
target_link_libraries(analyzer_bench_smoke_test PRIVATE
        analyzer_bench_common
        GTest::gtest_main)
add_test(NAME analyzer_bench_smoke_test COMMAND $<TARGET_FILE:analyzer_bench_smoke_test>)
set_tests_properties(analyzer_bench_smoke_test PROPERTIES
        TIMEOUT 180
        RUN_SERIAL TRUE        # 压测对 CPU 敏感，禁止与其它用例并发
        PROCESSORS 2
        LABELS "bench;perf")

endif()


# ---------------------------------------------------------------------------
# Deployment: library directories and C++ runtime bundling
# ---------------------------------------------------------------------------

# THIRDPARTY_LIB_DIRS tells the RUNTIME_DEPENDENCIES installer where to find
# our cross-compiled (or host-compiled) third-party shared libraries.
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    # CROSS_INSTALL_PREFIX is defined in the aarch64-toolchain.cmake file.
    # It points to /opt/aarch64-sysroot where all dependencies were installed.
    set(THIRDPARTY_LIB_DIRS
            "${CROSS_INSTALL_PREFIX}/lib"
    )
else()
    set(THIRDPARTY_LIB_DIRS
            "${CMAKE_SOURCE_DIR}/3rdparty/lib"
    )
endif()


# 1. Install the main executable
install(TARGETS Analyzer
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
)

# ---------------------------------------------------------------------------
# Helper: resolve a shared library to its real on-disk file and install it
# under the SONAME recorded in the ELF dynamic section.  The runtime linker
# resolves DT_NEEDED entries by SONAME (e.g. libfoo.so.1), not by the plain
# link-time symlink (libfoo.so), so using plain .so names causes "not found".
# ---------------------------------------------------------------------------
find_program(READELF_EXECUTABLE NAMES readelf)

function(install_lib_with_soname LIB_PATH DEST_DIR)
    if(NOT LIB_PATH OR NOT EXISTS "${LIB_PATH}")
        return()
    endif()
    # Dereference all symlinks to get the real file bytes
    file(REAL_PATH "${LIB_PATH}" _real)
    # Extract SONAME from ELF dynamic section
    execute_process(
        COMMAND "${READELF_EXECUTABLE}" -d "${_real}"
        OUTPUT_VARIABLE _elf_out
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    string(REGEX MATCH "\\(SONAME\\)[^\n]*\\[([^\n]+)\\]" _m "${_elf_out}")
    if(CMAKE_MATCH_1)
        install(FILES "${_real}" DESTINATION "${DEST_DIR}" RENAME "${CMAKE_MATCH_1}")
    else()
        get_filename_component(_name "${_real}" NAME)
        message(WARNING "No SONAME found in ${_real}; installing as ${_name}")
        install(FILES "${_real}" DESTINATION "${DEST_DIR}")
    endif()
endfunction()

# 2. Dependency Handling: Cross-Compiling vs. Native
if(CMAKE_CROSSCOMPILING)
    message(STATUS "Cross-compiling detected: Using manual dependency installation.")

    # --- A. ONNX Runtime ---
    if(ONNXRUNTIME_LIBRARY)
        install_lib_with_soname("${ONNXRUNTIME_LIBRARY}" lib)
    endif()

    # --- B. Miniaudio (static archive — no SONAME) ---
    if(MINIAUDIO_LIBRARY)
        if(MINIAUDIO_LIBRARY MATCHES "\\.a$")
            install(FILES "${MINIAUDIO_LIBRARY}" DESTINATION lib)
        else()
            install_lib_with_soname("${MINIAUDIO_LIBRARY}" lib)
        endif()
    endif()

    # --- C. spdlog ---
    # spdlog::spdlog may be header-only (INTERFACE) or a shared/static lib.
    get_target_property(_spdlog_loc spdlog::spdlog IMPORTED_LOCATION_RELEASE)
    if(NOT _spdlog_loc)
        get_target_property(_spdlog_loc spdlog::spdlog IMPORTED_LOCATION)
    endif()
    if(NOT _spdlog_loc OR NOT EXISTS "${_spdlog_loc}")
        find_library(_spdlog_loc spdlog
                     PATHS "${THIRDPARTY_LIB_DIRS}"
                     NO_DEFAULT_PATH)
    endif()
    if(_spdlog_loc AND EXISTS "${_spdlog_loc}")
        install_lib_with_soname("${_spdlog_loc}" lib)
    else()
        message(WARNING "spdlog shared library not found for bundling; skipping.")
    endif()
    unset(_spdlog_loc CACHE)

    # --- D. System & PkgConfig Libraries (FFmpeg, Curl, Freetype, ALSA, Libevent) ---
    set(CROSS_DEPS
        ${FFMPEG_LIBRARIES}
        postproc swresample  # transitive FFmpeg deps not in pkg_check_modules list
        x264                 # libavcodec encoder dependency
        ${CURL_LIBRARIES}
        ${FREETYPE_LIBRARIES}
        ${ALSA_LIBRARIES}
        event_core event_pthreads event_extra
    )
    foreach(lib_name ${CROSS_DEPS})
        find_library(_lib_path_${lib_name}
                     NAMES ${lib_name}
                     PATHS "${THIRDPARTY_LIB_DIRS}" "/usr/lib/aarch64-linux-gnu" "/lib/aarch64-linux-gnu"
                     NO_DEFAULT_PATH)
        if(_lib_path_${lib_name})
            install_lib_with_soname("${_lib_path_${lib_name}}" lib)
        else()
            message(WARNING "Library '${lib_name}' not found for bundling; skipping.")
        endif()
        unset(_lib_path_${lib_name} CACHE)
    endforeach()

    # --- E. OpenCV Libraries ---
    # OpenCV_LIBS contains imported target names, not file paths, so we must
    # query each target's location or fall back to find_library.
    set(_opencv_modules opencv_core opencv_imgproc opencv_dnn opencv_imgcodecs opencv_videoio)
    foreach(_cv_target ${_opencv_modules})
        unset(_cv_loc)
        unset(_cv_loc CACHE)
        if(TARGET ${_cv_target})
            get_target_property(_cv_loc ${_cv_target} IMPORTED_LOCATION_RELEASE)
            if(NOT _cv_loc)
                get_target_property(_cv_loc ${_cv_target} IMPORTED_LOCATION)
            endif()
        endif()
        if(NOT _cv_loc OR NOT EXISTS "${_cv_loc}")
            find_library(_cv_loc ${_cv_target}
                         PATHS "${THIRDPARTY_LIB_DIRS}"
                         NO_DEFAULT_PATH)
        endif()
        if(_cv_loc AND EXISTS "${_cv_loc}")
            install_lib_with_soname("${_cv_loc}" lib)
        else()
            message(WARNING "OpenCV library '${_cv_target}' not found for bundling; skipping.")
        endif()
        unset(_cv_loc CACHE)
    endforeach()

    # --- F. GCC C++ runtime ---
    # The deployment machine ships an older GCC runtime (openEuler 22.04 has
    # GCC 10/11) which is missing GLIBCXX_3.4.31+ symbols needed by C++20
    # features (std::format, std::jthread, etc.) compiled with GCC 13.
    # CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES is populated by CMake from the
    # active cross-compiler, so this finds the right versioned files regardless
    # of the exact GCC 13 installation prefix.
    # DT_RPATH=$ORIGIN/../lib (already set via --disable-new-dtags) ensures
    # these bundled copies take precedence over the system versions for the
    # Analyzer process and all libraries it loads transitively.
    foreach(_rtlib libstdc++.so.6 libgcc_s.so.1)
        foreach(_rtlibdir ${CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES})
            set(_rtlib_path "${_rtlibdir}/${_rtlib}")
            if(EXISTS "${_rtlib_path}")
                file(REAL_PATH "${_rtlib_path}" _rtlib_real)
                install(PROGRAMS "${_rtlib_real}" DESTINATION lib RENAME "${_rtlib}")
                message(STATUS "Bundling runtime lib: ${_rtlib_real} -> lib/${_rtlib}")
                break()
            endif()
        endforeach()
    endforeach()

else()
    # --- Native Build (x86_64) Logic ---
    # For native builds, RUNTIME_DEPENDENCIES works perfectly
    message(STATUS "Native build detected: Using RUNTIME_DEPENDENCIES scanner.")

    install(TARGETS Analyzer
            RUNTIME_DEPENDENCIES
            PRE_EXCLUDE_REGEXES
                ".*ld-linux.*\\.so.*" ".*linux-vdso.*" ".*libc\\.so.*"
                ".*libm\\.so.*" ".*libdl\\.so.*" ".*librt\\.so.*"
                ".*libpthread\\.so.*" ".*libresolv\\.so.*" ".*libnsl\\.so.*"
                ".*libgcc_s\\.so.*" ".*gtest.*" ".*gmock.*"
            POST_EXCLUDE_REGEXES
                ".*dri/.*"
            DIRECTORIES
                ${THIRDPARTY_LIB_DIRS}
    )
endif()

# 3. Install configuration file
install(FILES "${CMAKE_SOURCE_DIR}/conf/config.json" DESTINATION conf)

# 4. Install ALSA configuration directory
# Find the ALSA share directory and install it entirely
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    find_path(ALSA_SHARE_DIR alsa.conf
        PATHS "${CROSS_INSTALL_PREFIX}/share/alsa"
        NO_DEFAULT_PATH
    )
else()
    find_path(ALSA_SHARE_DIR alsa.conf
        PATH_SUFFIXES ../share/alsa share/alsa
        PATHS /usr /usr/local
    )
endif()
if(ALSA_SHARE_DIR)
    get_filename_component(ALSA_PARENT "${ALSA_SHARE_DIR}" DIRECTORY)
    get_filename_component(ALSA_TOP "${ALSA_PARENT}" DIRECTORY)
    install(DIRECTORY "${ALSA_TOP}/share/alsa" DESTINATION share)
    message(STATUS "ALSA share directory found: ${ALSA_TOP}/share/alsa")
else()
    message(WARNING "ALSA share directory not found; ALSA audio may not work correctly on target")
endif()

# Install test helper files
install(FILES "${CMAKE_SOURCE_DIR}/test_api.py"
    DESTINATION bin
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ WORLD_READ)
install(FILES "${CMAKE_SOURCE_DIR}/test_params.json"
    DESTINATION conf)
