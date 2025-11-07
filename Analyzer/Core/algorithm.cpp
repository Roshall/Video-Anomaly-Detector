#include "algorithm.h"


namespace avs_analyzer {
Algorithm::~Algorithm() = default;
bool Algorithm::CreateState() const { return create_state_; }

}  // namespace avs_analyzer
