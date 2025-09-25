#include "Algorithm.h"

#include "Config.h"
#include "Utils/Common.h"
#include "Utils/Log.h"

namespace avs_analyzer {
Algorithm::Algorithm(Config* config) : config_(config) {}

Algorithm::~Algorithm() {}
bool Algorithm::CreateState() { return create_state_; }

}  // namespace avs_analyzer
