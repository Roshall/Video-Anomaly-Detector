//
// Created by lg on 10/11/25.
//
#include "track_algorithm.h"
#include "detector/inference.h"

#include <iostream>
#include <opencv2/opencv.hpp>
#include <vector>

int main() {
  // create track algorithm (min_frame_duration in milliseconds). Using 0 disables filtering
  avs_analyzer::TrackAlgorithm track_algo(0);
  YOLOv8Detect detector;
  DL_INIT_PARAM param{};
  param.modelPath = "/home/lg/CXXProjects/BXC_VideoAnalyzer_v3.51/Analyzer/models/yolov8n.onnx";
  param.target_id = 0;
  detector.CreateSession(param);
  cv::VideoWriter writer("output_video.mp4", cv::VideoWriter::fourcc('M', 'J', 'P', 'G'), 30, cv::Size(1920, 1080));

  for (int i =1; i < 601; ++i) {
    std::ostringstream oss;
    oss << std::setw(6) << std::setfill('0') << i;  // setw sets the width and setfill sets the padding character
    std::string paddedString = oss.str();
    auto img = cv::imread("/home/lg/Downloads/MOT16/train/MOT16-02/img1/" + paddedString + ".jpg");

    // Detect objects and get tracking results
    // Run detector on the frame and push result to algorithm queue.
    std::vector<DL_RESULT> res = detector.RunSession(img);
    unsigned long frame_ms = static_cast<unsigned long>(i * (1000 / 30));
    track_algo.PushResult(avs_analyzer::YOLODetector::ResultType{res, frame_ms}, frame_ms);
    avs_analyzer::Detection detection;
    if (!track_algo.TryProcessDetections(detection,
                                         std::chrono::milliseconds(100))) {
      std::cerr << "No detection result for frame " << i << "\n";
      continue;
    }
    auto& bbox = detection.bbox;
    auto& confs = detection.confidences;

    // Draw bounding boxes on the frame
    for (size_t i = 0; i < bbox.size(); ++i) {
      cv::rectangle(img, bbox[i], cv::Scalar(0, 255, 0), 2);  // Green color for bbox
      cv::putText(img, "Confidence: " + std::to_string(confs[i]), bbox[i].tl(), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(255, 255, 255), 1);
    }

    // Save the frame to output video
    cv::imwrite("/home/lg/tmp/test/" +paddedString + ".jpg", img);
    std::cout << "write to /home/lg/tmp/test/" +paddedString + ".jpg" << std::endl;
  }

  writer.release();


  return 0;
}