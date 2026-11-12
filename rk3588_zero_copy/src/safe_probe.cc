#include "safe_probe.h"

namespace rk {

sigjmp_buf SafeProbe::jump_;
volatile sig_atomic_t SafeProbe::in_probe_ = 0;

}  // namespace rk
