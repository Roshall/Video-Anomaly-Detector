#ifndef ANALYZER_SCHEDULER_H
#define ANALYZER_SCHEDULER_H

#include <condition_variable>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <concurrentqueue/moodycamel/concurrentqueue.h>

using namespace moodycamel;

namespace AVSAnalyzer {
	class Config;
	class Worker;
	class Algorithm;
	struct Monitor;
	struct AlarmImage;
	struct Alarm;

	class Scheduler
	{
	public:
		friend class Worker;

		explicit Scheduler(Config* config);
		~Scheduler();
	public:
		Config* getConfig() const;

		bool initAlgorithm();
		std::mutex mAlgorithmMtx;
		std::unique_ptr<Algorithm>on_yolov8n_80{};
		void CleanWorkerLoop();


		void setState(bool state);
		bool getState() const;

		void addAlarm(std::unique_ptr<Alarm> alarm);

		// ApiServer 对应的函数 start
		std::vector<Monitor*> GetAllMonitors();
		Monitor* GetMonitorByCode(const std::string& code);
		void MonitorAdd(Monitor* control, int& result_code, std::string& result_msg);
		void MonitorRemove(Monitor* control, int& result_code, std::string& result_msg);
		void StartAlarmThread();
		// ApiServer 对应的函数 end

	private:
		Config* mConfig;
		bool  mState;

		// each worker monitors a streaming which
		std::unordered_map<std::string, std::unique_ptr<Worker>> mWorkerMap; // <control.code,Worker*>
		std::mutex                     mWorkerMapMtx;
		int  getWorkerSize();
		bool isAdd(Monitor* control);
		bool addWorker(Monitor* control, std::unique_ptr<Worker> worker);
		bool removeWorker(const std::string& code);//加入到待实际删除队列
		Worker* GetWorkerByCode(const std::string& code);

		ConcurrentQueue<std::unique_ptr<Worker>> mTobeDeletedWorkerQ;

		//报警处理 start
		std::unique_ptr<std::thread> mLoopAlarmThread;
		static void loopAlarmThread(void* arg);
		[[noreturn]] void PullAndGenAlarmLoop();
		ConcurrentQueue<std::unique_ptr<Alarm>> mAlarmQ;
		// void clearAlarmQueue();

		//报警处理 end

	};
}
#endif //ANALYZER_SCHEDULER_H
