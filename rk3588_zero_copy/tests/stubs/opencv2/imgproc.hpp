#pragma once
#include "opencv2/core.hpp"
namespace cv {
void rectangle(Mat& img, Rect rec, const Scalar& color, int thickness = 1);
void putText(Mat& img, const std::string& text, Point org, int fontFace, double fontScale,
             const Scalar& color, int thickness = 1);
}
