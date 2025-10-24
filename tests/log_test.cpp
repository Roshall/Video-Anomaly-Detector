#include "../Analyzer/Core/Utils/log.h"
#include <iostream>

int main() {
    // Test different log levels
    LOGI("This is an info message");
    LOGW("This is a warning message");
    LOGD("This is a debug message");
    LOGE("This is an error message");
    
    return 0;
}