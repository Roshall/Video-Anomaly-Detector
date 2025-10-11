// Ultralytics 🚀 AGPL-3.0 License - https://ultralytics.com/license

#include "inference.h"

#include <regex>

#define min(a, b) (((a) < (b)) ? (a) : (b))
YOLO_V8::YOLO_V8() = default;

YOLO_V8::~YOLO_V8() {
  for(int i = 0; i < inputNodeNames.size(); ++i) {
    delete[] inputNodeNames[i];
  }
  for(int i = 0; i < outputNodeNames.size(); ++i) {
    delete[] outputNodeNames[i];
  }
};

#ifdef USE_CUDA
namespace Ort {
template <>
struct TypeToTensorType<half> {
  static constexpr ONNXTensorElementDataType type =
      ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16;
};
}  // namespace Ort
#endif

template <typename T>
char* BlobFromImage(cv::Mat& iImg, T& iBlob) {
  int channels = iImg.channels();
  int imgHeight = iImg.rows;
  int imgWidth = iImg.cols;

  for (int c = 0; c < channels; c++) {
    for (int h = 0; h < imgHeight; h++) {
      for (int w = 0; w < imgWidth; w++) {
        iBlob[c * imgWidth * imgHeight + h * imgWidth + w] =
            typename std::remove_pointer<T>::type(
                (iImg.at<cv::Vec3b>(h, w)[c]) / 255.0f);
      }
    }
  }
  return RET_OK;
}

char* YOLO_V8::PreProcess(cv::Mat& iImg, std::array<int, 2> iImgSize,
                          cv::Mat& oImg) {
  oImg = iImg.clone();
  cv::cvtColor(oImg, oImg, cv::COLOR_BGR2RGB);

  if (iImg.cols >= iImg.rows) {
    resizeScales = iImg.cols / (float)iImgSize.at(0);
    cv::resize(oImg, oImg,
               cv::Size(iImgSize.at(0), int(iImg.rows / resizeScales)));
  } else {
    resizeScales = iImg.rows / (float)iImgSize.at(0);
    cv::resize(oImg, oImg,
               cv::Size(int(iImg.cols / resizeScales), iImgSize.at(1)));
  }
  cv::Mat tempImg = cv::Mat::zeros(iImgSize.at(0), iImgSize.at(1), CV_8UC3);
  oImg.copyTo(tempImg(cv::Rect(0, 0, oImg.cols, oImg.rows)));
  oImg = tempImg;

  return RET_OK;
}

const char* YOLO_V8::CreateSession(DL_INIT_PARAM& iParams) {
  char* Ret = RET_OK;
  std::regex pattern("[\u4e00-\u9fa5]");
  bool res = std::regex_search(iParams.modelPath, pattern);
  if (res) {
    Ret =
        "[YOLO_V8]:Your model path is error.Change your model path without "
        "chinese characters.";
    std::cout << Ret << std::endl;
    return Ret;
  }
  try {
    rectConfidenceThreshold = iParams.rectConfidenceThreshold;
    iouThreshold = iParams.iouThreshold;
    imgSize = iParams.imgSize;
    modelType = iParams.modelType;
    cudaEnable = iParams.cudaEnable;
    env = Ort::Env(ORT_LOGGING_LEVEL_WARNING, "Yolo");
    Ort::SessionOptions sessionOption;
    if (iParams.cudaEnable) {
      OrtCUDAProviderOptions cudaOption;
      cudaOption.device_id = 0;
      sessionOption.AppendExecutionProvider_CUDA(cudaOption);
    }
    sessionOption.SetGraphOptimizationLevel(
        GraphOptimizationLevel::ORT_ENABLE_ALL);
    sessionOption.SetIntraOpNumThreads(iParams.intraOpNumThreads);
    sessionOption.SetLogSeverityLevel(iParams.logSeverityLevel);

#ifdef _WIN32
    int ModelPathSize = MultiByteToWideChar(
        CP_UTF8, 0, iParams.modelPath.c_str(),
        static_cast<int>(iParams.modelPath.length()), nullptr, 0);
    wchar_t* wide_cstr = new wchar_t[ModelPathSize + 1];
    MultiByteToWideChar(CP_UTF8, 0, iParams.modelPath.c_str(),
                        static_cast<int>(iParams.modelPath.length()), wide_cstr,
                        ModelPathSize);
    wide_cstr[ModelPathSize] = L'\0';
    const wchar_t* modelPath = wide_cstr;
#else
    const char* modelPath = iParams.modelPath.c_str();
#endif  // _WIN32

    session = std::make_unique<Ort::Session>(env, modelPath, sessionOption);
    Ort::AllocatorWithDefaultOptions allocator;
    size_t inputNodesNum = session->GetInputCount();
    for (size_t i = 0; i < inputNodesNum; i++) {
      Ort::AllocatedStringPtr input_node_name =
          session->GetInputNameAllocated(i, allocator);
      char* temp_buf = new char[50];
      strcpy(temp_buf, input_node_name.get());
      inputNodeNames.push_back(temp_buf);
    }
    size_t OutputNodesNum = session->GetOutputCount();
    for (size_t i = 0; i < OutputNodesNum; i++) {
      Ort::AllocatedStringPtr output_node_name =
          session->GetOutputNameAllocated(i, allocator);
      char* temp_buf = new char[10];
      strcpy(temp_buf, output_node_name.get());
      outputNodeNames.push_back(temp_buf);
    }
    options = Ort::RunOptions{nullptr};
    WarmUpSession();
    return RET_OK;
  } catch (const std::exception& e) {
    const char* str1 = "[YOLO_V8]:";
    const char* str2 = e.what();
    std::string result = std::string(str1) + std::string(str2);
    char* merged = new char[result.length() + 1];
    std::strcpy(merged, result.c_str());
    std::cout << merged << std::endl;
    delete[] merged;
    return "[YOLO_V8]:Create session failed.";
  }
}

