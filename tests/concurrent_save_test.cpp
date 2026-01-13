// Concurrent save test for JPEGEncoder
// Creates a synthetic YUV420P frame and spawns multiple threads that clone
// the frame and call saveFrame concurrently on a single shared JPEGEncoder.

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/frame.h>
}

#include <filesystem>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "jpeg_encoder.h"

int main() {
  try {
    const int width = 640;
    const int height = 360;
    // Allocate a base YUV420P frame
    AVFrame* base = av_frame_alloc();
    if (!base) {
      std::cerr << "Failed to alloc base frame" << std::endl;
      return 1;
    }
    base->format = AV_PIX_FMT_YUV420P;
    base->width = width;
    base->height = height;
    if (av_frame_get_buffer(base, 32) < 0) {
      std::cerr << "Failed to alloc buffer for base frame" << std::endl;
      av_frame_free(&base);
      return 1;
    }

    // Fill planes with a neutral gray
    for (int p = 0; p < 3; ++p) {
      int h = (p == 0) ? height : height / 2;
      int linesize = base->linesize[p];
      for (int y = 0; y < h; ++y) {
        memset(base->data[p] + y * linesize, 128, (p == 0) ? width : (width / 2));
      }
    }

    // Shared encoder instance
    auto encoder = std::make_shared<avs_analyzer::JPEGEncoder>(width, height);

    const unsigned nthreads = std::min<unsigned>(8, std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 8);

    std::filesystem::path out_dir = std::filesystem::current_path() / "Testing" / "Temporary" / "concurrent_save";
    std::filesystem::create_directories(out_dir);

    std::vector<std::thread> threads;
    threads.reserve(nthreads);

    for (unsigned i = 0; i < nthreads; ++i) {
      threads.emplace_back([i, &encoder, base, out_dir]() {
        AVFrame* f = av_frame_clone(base);
        if (!f) {
          std::cerr << "thread " << i << " failed to clone frame" << std::endl;
          return;
        }
        std::string fname = (out_dir / ("concurrent_" + std::to_string(i) + ".jpg")).string();
        auto [ret, size] = encoder->saveFrame(f, fname);
        if (ret < 0) {
          std::cerr << "thread " << i << " saveFrame failed: " << ret << std::endl;
        } else {
          std::cout << "thread " << i << " saved " << size << " bytes -> " << fname << std::endl;
          // verify size on disk if possible
          try {
            auto fsz = std::filesystem::file_size(fname);
            if (fsz != static_cast<uintmax_t>(size)) {
              std::cerr << "thread " << i << " size mismatch: on-disk=" << fsz << " reported=" << size << std::endl;
            }
          } catch (...) {
            // ignore
          }
        }
        av_frame_free(&f);
      });
    }

    for (auto &t : threads) t.join();

    // Verify files exist
    unsigned ok = 0;
    for (unsigned i = 0; i < nthreads; ++i) {
      std::string fname = (out_dir / ("concurrent_" + std::to_string(i) + ".jpg")).string();
      if (std::filesystem::exists(fname)) ++ok;
      else std::cerr << "Missing output: " << fname << std::endl;
    }

    av_frame_free(&base);

    if (ok != nthreads) {
      std::cerr << "Some threads failed to create output(" << ok << "/" << nthreads << ")" << std::endl;
      return 1;
    }

    std::cout << "All " << nthreads << " concurrent saves succeeded." << std::endl;
    return 0;
  } catch (const std::exception &ex) {
    std::cerr << "Exception: " << ex.what() << std::endl;
    return 1;
  }
}