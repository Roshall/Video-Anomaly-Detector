#include "analyzer.h"

#include <json/json.h>

#include <fstream>

#include "Utils/base64.h"
#include "Utils/img_processing.h"
#include "Utils/log.h"
#include "Utils/request.h"
#include "algorithm.h"
#include "config.h"
#include "control.h"
#include "scheduler.h"

namespace avs_analyzer {

Analyzer::Analyzer(Scheduler* scheduler, Monitor* monitor)
    : scheduler_(scheduler), monitor_(monitor) {}

std::vector<cv::Rect> Analyzer::ProcessVideoFrame(const cv::Mat& image) const {
  std::vector<cv::Rect> detection_results;
  Detection det;
  static int counter{0};
  if (monitor_->algorithm_code == "on_yolov8n_80") {
    if (scheduler_->yolov8n_80_classes_) {
      scheduler_->algorithm_mutex_.lock();
      det = scheduler_->yolov8n_80_classes_->DetectObjects(image);
      scheduler_->algorithm_mutex_.unlock();
    }
  } else {
    LOGE("不支持的算法：%s", monitor_->algorithm_code.data());
  }

  if (!det.bbox.empty()) {
    for (size_t i = 0; i < det.confidences.size(); ++i) {
      if (det.class_id[i] != 0) {
        continue;
      }
      // TODO: confidence threshold configurable
      if (det.confidences[i] < 0.5) {
        continue;
      }
      auto [x, y, w, h] = det.bbox[i];
      float m_x = static_cast<float >(w)/2 + static_cast<float >(x);
      auto m_y = static_cast<float >(y + h);

      auto point_to_polygon = cv::pointPolygonTest(monitor_->roi_points, {m_x, m_y}, false);

      // TODO: inside boundary threshold configuration
      if (point_to_polygon > 0) {
        detection_results.push_back(det.bbox[i]);
      }
    }
    // TODO: this should be minimum warning level
//    if (detection_results.size() < 1) {
//      detection_results.clear();
//    }
  }

  return detection_results;
}
//bool Analyzer::PostImageToServer(int64_t frameCount, cv::Mat& image,
//                                 std::vector<Detection>& detection_results,
//                                 bool& happen, float& happenScore) {
//  auto config = scheduler_->GetConfig();
//  int height = monitor_->video_height;
//  int width = monitor_->video_width;
//
//  // C++示例：指定编码参数
//  std::vector<uchar> jpg;
//  std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 90};
//  cv::imencode(".jpg", image, jpg, params);
//
//  // std::vector<int> JPEG_QUALITY = { 75 };
//  // std::vector<uchar> jpg;
//  // cv::imencode(".jpg", image, jpg, JPEG_QUALITY);
//
//  if (auto JPGBufSize = static_cast<int>(jpg.size()); JPGBufSize > 0) {
//    Base64 base64;
//    std::string imageBase64;
//    Base64::Encode(jpg.data(), JPGBufSize, imageBase64);
//
//    std::string response;
//    Json::Value param;
//    param["image_base64"] = imageBase64;
//
//    param["code"] = monitor_->code;  // 布控编号
//    param["objectCode"] = monitor_->object_code;
//    param["objects"] = monitor_->objects;
//    param["recognitionRegion"] = monitor_->roi_str;
//    param["min_interval"] = monitor_->minInterval;
//
//    std::string data = param.toStyledString();
//    // int64_t t1 = Common::getCurTimestamp();
//    Request request;
//    if (request.post(config->algorithm_api_url.data(), data.data(), response)) {
//      Json::CharReaderBuilder builder;
//      const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
//
//      Json::Value root;
//      JSONCPP_STRING errs;
//
//      if (reader->parse(response.data(),
//                        response.data() + std::strlen(response.data()), &root,
//                        &errs) &&
//          errs.empty()) {
//        if (root["code"].isInt() && root["msg"].isString()) {
//          int code = root["code"].asInt();
//          std::string msg = root["msg"].asCString();
//
//          if (1000 == code) {
//            detection_results.clear();
//            happen = false;
//            happenScore = 0.0;
//
//            Json::Value result = root["result"];
//            if (result["happen"].isBool() && result["happenScore"].isDouble()) {
//              happen = result["happen"].asBool();
//              happenScore = result["happenScore"].asFloat();
//
//              Json::Value result_detects = result["detects"];
//              for (auto i : result_detects) {
//                int x1 = i["x1"].asInt();
//                int y1 = i["y1"].asInt();
//                int x2 = i["x2"].asInt();
//                int y2 = i["y2"].asInt();
//                float class_score = i["class_score"].asFloat();
//                int class_id = i["class_id"].asInt();
//                std::string class_name = i["class_name"].asString();
//
//                Detection detect;
//                detect.x1 = x1;
//                detect.y1 = y1;
//                detect.x2 = x2;
//                detect.y2 = y2;
//                detect.class_score = class_score;
//                detect.class_id = class_id;
//                detect.class_name = class_name;
//
//                detection_results.push_back(detect);
//              }
//            }
//          } else {
//            LOGE("code=%d,msg=%s", code, msg.data());
//          }
//
//        } else {
//          LOGE("incorrect return parameter format");
//        }
//      }
//    } else {
//      detection_results.clear();
//      happen = false;
//      happenScore = 0.0;
//    }
//    // int64_t t2 = Common::getCurTimestamp();
//  }
//
//  return true;
//}

}  // namespace avs_analyzer
