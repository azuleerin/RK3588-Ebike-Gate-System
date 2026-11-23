// src/stat.cpp —— stat.h 的实现(目前全部是头内联, 这里只留一个翻译单元锚点)
#include "nlb/stat.h"

namespace nlb {
// 全部实现都在头文件里(header-only)。这个 .cpp 的作用是让 CMake 有一个
// 稳定的翻译单元, 以及将来加非内联实现时不用改 CMakeLists。
}  // namespace nlb
