//
// Created by lg on 10/11/25.
//
#include <filesystem>

#include "constant_pool.h"
#include "yolo_detector_onnx.h"
#include "constant_yolo.h"
using avs_analyzer::YOLODetector;
using avs_analyzer::Task;
void Detector(std::shared_ptr<const YOLODetector> p, std::string path) {

      cv::Mat img = cv::imread(path);
      std::vector<DL_RESULT> res;
      res = p->Run(img);

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
  std::vector<std::string> test_img{"ireland10h.jpg", "sleep.jpg", "ireland10h.jpg", "phone.jpg"};
  for(int i =2; i < tasks.size(); ++i) {
    auto detector = avs_analyzer::GetDetector("/home/lg/CXXProjects/BXC_VideoAnalyzer_v3.51/Analyzer/models", tasks[i], false);
    Detector(detector, std::format("/home/lg/Pictures/{}", test_img[i]));
  }
  return 0;
}