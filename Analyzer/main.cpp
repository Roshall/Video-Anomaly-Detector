#include "Core/Config.h"
#include "Core/Scheduler.h"
#include "Core/Server.h"
#include "Core/Version.h" 
#include "Core/Algorithm.h"
#include "Core/Utils/Log.h"
using namespace AVSAnalyzer;

int main(int argc, char** argv)
{
	const char* file = nullptr;

	for (int i = 1; i < argc; i += 2)
	{
		if (argv[i][0] != '-')
		{
			printf("parameter error:%s\n", argv[i]);
			return -1;
		}
		switch (argv[i][1])
		{
			case 'h': {
				//打印help信息
				printf("-h 打印参数配置信息并退出\n");
				printf("-f 配置文件    如：-f config.json \n");
				system("pause\n"); 
				exit(0); 
				return -1;
			}
			case 'f': {
				file = argv[i + 1];
				break;
			}
			default: {
				printf("set parameter error:%s\n", argv[i]);
				return -1;

			}
		}
	}
	
	if (file == nullptr) {
		LOGE("failed to read config file\n");
		return -1;
	}

	Config config(file);
	if (!config.mState) {
		LOGE("failed to read config file: %s\n", file);
		return -1;
	}
	LOGI("Analyzer %s \n", PROJECT_VERSION);

	Scheduler scheduler(&config);
	if (!scheduler.initAlgorithm()) {
		return -1;
	}

	HTTPServerStart(scheduler);
	LOGI("Start Success");
	// TODO: consider to use the same thread to clean up alarms and workers
	scheduler.StartAlarmThread();
	scheduler.CleanWorkerLoop();

	return 0;
}