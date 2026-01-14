//
// Created by lg on 10/2/25.
//
#include "test_helper.h"

#include <format>

#include "frame_recorder.h"
#include "helper.h"

int runner(Streamer& input_stream,
           const std::function<AVFrame*(AVFrame*, int&)>& func,
           bool free_frame) {

  // --- Configuration ---
  avs_analyzer::Recorder::VideoConfig config{};
  config.width = input_stream.width();
  config.height = input_stream.height();
  config.fps = 25;

  // --- Recorder Setup ---
  avs_analyzer::Recorder recorder(config);
  if (!recorder.Initialize("test.mp4", avs_analyzer::Recorder::OutputType::kFile)) {
    std::cerr << "Failed to initialize recorder_." << std::endl;
    return 1;
  }


  // --- Simulation Loop ---
  int frame_count{};

  while (!input_stream.EoF()) {
    AVFrame* frame;
    if(!(frame = input_stream.NextFrame())) {
      continue;
    }
    frame = func(frame, frame_count);
    frame->time_base = input_stream.time_base;
    frame->pict_type = AV_PICTURE_TYPE_NONE;
    recorder.AddVideoFrame(frame);
    if (free_frame) {
      av_frame_free(&frame);
    }
    // Simulate real-time delay
  }
  std::cout << "Simulation finished." << std::endl;
  return 0;
}