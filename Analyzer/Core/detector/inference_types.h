// Lightweight inference value types shared by the detector, mailbox and
// admission layers. Kept separate from inference.h so that code which only
// needs the result struct does not pull in the ONNX Runtime headers.
#ifndef ANALYZER_INFERENCE_TYPES_H
#define ANALYZER_INFERENCE_TYPES_H

#include <array>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

struct DL_INIT_PARAM {
  std::string modelPath;
  float rectConfidenceThreshold = 0.25f;
  float iouThreshold = 0.45f;
  std::array<int,2> imgSize{640, 640}; // {W, H}
  bool cudaEnable = false;             // optional, Linux CUDA EP
  int intraOpNumThreads = 0;
  int logSeverityLevel = 2;
  int target_id = 0;
};

struct DL_RESULT {
  int classId;
  float confidence;
  cv::Rect box;
};

#endif  // ANALYZER_INFERENCE_TYPES_H
