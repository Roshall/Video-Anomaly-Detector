//
// Created by lg on 10/11/25.
//
#include "track_algorithm.h"

#include <iostream>
#include <opencv2/opencv.hpp>
#include <vector>
#include "config.h"

int main() {
  avs_analyzer::Config config{"/home/lg/CXXProjects/BXC_VideoAnalyzer_v3.51/config.json"};
  avs_analyzer::TrackAlgorithm track_algo(&config);
  cv::VideoWriter writer("output_video.mp4", cv::VideoWriter::fourcc('M', 'J', 'P', 'G'), 30, cv::Size(1920, 1080));

  for (int i =1; i < 601; ++i) {
    std::ostringstream oss;
    oss << std::setw(6) << std::setfill('0') << i;  // setw sets the width and setfill sets the padding character
    std::string paddedString = oss.str();
    auto img = cv::imread("/home/lg/Downloads/MOT16/train/MOT16-02/img1/" + paddedString + ".jpg");

    // Detect objects and get tracking results
    auto [bbox, confs] = track_algo.DetectObjects(img);

    // Draw bounding boxes on the frame
    for (size_t i = 0; i < bbox.size(); ++i) {
      cv::rectangle(img, bbox[i], cv::Scalar(0, 255, 0), 2);  // Green color for bbox
      cv::putText(img, "Confidence: " + std::to_string(confs[i]), bbox[i].tl(), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(255, 255, 255), 1);
    }

    // Save the frame to output video
    cv::imwrite("test/" +paddedString + ".jpg", img);

  }

  writer.release();


  return 0;
}