//
// Created by lg on 10/2/25.
//
#include "annotator.h"
#include <iostream>
#include <opencv2/opencv.hpp>

#include "test_helper.h"

int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " <RTSP_URL>" << std::endl;
    return 1;
  }
  std::string url = argv[1];
  Streamer input_stream(url);
  auto f = input_stream.NextFrame();
  avs_analyzer::VideoOverlay graph;

  std::vector<cv::Point> roiPoints{{40, 50}, {1100, 60}, {1100, 720}, {30, 700}};
  graph.init(roiPoints, f);

  std::vector<cv::Rect> bboxes{{30, 30, 50, 60}, {100, 100, 100, 100}, {230, 340, 203, 200}};


//  cv::Mat polyRGBA(input_stream.height(), input_stream.width(), CV_8UC4, cv::Scalar(0,0,0,0));

//  cv::polylines(polyRGBA, roiPoints, true, cv::Scalar(0,255,0,220), 3, cv::LINE_4);

  ;
  return runner(
      input_stream,
      [&](AVFrame* frame, int& fc) {
        static int count{0};
        bboxes[0].width += std::min(5 * count, 500);
        return graph.draw_elements(frame, bboxes, 6).release();
      },
      true);
}