std::vector<DL_RESULT> YOLO_V8::RunSession(cv::Mat& iImg) {
#ifdef benchmark
  clock_t starttime_1 = clock();
#endif  // benchmark

  char* Ret = RET_OK;
  cv::Mat processedImg;
  PreProcess(iImg, imgSize, processedImg);
  if (modelType < 4) {
    auto blob = std::make_unique<float []>(processedImg.total() * 3);
    auto blob_raw = blob.get();
    BlobFromImage(processedImg, blob_raw);
    std::vector<int64_t> inputNodeDims = {1, 3, imgSize.at(0), imgSize.at(1)};
    return TensorProcess(blob_raw, inputNodeDims);
  } else {
#ifdef USE_CUDA
    half* blob = new half[processedImg.total() * 3];
    BlobFromImage(processedImg, blob);
    std::vector<int64_t> inputNodeDims = {1, 3, imgSize.at(0), imgSize.at(1)};
    TensorProcess(starttime_1, iImg, blob, inputNodeDims, oResult);
#endif
  }

  return {};
}

template <typename N>
std::vector<DL_RESULT> YOLO_V8::TensorProcess(N& blob,
    std::vector<int64_t>& inputNodeDims) {
  std::vector<DL_RESULT> oResult;
  Ort::Value inputTensor =
      Ort::Value::CreateTensor<typename std::remove_pointer<N>::type>(
          Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU), blob,
          3 * imgSize.at(0) * imgSize.at(1), inputNodeDims.data(),
          inputNodeDims.size());
#ifdef benchmark
  clock_t starttime_2 = clock();
#endif  // benchmark
  auto outputTensor =
      session->Run(options, inputNodeNames.data(), &inputTensor, 1,
                   outputNodeNames.data(), outputNodeNames.size());
#ifdef benchmark
  clock_t starttime_3 = clock();
