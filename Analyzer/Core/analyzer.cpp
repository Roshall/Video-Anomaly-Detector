#include "analyzer.h"

#include <json/json.h>

#include <fstream>

#include "algorithm.h"
#include "config.h"
#include "control.h"
#include "scheduler.h"
#include "Utils/base64.h"
#include "Utils/calcu_iou.h"
#include "Utils/log.h"
#include "Utils/request.h"

namespace avs_analyzer {

Analyzer::Analyzer(Scheduler* scheduler, Monitor* monitor)
    : scheduler_(scheduler), monitor_(monitor) {}

bool Analyzer::ProcessVideoFrame(int64_t frameCount, cv::Mat& image,
                                 std::vector<Detection>& detection_results,
                                 bool& happen, float& happenScore) {
  //        LOGI("monitor_->algorithmCode=%s",monitor_->algorithmCode.data());

  if (monitor_->algorithm_code == "api") {
    // v3.40新增，调用API类型的算法服务
    return this->PostImageToServer(frameCount, image, detection_results, happen,
                                   happenScore);
  }
  if (monitor_->algorithm_code == "on_yolov8n_80") {
    // v3.49新增
    detection_results.clear();
    happen = false;
    happenScore = 0;
    scheduler_->algorithm_mutex_.lock();
    if (scheduler_->yolov8n_80_classes_) {
      scheduler_->yolov8n_80_classes_->DetectObjects(image, detection_results);
    }
    scheduler_->algorithm_mutex_.unlock();
  } else {
    LOGE("不支持的算法：%s", monitor_->algorithm_code.data());
  }

  if (!detection_results.empty()) {
    cv::polylines(image, monitor_->ror_points,
                  monitor_->ror_points.size(),
                  cv::Scalar(0, 0, 255), 4, 8);  // 绘制多边形
    int x1, y1, x2, y2;
    int matchCount = 0;
    for (auto & detection_result : detection_results) {
      x1 = detection_result.x1;
      y1 = detection_result.y1;
      x2 = detection_result.x2;
      y2 = detection_result.y2;

      std::vector<double> object_d;
      object_d.push_back(x1);
      object_d.push_back(y1);

      object_d.push_back(x2);
      object_d.push_back(y1);

      object_d.push_back(x2);
      object_d.push_back(y2);

      object_d.push_back(x1);
      object_d.push_back(y2);

      double iou = CalcuPolygonIOU(monitor_->roi_vector, object_d);

      if (iou >= 0.5) {
        int class_id = detection_result.class_id;
        std::string class_name;
        if (class_id < monitor_->objects_v1_len) {
          class_name = monitor_->objects_v1[class_id];
        } else {
          LOGE("class error,class_id=%d,objects_v1_len=%d", class_id,
               monitor_->objects_v1_len);
        }

        detection_result.class_name = class_name;

        float class_score = detection_result.class_score;
        if (class_name == monitor_->object_code) {
          detection_result.happen = true;
          ++matchCount;
        }
      }
    }
    if (matchCount > 0) {  // 匹配数据大于0，则认为发生了报警事件
      happen = true;
      happenScore = 1.0;
    }
  }

  return true;
}
bool Analyzer::PostImageToServer(int64_t frameCount, cv::Mat& image,
                                 std::vector<Detection>& detection_results,
                                 bool& happen, float& happenScore) {
  auto config = scheduler_->GetConfig();
  int height = monitor_->video_height;
  int width = monitor_->video_width;

  // C++示例：指定编码参数
  std::vector<uchar> jpg;
  std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 90};
  cv::imencode(".jpg", image, jpg, params);

  // std::vector<int> JPEG_QUALITY = { 75 };
  // std::vector<uchar> jpg;
  // cv::imencode(".jpg", image, jpg, JPEG_QUALITY);

  if (auto JPGBufSize = static_cast<int>(jpg.size()); JPGBufSize > 0) {
    Base64 base64;
    std::string imageBase64;
    Base64::Encode(jpg.data(), JPGBufSize, imageBase64);

    std::string response;
    Json::Value param;
    param["image_base64"] = imageBase64;

    param["code"] = monitor_->code;  // 布控编号
    param["objectCode"] = monitor_->object_code;
    param["objects"] = monitor_->objects;
    param["recognitionRegion"] = monitor_->roi_str;
    param["min_interval"] = monitor_->minInterval;

    std::string data = param.toStyledString();
    // int64_t t1 = Common::getCurTimestamp();
    Request request;
    if (request.post(config->algorithm_api_url.data(), data.data(), response)) {
      Json::CharReaderBuilder builder;
      const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());

      Json::Value root;
      JSONCPP_STRING errs;

      if (reader->parse(response.data(),
                        response.data() + std::strlen(response.data()), &root,
                        &errs) &&
          errs.empty()) {
        if (root["code"].isInt() && root["msg"].isString()) {
          int code = root["code"].asInt();
          std::string msg = root["msg"].asCString();

          if (1000 == code) {
            detection_results.clear();
            happen = false;
            happenScore = 0.0;

            Json::Value result = root["result"];
            if (result["happen"].isBool() && result["happenScore"].isDouble()) {
              happen = result["happen"].asBool();
              happenScore = result["happenScore"].asFloat();

              Json::Value result_detects = result["detects"];
              for (auto i : result_detects) {
                int x1 = i["x1"].asInt();
                int y1 = i["y1"].asInt();
                int x2 = i["x2"].asInt();
                int y2 = i["y2"].asInt();
                float class_score = i["class_score"].asFloat();
                int class_id = i["class_id"].asInt();
                std::string class_name = i["class_name"].asString();

                Detection detect;
                detect.x1 = x1;
                detect.y1 = y1;
                detect.x2 = x2;
                detect.y2 = y2;
                detect.class_score = class_score;
                detect.class_id = class_id;
                detect.class_name = class_name;

                detection_results.push_back(detect);
              }
            }
          } else {
            LOGE("code=%d,msg=%s", code, msg.data());
          }

        } else {
          LOGE("incorrect return parameter format");
        }
      }
    } else {
      detection_results.clear();
      happen = false;
      happenScore = 0.0;
    }
    // int64_t t2 = Common::getCurTimestamp();
  }

  return true;
}

}  // namespace avs_analyzer
