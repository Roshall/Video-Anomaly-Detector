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
    SET(CMAKE_CXX_FLAGS_DEBUG   "$ENV{CXXFLAGS} -O0 -Wall -g2 -ggdb -fopenmp-simd")
    SET(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS} -O3 -Wall -fopenmp-simd -march=armv8-a")
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

option(BUILD_TESTS "Build unit tests" OFF)

if(NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    list(APPEND CMAKE_PREFIX_PATH
            "${CMAKE_SOURCE_DIR}/3rdparty"
            "${CMAKE_SOURCE_DIR}/3rdparty/onnxruntime-linux-x64-gpu-1.23.0"
    )
endif()

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
    
    # Apply Kunpeng optimizations directly if found
    add_compile_options(-march=armv8.2-a)
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
        Analyzer/Core/video_processing/hls_utils.cpp
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
        Analyzer/Core/video_processing/hls_utils.cpp
        tests/test_helper.cpp
        tests/annotator_test.cpp
)
target_link_libraries(annotator_test PRIVATE PkgConfig::FFMPEG opencv_core opencv_imgproc)
target_include_directories(annotator_test PRIVATE ${FFMPEG_INCLUDE_DIRS} ${OpenCV_INCLUDE_DIRS})
target_include_directories(annotator_test PRIVATE Analyzer/Core/video_processing)
target_compile_definitions(annotator_test PRIVATE
        __STDC_CONSTANT_MACROS __STDC_FORMAT_MACROS __STDC_LIMIT_MACROS)

