#ifndef ANALYZER_ALGORITHMONYOLO_H
#define ANALYZER_ALGORITHMONYOLO_H

#include <string>
#include <vector>
#include <mutex>
#include <queue>
#include "Algorithm.h"
#include <onnxruntime_cxx_api.h>

namespace AVSAnalyzer {
	class Config;
	class OnnxRuntimeEngine
	{
	public:
		explicit OnnxRuntimeEngine(Config* config, std::string& modelPath, std::vector<std::string>& classNames);
		~OnnxRuntimeEngine();
	public:
		bool runInference(cv::Mat& image, std::vector<DetectObject>& detects);

	private:
		Config* mConfig;
		std::vector<std::string> mClassNames;
		std::string mModelPath;
		Ort::Env mEnv{ nullptr };
		Ort::SessionOptions mSessionOptions{ nullptr };
		Ort::Session mSession{ nullptr };

	};
	class AlgorithmOnYolo : public Algorithm
	{
	public:
		AlgorithmOnYolo(Config* config, std::string& modelPath, std::vector<std::string>& classNames);
		virtual ~AlgorithmOnYolo();
	public:
		virtual bool objectDetect(cv::Mat& image, std::vector<DetectObject>& detects);
	private:
		std::vector<std::string> mClassNames;
		std::mutex mMtx;
		OnnxRuntimeEngine* mEngine;
	};
}
#endif //ANALYZER_ALGORITHMONYOLO_H

