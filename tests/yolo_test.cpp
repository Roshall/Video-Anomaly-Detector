//
// Created by lg on 10/11/25.
//
#include <filesystem>
#include <spdlog/fmt/fmt.h>

#include "constant_pool.h"
#include "detector/inference.h"
#include "constant_yolo.h"
using avs_analyzer::Task;
void Detector(std::string model_path, int target_id, std::string path) {
  YOLOv8Detect model;
  DL_INIT_PARAM param{};
  param.modelPath = model_path;
  param.target_id = target_id;
  model.CreateSession(param);

  cv::Mat img = cv::imread(path);
  std::vector<DL_RESULT> res = model.RunSession(img);

      for (auto& re : res)
      {
        cv::RNG rng(cv::getTickCount());
        cv::Scalar color(rng.uniform(0, 256), rng.uniform(0, 256), rng.uniform(0, 256));

        cv::rectangle(img, re.box, color, 3);

        float confidence = floor(100 * re.confidence) / 100;
        std::cout << std::fixed << std::setprecision(2);
        std::string label = std::to_string(re.classId) + "-||-" +
                            std::to_string(confidence).substr(0, std::to_string(confidence).size() - 4);

        cv::rectangle(
            img,
            cv::Point(re.box.x, re.box.y - 25),
            cv::Point(re.box.x + label.length() * 15, re.box.y),
            color,
            cv::FILLED
        );

        cv::putText(
            img,
            label,
            cv::Point(re.box.x, re.box.y - 5),
            cv::FONT_HERSHEY_SIMPLEX,
            0.75,
            cv::Scalar(0, 0, 0),
            2
        );


      }
      cv::imwrite(std::filesystem::path(path).filename(), img);
}

int main() {
  std::vector<Task> tasks{Task::kBreakIn, Task::kSleepWork, Task::kWallClimbing, Task::kPhonePlaying};
  std::vector<std::string> test_img{"ireland10h.jpg", "sleep.jpg", "climb.jpg", "phone.jpg"};
  std::vector<std::string> model_names{"yolov8n.onnx", "sleeping_n.onnx", "climbing_m.onnx", "phone_playing_m.onnx"};
  for(size_t i = 0; i < tasks.size(); ++i) {
    std::string model_path = fmt::format("/home/lg/CXXProjects/BXC_VideoAnalyzer_v3.51/Analyzer/models/{}", model_names[i]);
    int target_id = 0; // most models default to target 0; adjust if needed per task
    Detector(model_path, target_id, fmt::format("/home/lg/Pictures/{}", test_img[i]));
  }
  return 0;
}