#include "AlgorithmOnYolo.h"

#include "Config.h"
#include "Utils/Common.h"
#include "Utils/Log.h"

namespace avs_analyzer {
OnnxRuntimeEngine::OnnxRuntimeEngine(Config* config, std::string& modelPath,
                                     std::vector<std::string>& classNames)
    : config_(config), class_names_(classNames) {
  LOGI("modelPath=%s", modelPath.data());

  env_ = Ort::Env(OrtLoggingLevel::ORT_LOGGING_LEVEL_WARNING, "YOLOV8");
  session_options_ = Ort::SessionOptions();
  session_options_.SetGraphOptimizationLevel(ORT_ENABLE_BASIC);

  // std::cout << "onnxruntime inference try to use GPU Device" << std::endl;
  // OrtSessionOptionsAppendExecutionProvider_CUDA(session_options, 0);

  std::vector<std::string> providers = Ort::GetAvailableProviders();

  LOGI("supported onnxruntime providers");
  for (size_t i = 0; i < providers.size(); i++) {
    LOGI("%lld,%s", i, providers[i].data());
  }

  auto f =
      std::find(providers.begin(), providers.end(), "CUDAExecutionProvider");
  if (f != providers.end()) {
    // OrtCUDAProviderOptions cudaOption;
    // cudaOption.device_id = 0;
    // mSessionOptions.AppendExecutionProvider_CUDA(cudaOption);
  }
  session_ = Ort::Session(env_, modelPath.c_str(), session_options_);

  // 创建InferSession, 查询支持硬件设备
  // GPU Mode, 0 - gpu device id
}

OnnxRuntimeEngine::~OnnxRuntimeEngine() {
  session_options_.release();
  session_.release();
  env_.release();
}
bool OnnxRuntimeEngine::RunInference(cv::Mat& image,
                                     std::vector<Detection>& detects) {
  int image_w = image.cols;
  int image_h = image.rows;

  float score_threshold = 0.5;
  float nms_threshold = 0.5;

  std::vector<std::string> input_node_names;
  std::vector<std::string> output_node_names;
  size_t numInputNodes = session_.GetInputCount();
  size_t numOutputNodes = session_.GetOutputCount();
  Ort::AllocatorWithDefaultOptions allocator;
  input_node_names.reserve(numInputNodes);
  // 获取输入信息
  int input_w = 0;
  int input_h = 0;
  for (int i = 0; i < numInputNodes; i++) {
    auto input_name = session_.GetInputNameAllocated(i, allocator);
    input_node_names.push_back(input_name.get());
    Ort::TypeInfo input_type_info = session_.GetInputTypeInfo(i);
    auto input_tensor_info = input_type_info.GetTensorTypeAndShapeInfo();
    auto input_dims = input_tensor_info.GetShape();
    input_w = input_dims[3];
    input_h = input_dims[2];

    // std::cout << "input format: NxCxHxW = " << input_dims[0] << "x" <<
    // input_dims[1] << "x" << input_dims[2] << "x" << input_dims[3] <<
    // std::endl;
  }
  // 获取输出信息
  Ort::TypeInfo output_type_info = session_.GetOutputTypeInfo(0);
  auto output_tensor_info = output_type_info.GetTensorTypeAndShapeInfo();
  auto output_dims = output_tensor_info.GetShape();
  int output_dim = output_dims[1];  // 84
  int output_row = output_dims[2];  // 8400

  // std::cout << "output format : HxW = " << output_dims[1] << "x" <<
  // output_dims[2] << std::endl;
  for (int i = 0; i < numOutputNodes; i++) {
    auto out_name = session_.GetOutputNameAllocated(i, allocator);
    output_node_names.push_back(out_name.get());
  }
  // std::cout << "input: " << input_node_names[0] << " output: " <<
  // output_node_names[0] << std::endl;

  // 预处理
  int image_size_max = std::max(image_h, image_w);
  cv::Mat mask =
      cv::Mat::zeros(cv::Size(image_size_max, image_size_max), CV_8UC3);
  cv::Rect roi(0, 0, image_w, image_h);
  image.copyTo(mask(roi));

  // fix bug, boxes consistence!
  float x_factor = mask.cols / static_cast<float>(input_w);
  float y_factor = mask.rows / static_cast<float>(input_h);

  cv::Mat blob =
      cv::dnn::blobFromImage(mask, 1 / 255.0, cv::Size(input_w, input_h),
                             cv::Scalar(0, 0, 0), true, false);
  size_t tpixels = input_h * input_w * 3;
  std::array<int64_t, 4> input_shape_info{1, 3, input_h, input_w};

  // set input data and inference
  auto allocator_info =
      Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU);
  Ort::Value input_tensor_ = Ort::Value::CreateTensor<float>(
      allocator_info, blob.ptr<float>(), tpixels, input_shape_info.data(),
      input_shape_info.size());
  const std::array<const char*, 1> inputNames = {input_node_names[0].c_str()};
  const std::array<const char*, 1> outNames = {output_node_names[0].c_str()};

