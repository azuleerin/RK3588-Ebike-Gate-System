#pragma once
#include "opencv2/core.hpp"
namespace cv {
enum { FONT_HERSHEY_SIMPLEX = 0 };
void imshow(const std::string& winname, const Mat& mat);
int waitKey(int delay);
}