#endif  // benchmark

  Ort::TypeInfo typeInfo = outputTensor.front().GetTypeInfo();
  auto tensor_info = typeInfo.GetTensorTypeAndShapeInfo();
  std::vector<int64_t> outputNodeDims = tensor_info.GetShape();
  auto output =
      outputTensor.front()
          .GetTensorMutableData<typename std::remove_pointer<N>::type>();
  if (outputNodeDims.size() == 3 && outputNodeDims[2] == 6) {

    int batch_size = outputNodeDims[0];
    int num_detections = outputNodeDims[1];

    cv::Mat detectionResults;
    if (modelType == YOLO_DETECT_V8) {
      // FP32
      detectionResults = cv::Mat(num_detections, 6, CV_32F, output);
    } else {
      // FP16
      detectionResults = cv::Mat(num_detections, 6, CV_16F, output);
      detectionResults.convertTo(detectionResults, CV_32F);
    }

    // 直接处理已经过NMS的检测结果
    for (int i = 0; i < num_detections; i++) {
      float* detection = detectionResults.ptr<float>(i);

      float confidence = detection[4];
      int class_id = static_cast<int>(detection[5]);

      // 根据置信度阈值过滤
      if (confidence < rectConfidenceThreshold) continue;

      // 提取边界框坐标并应用缩放
      int left = static_cast<int>(detection[0] * resizeScales);
      int top = static_cast<int>(detection[1] * resizeScales);
      int right = static_cast<int>(detection[2] * resizeScales);
      int bottom = static_cast<int>(detection[3] * resizeScales);

      int width = right - left;
      int height = bottom - top;

      // 创建检测结果
      oResult.emplace_back(class_id, confidence, cv::Rect{left, top, width, height});
    }
  } else {
    int signalResultNum = outputNodeDims[1];  // 84
    int strideNum = outputNodeDims[2];        // 8400
    std::vector<int> class_ids;
    std::vector<float> confidences;
    std::vector<cv::Rect> boxes;
    cv::Mat rawData;
    if (modelType == YOLO_DETECT_V8) {
      // FP32
      rawData = cv::Mat(signalResultNum, strideNum, CV_32F, output);
    } else {
      // FP16
      rawData = cv::Mat(signalResultNum, strideNum, CV_16F, output);
      rawData.convertTo(rawData, CV_32F);
    }
    // Note:
    // ultralytics add transpose operator to the output of yolov8
    // model.which make yolov8/v5/v7 has same shape
    // https://github.com/ultralytics/assets/releases/download/v8.3.0/yolov8n.pt
    rawData = rawData.t();

    float* data = (float*)rawData.data;

    for (int i = 0; i < strideNum; ++i) {
      float* classesScores = data + 4;
      cv::Mat scores(1, this->classes.size(), CV_32FC1, classesScores);
      cv::Point class_id;
      double maxClassScore;
      cv::minMaxLoc(scores, 0, &maxClassScore, 0, &class_id);
      if (maxClassScore > rectConfidenceThreshold) {
        confidences.push_back(maxClassScore);
        class_ids.push_back(class_id.x);
        float x = data[0];
        float y = data[1];
        float w = data[2];
        float h = data[3];

        int left = int((x - 0.5 * w) * resizeScales);
        int top = int((y - 0.5 * h) * resizeScales);

        int width = int(w * resizeScales);
        int height = int(h * resizeScales);

        boxes.push_back(cv::Rect(left, top, width, height));
      }
      data += signalResultNum;
    }
    // nms is here
    std::vector<int> nmsResult;
    cv::dnn::NMSBoxes(boxes, confidences, rectConfidenceThreshold, iouThreshold,
                      nmsResult);
    for (int i = 0; i < nmsResult.size(); ++i) {
      int idx = nmsResult[i];
      DL_RESULT result;
      result.classId = class_ids[idx];
      result.confidence = confidences[idx];
      result.box = boxes[idx];
      oResult.push_back(result);
      oResult.emplace_back(class_ids[idx], confidences[idx], boxes[idx]);
    }
  }

  return oResult;
}

char* YOLO_V8::WarmUpSession() {
  cv::Mat iImg = cv::Mat(cv::Size(imgSize.at(0), imgSize.at(1)), CV_8UC3);
  cv::Mat processedImg;
  PreProcess(iImg, imgSize, processedImg);
  if (modelType < 4) {
    auto blob = std::make_unique<float []>(iImg.total() * 3);
    auto blob_raw = blob.get();
    BlobFromImage(processedImg, blob_raw);
    std::vector<int64_t> YOLO_input_node_dims = {1, 3, imgSize.at(0),
                                                 imgSize.at(1)};
    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
        Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU), blob_raw,
        3 * imgSize.at(0) * imgSize.at(1), YOLO_input_node_dims.data(),
        YOLO_input_node_dims.size());
    auto output_tensors =
        session->Run(options, inputNodeNames.data(), &input_tensor, 1,
                     outputNodeNames.data(), outputNodeNames.size());
  } else {
#ifdef USE_CUDA
    half* blob = new half[iImg.total() * 3];
    BlobFromImage(processedImg, blob);
    std::vector<int64_t> YOLO_input_node_dims = {1, 3, imgSize.at(0),
                                                 imgSize.at(1)};
    Ort::Value input_tensor = Ort::Value::CreateTensor<half>(
        Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU), blob,
        3 * imgSize.at(0) * imgSize.at(1), YOLO_input_node_dims.data(),
        YOLO_input_node_dims.size());
    auto output_tensors =
        session->Run(options, inputNodeNames.data(), &input_tensor, 1,
                     outputNodeNames.data(), outputNodeNames.size());
    delete[] blob;
    clock_t starttime_4 = clock();
    double post_process_time =
        (double)(starttime_4 - starttime_1) / CLOCKS_PER_SEC * 1000;
    if (cudaEnable) {
      std::cout << "[YOLO_V8(CUDA)]: " << "Cuda warm-up cost "
                << post_process_time << " ms. " << std::endl;
    }
#endif
  }
  return RET_OK;
}