  std::vector<Ort::Value> ort_outputs =
      session_.Run(Ort::RunOptions{nullptr}, inputNames.data(), &input_tensor_,
                   1, outNames.data(), outNames.size());

  // output data
  const float* pdata = ort_outputs[0].GetTensorMutableData<float>();
  cv::Mat dout(output_dim, output_row, CV_32F, (float*)pdata);
  cv::Mat det_output = dout.t();  // 8400x84

  // post-process
  std::vector<cv::Rect> boxes;
  std::vector<int> classIds;
  std::vector<float> confidences;

  for (int i = 0; i < det_output.rows; i++) {
    cv::Mat classes_scores = det_output.row(i).colRange(4, 84);
    cv::Point classIdPoint;
    double score;
    minMaxLoc(classes_scores, 0, &score, 0, &classIdPoint);

    if (score > score_threshold) {
      float cx = det_output.at<float>(i, 0);
      float cy = det_output.at<float>(i, 1);
      float ow = det_output.at<float>(i, 2);
      float oh = det_output.at<float>(i, 3);
      int x = static_cast<int>((cx - 0.5 * ow) * x_factor);
      int y = static_cast<int>((cy - 0.5 * oh) * y_factor);
      int width = static_cast<int>(ow * x_factor);
      int height = static_cast<int>(oh * y_factor);

      cv::Rect box;
      box.x = x;
      box.y = y;
      box.width = width;
      box.height = height;

      boxes.push_back(box);
      classIds.push_back(classIdPoint.x);
      confidences.push_back(score);
    }
  }

  // NMS
  std::vector<int> indexes;
  cv::dnn::NMSBoxes(boxes, confidences, score_threshold, nms_threshold,
                    indexes);
  for (size_t i = 0; i < indexes.size(); i++) {
    int index = indexes[i];
    int class_id = classIds[index];
    float class_score = confidences[index];
    cv::Rect box = boxes[index];

    Detection detect;
    detect.x1 = box.x;
    detect.y1 = box.y;
    detect.x2 = box.x + box.width;
    detect.y2 = box.y + box.height;
    detect.class_id = class_id;
    detect.class_name = class_names_[class_id];
    detect.class_score = class_score;

    detects.push_back(detect);
  }

  return true;
}
YoloAlgorithm::YoloAlgorithm(Config* config, std::string& modelPath,
                             std::vector<std::string>& classNames)
    : Algorithm(config), class_names_(classNames) {
  engine_ = new OnnxRuntimeEngine(config, modelPath, classNames);
}

YoloAlgorithm::~YoloAlgorithm() {
  LOGI("");
  delete engine_;
  engine_ = nullptr;
}

bool YoloAlgorithm::DetectObjects(cv::Mat& image,
                                  std::vector<Detection>& detects) {
  mutex_.lock();
  bool ret = engine_->RunInference(image, detects);
  mutex_.unlock();
  return ret;
}

}  // namespace avs_analyzer
