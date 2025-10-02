
#include <functional>
#include <iostream>
#include <thread>  // For std::this_thread::sleep_for

#include "frame_recorder.h"
#include "test_helper.h"

// Function to allocate a YUV420P frame
// AVFrame* create_frame(int width, int height) {
//  AVFrame* frame = av_frame_alloc();
//  if (!frame) return nullptr;
//  frame->width = width;
//  frame->height = height;
//  frame->format = AV_PIX_FMT_YUV420P;
//  if (av_frame_get_buffer(frame, 32) < 0) {
//    av_frame_free(&frame);
//    return nullptr;
//  }
//  return frame;
//}

int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " <RTSP_URL>" << std::endl;
    return 1;
  }
  std::string url = argv[1];
  Streamer input_stream(url);
  if (!input_stream.Valid()) {
    return -1;
  }
  return runner(
      input_stream,
      [](AVFrame* frame, int& fc) {
        if (++fc > 45) {
          std::this_thread::sleep_for(std::chrono::seconds(1));
          fc = 0;
        }
        return frame;
      },
      false);
}