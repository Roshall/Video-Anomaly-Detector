#include "server_apis/api_common.h"

#include <format>
#include <string>
#include <vector>

#include <opencv2/opencv.hpp>

#include "Utils/base64.h"
#include "Utils/log.h"

void ApiSnapShot(evhttp_request* req, void*) {
  if (nlohmann::json json_req; get_request_json(req, json_req)) {
    const std::string path = json_req.value("videoPath", "");
    if (path.empty()) {
      send_json(req, HTTP_BADREQUEST, R"({"error":"missing videoPath"})");
      return;
    }
    cv::VideoCapture cap;
    if (!cap.open(path)) {
      auto msg = std::format(R"({{"error":"failed to open video","path":"{}"}})", escape_json(path));
      send_json(req, HTTP_BADREQUEST, msg);
      return;
    }

    cv::Mat frame;
    if (!cap.read(frame) || frame.empty()) {
      auto msg = std::format(R"({{"error":"failed to read first frame","path":"{}" }})", escape_json(path));
      send_json(req, HTTP_BADREQUEST, msg);
      return;
    }

    std::vector<unsigned char> jpeg;
    if (std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 90};
        !cv::imencode(".jpg", frame, jpeg, params)) {
      send_json(req, HTTP_SERVUNAVAIL, R"({"error":"JPEG encode failed"})");
      return;
    }
    int fourcc = static_cast<int>(cap.get(cv::CAP_PROP_FOURCC));
    char codec_c[5] = {0};
    codec_c[0] = static_cast<char>(fourcc & 0xFF);
    codec_c[1] = static_cast<char>((fourcc >> 8) & 0xFF);
    codec_c[2] = static_cast<char>((fourcc >> 16) & 0xFF);
    codec_c[3] = static_cast<char>((fourcc >> 24) & 0xFF);
    std::string codec = codec_c;
    if (codec.empty() || codec.find_first_not_of("\0 ") == std::string::npos) {
      codec = "unknown";
    }

    std::string resolution = std::format("{}x{}", frame.cols, frame.rows);

    const std::string b64 = base64_encode(jpeg.data(),
                                          static_cast<unsigned int>(jpeg.size()),
                                          false);

    nlohmann::json json;
    json["mime"] = "image/jpeg";
    json["data"] = b64;
    json["codec"] = codec;
    json["resolution"] = resolution;

    send_json(req, HTTP_OK, json.dump());
    LOGI("receive request for snapshot: {}. Successfully sent.", path);
  } else {
    LOGE("failed to parse json");
  }
}