add_executable(write_pict_test
        Analyzer/Core/video_processing/jpeg_encoder.cpp
        Analyzer/Core/video_processing/helper.cpp
        tests/save_pict_test.cpp
)
target_link_libraries(write_pict_test PRIVATE PkgConfig::FFMPEG)
target_include_directories(write_pict_test PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_include_directories(write_pict_test PRIVATE
        Analyzer/Core/video_processing)

add_executable(concurrent_save_test
        Analyzer/Core/video_processing/jpeg_encoder.cpp
        Analyzer/Core/Utils/log.cpp
        Analyzer/Core/Utils/log_formatter.cpp
        tests/concurrent_save_test.cpp
)
target_link_libraries(concurrent_save_test PRIVATE PkgConfig::FFMPEG Threads::Threads spdlog::spdlog)
target_include_directories(concurrent_save_test PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_include_directories(concurrent_save_test PRIVATE
        Analyzer/Core/video_processing
        Analyzer/Core)
add_test(NAME concurrent_save_test COMMAND $<TARGET_FILE:concurrent_save_test>)

add_executable(yolo_test
        Analyzer/Core/detector/inference.h
        Analyzer/Core/detector/yolo_detector_onnx.cpp
        Analyzer/Core/detector/constant_pool.h
        Analyzer/Core/detector/constant_yolo.cpp
        Analyzer/Core/detector/inference.cpp
        Analyzer/Core/task.cpp
        tests/yolo_test.cpp
)
target_link_libraries(yolo_test PRIVATE PkgConfig::FFMPEG opencv_core opencv_imgproc opencv_imgcodecs opencv_dnn_objdetect onnxruntime)
target_include_directories(yolo_test PRIVATE ${FFMPEG_INCLUDE_DIRS} ${OpenCV_INCLUDE_DIRS})
target_include_directories(yolo_test PRIVATE Analyzer/Core/detector Analyzer/Core)
target_compile_definitions(yolo_test PRIVATE
        __STDC_CONSTANT_MACROS __STDC_FORMAT_MACROS __STDC_LIMIT_MACROS)

add_executable(tracker_test
        Analyzer/Core/detector/yolo_detector_onnx.cpp
        Analyzer/Core/detector/constant_pool.h
        Analyzer/Core/tracker/ByteTrack/BYTETracker.cpp
        Analyzer/Core/tracker/ByteTrack/KalmanFilter.cpp
        Analyzer/Core/tracker/ByteTrack/lapjv.cpp
        Analyzer/Core/tracker/ByteTrack/Object.cpp
        Analyzer/Core/tracker/ByteTrack/Rect.cpp
        Analyzer/Core/tracker/ByteTrack/STrack.cpp
        Analyzer/Core/detector/constant_yolo.cpp
        Analyzer/Core/detector/inference.cpp
        Analyzer/Core/task.cpp
        Analyzer/Core/track_algorithm.cpp
        Analyzer/Core/algorithm.cpp
        Analyzer/Core/config.cpp
        tests/tracker_test.cpp
)
target_link_libraries(tracker_test PRIVATE PkgConfig::FFMPEG opencv_core opencv_imgproc opencv_imgcodecs opencv_dnn_objdetect opencv_video opencv_videoio onnxruntime nlohmann_json::nlohmann_json)
target_include_directories(tracker_test PRIVATE ${FFMPEG_INCLUDE_DIRS} ${OpenCV_INCLUDE_DIRS})
target_include_directories(tracker_test PRIVATE Analyzer/Core Analyzer/Core/tracker ${EIGEN_INCLUDE_DIR})
target_compile_definitions(tracker_test PRIVATE
        __STDC_CONSTANT_MACROS __STDC_FORMAT_MACROS __STDC_LIMIT_MACROS)

add_executable(log_test
        Analyzer/Core/Utils/log_formatter.cpp
        Analyzer/Core/Utils/log.cpp
        tests/log_test.cpp
)
target_link_libraries(log_test PRIVATE spdlog::spdlog)
target_include_directories(log_test PRIVATE Analyzer/Core/Utils)

add_executable(draw_text_test Analyzer/Core/video_processing/painter_text.cpp
        Analyzer/Core/video_processing/painter_shapes.cpp
        tests/draw_text_test.cpp
)
target_link_libraries(draw_text_test PRIVATE PkgConfig::FREETYPE PkgConfig::FFMPEG opencv_core)
target_include_directories(draw_text_test PRIVATE ${FFMPEG_INCLUDE_DIRS} ${FREETYPE_INCLUDE_DIRS} ${OpenCV_INCLUDE_DIRS})
target_include_directories(draw_text_test PRIVATE Analyzer/Core/video_processing Analyzer/Core)

add_executable(dynamic_annotator_test
        Analyzer/Core/video_processing/dynamic_annotator.cpp
        Analyzer/Core/video_processing/painter.cpp
        Analyzer/Core/video_processing/painter_text.cpp
        Analyzer/Core/video_processing/painter_shapes.cpp
        Analyzer/Core/video_processing/helper.cpp
        Analyzer/Core/task.cpp
        Analyzer/Core/Utils/log.cpp
        Analyzer/Core/Utils/log_formatter.cpp
        tests/dynamic_annotator_test.cpp)
target_link_libraries(dynamic_annotator_test PRIVATE PkgConfig::FFMPEG opencv_core opencv_imgproc PkgConfig::FREETYPE nlohmann_json::nlohmann_json)
target_include_directories(dynamic_annotator_test PRIVATE ${FFMPEG_INCLUDE_DIRS} ${FREETYPE_INCLUDE_DIRS} ${OpenCV_INCLUDE_DIRS})
target_include_directories(dynamic_annotator_test PRIVATE Analyzer/Core/video_processing Analyzer/Core)

# Unit tests for CombinedEventManager edge transitions
add_executable(combined_event_manager_test
        tests/combined_event_manager_test.cpp
        tests/dynamic_annotator_stub.cpp
        Analyzer/Core/combined_event_manager.cpp)
target_link_libraries(combined_event_manager_test PRIVATE GTest::gtest_main PkgConfig::FFMPEG opencv_core opencv_imgproc)
target_include_directories(combined_event_manager_test PRIVATE ${FFMPEG_INCLUDE_DIRS} ${OpenCV_INCLUDE_DIRS} Analyzer/Core Analyzer/Core/video_processing)
add_test(NAME combined_event_manager_test COMMAND $<TARGET_FILE:combined_event_manager_test>)
# Test executable that starts the HTTP server and exercises endpoints
add_executable(test_server
        tests/test_server.cpp
)
target_link_libraries(test_server PRIVATE
        GTest::gtest_main
        Threads::Threads
        libevent::core
        libevent::pthreads
        libevent::extra
)
target_include_directories(test_server PRIVATE
        Analyzer/Core
)
add_test(NAME test_server COMMAND $<TARGET_FILE:test_server>)

add_executable(frame_recorder_hls_test
        tests/frame_recorder_hls_test.cpp
        Analyzer/Core/video_processing/frame_recorder.cpp
        Analyzer/Core/video_processing/hls_utils.cpp
        Analyzer/Core/video_processing/helper.cpp
        Analyzer/Core/Utils/log.cpp
        Analyzer/Core/Utils/log_formatter.cpp)
target_link_libraries(frame_recorder_hls_test PRIVATE
        GTest::gtest_main
        PkgConfig::FFMPEG
        Threads::Threads
        spdlog::spdlog)
target_link_directories(frame_recorder_hls_test PRIVATE ${FFMPEG_LIBRARY_DIRS})
set_target_properties(frame_recorder_hls_test PROPERTIES
        BUILD_RPATH "${FFMPEG_LIBRARY_DIRS}")
target_include_directories(frame_recorder_hls_test PRIVATE
        ${FFMPEG_INCLUDE_DIRS}
        Analyzer/Core
        Analyzer/Core/video_processing)
add_test(NAME frame_recorder_hls_test COMMAND $<TARGET_FILE:frame_recorder_hls_test>)
endif()

set(source
        Analyzer/Core/alarm.cpp
        Analyzer/Core/alarm_sound.cpp
        Analyzer/Core/algorithm.cpp
        Analyzer/Core/analyzer.cpp
        Analyzer/Core/av_pull_stream.cpp
        Analyzer/Core/av_push_stream.cpp
        Analyzer/Core/bunkoff_analyzer.cpp
        Analyzer/Core/combined_event_manager.cpp
        Analyzer/Core/config.cpp
        Analyzer/Core/detection_algorithm.cpp
        Analyzer/Core/detector/constant_pool.h
        Analyzer/Core/detector/constant_yolo.cpp
        Analyzer/Core/detector/inference.cpp
        Analyzer/Core/detector/yolo_detector_onnx.cpp
        Analyzer/Core/detector/yolo_detector_onnx.h
        Analyzer/Core/event.cpp
        Analyzer/Core/general_analyzer.cpp
        Analyzer/Core/scheduler.cpp
        Analyzer/Core/server.cpp
        Analyzer/Core/server_apis/api_alarm.cpp
        Analyzer/Core/server_apis/api_common.cpp
        Analyzer/Core/server_apis/api_media.cpp
        Analyzer/Core/server_apis/api_monitor.cpp
        Analyzer/Core/server_apis/api_rtsp.cpp
        Analyzer/Core/server_apis/api_storage.cpp
        Analyzer/Core/server_apis/api_system.cpp
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
        Analyzer/main.cpp
        Analyzer/Core/worker.cpp
)

add_executable(Analyzer ${source})
target_link_libraries(Analyzer PRIVATE
        Threads::Threads
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
target_link_options(Analyzer PRIVATE -Wl,--disable-new-dtags)

# Include directories (if needed beyond what find_package provides)
target_include_directories(Analyzer PRIVATE
        ${OpenCV_INCLUDE_DIRS}
        ${FFMPEG_INCLUDE_DIRS}
        ${CURL_INCLUDE_DIRS}
        ${EIGEN_INCLUDE_DIR}
        ${FREETYPE_INCLUDE_DIRS}
)

target_include_directories(Analyzer PRIVATE Analyzer/Core Analyzer/Core/video_processing Analyzer/Core/tracker)

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

# Bundle the GCC C++ runtime so the binary runs on target systems that have
# an older libstdc++ (e.g., openEuler 22.04 ships with GCC 10/11 runtime
# while we compile with GCC 13 for full C++20 support: std::format,
# std::jthread, std::counting_semaphore, ranges::transform, etc.).
#
# CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES is populated automatically by CMake
# from the chosen compiler, so this works regardless of GCC version or
# the exact toolchain installation prefix.
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    foreach(_rtlib libstdc++.so.6 libgcc_s.so.1)
        foreach(_libdir ${CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES})
            set(_rtlib_path "${_libdir}/${_rtlib}")
            if(EXISTS "${_rtlib_path}")
                # Resolve the symlink so cmake --install copies the real file,
                # but install it under the plain SONAME that the binary expects.
                file(REAL_PATH "${_rtlib_path}" _rtlib_real)
                install(PROGRAMS "${_rtlib_real}" DESTINATION lib
                        RENAME "${_rtlib}")
                message(STATUS "Will bundle runtime lib: ${_rtlib_real} -> lib/${_rtlib}")
                break()
            endif()
        endforeach()
    endforeach()
endif()

# 1. Install the main executable
install(TARGETS Analyzer
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
)

# 2. Dependency Handling: Cross-Compiling vs. Native
if(CMAKE_CROSSCOMPILING)
    message(STATUS "Cross-compiling detected: Using manual dependency installation.")

    # --- A. ONNX Runtime ---
    if(ONNXRUNTIME_LIBRARY)
        # We use REAL_PATH to ensure we get the actual .so file if it's a symlink
        file(REAL_PATH "${ONNXRUNTIME_LIBRARY}" ONNX_REAL_PATH)
        get_filename_component(ONNX_SO_NAME "${ONNXRUNTIME_LIBRARY}" NAME)
        install(FILES "${ONNX_REAL_PATH}" 
                DESTINATION lib 
                RENAME "${ONNX_SO_NAME}")
    endif()

    # --- B. Miniaudio ---
    if(MINIAUDIO_LIBRARY)
        install(FILES "${MINIAUDIO_LIBRARY}" DESTINATION lib)
    endif()

    # --- C. System & PkgConfig Libraries (FFmpeg, Curl, Freetype, ALSA, etc.) ---
    # We collect the library names from the PkgConfig modules you found earlier
    set(CROSS_DEPS 
        ${FFMPEG_LIBRARIES} 
        ${CURL_LIBRARIES} 
        ${FREETYPE_LIBRARIES} 
        ${ALSA_LIBRARIES}
        event_core event_pthreads event_extra # Libevent
    )

    foreach(lib_name ${CROSS_DEPS})
        # Search for the .so in the sysroot lib folders
        find_library(LIB_PATH_${lib_name} 
                     NAMES "lib${lib_name}.so"
                     PATHS "${THIRDPARTY_LIB_DIRS}" "/usr/lib/aarch64-linux-gnu" "/lib/aarch64-linux-gnu"
                     NO_DEFAULT_PATH)
        
        if(LIB_PATH_${lib_name})
            file(REAL_PATH "${LIB_PATH_${lib_name}}" LIB_REAL)
            # Use the generic .so name so the binary can find it
            install(FILES "${LIB_REAL}" 
                    DESTINATION lib 
                    RENAME "lib${lib_name}.so")
        endif()
    endforeach()

    # --- D. OpenCV Libraries ---
    # OpenCV_LIBS usually contains full paths to the .so files
    foreach(cv_lib ${OpenCV_LIBS})
        if(EXISTS "${cv_lib}")
            get_filename_component(cv_name "${cv_lib}" NAME)
            install(FILES "${cv_lib}" DESTINATION lib)
        endif()
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
