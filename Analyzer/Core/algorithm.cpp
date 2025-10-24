#include "algorithm.h"

#include "config.h"
#include "Utils/common.h"
#include "Utils/log.h"

namespace avs_analyzer {
Algorithm::~Algorithm() = default;
bool Algorithm::CreateState() const { return create_state_; }

}  // namespace avs_analyzer
