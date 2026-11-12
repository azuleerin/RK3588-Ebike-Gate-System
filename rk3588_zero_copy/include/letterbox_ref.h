// letterbox_ref.h —— YOLOv8 标准预处理(Letterbox)参考实现
//
// 为什么是 Letterbox 而不是直接拉伸:
//   Ultralytics 训练/验证用的是 letterbox(本项目 detect_exp/exp02_person_ebike_gpu2
//   的 args.yaml 里 rect: false, 即方形 letterbox)。推理必须与训练一致。
//
//   关键参数(与 Ultralytics LetterBox 完全对齐):
//     · 缩放   : scale = min(640/H, 640/W)   (按【短边】对齐, 不放大超出部分)
//     · 填充色 : 114 (0x72) 灰
//     · 位置   : 居中(上下左右对称)
//     · 插值   : INTER_LINEAR
//
//   反变换(把模型输出的 640 尺度坐标换回原图坐标):
//     x_orig = (x_model - pad_x) / scale
//     y_orig = (y_model - pad_y) / scale
//
// ⚠️ 超宽画面(如 1920x540)的坑:
//   letterbox 按短边缩放, 1920x540 -> 内容只占 640x180, 上下各留 230px 灰边,
//   目标像素尺寸只有拉伸方式的 1/3。实测这类画面上 letterbox 的检测分数
//   会明显低于拉伸(见 tools/rknn/aspect_and_cam_check.py 的实测数据)。
//   所以: 模型在训练域上应当用 letterbox; 但相机域若与训练域比例差异大,
//   应当【重新训练/微调】而不是靠改预处理硬凑。
#ifndef RK_LETTERBOX_REF_H_
#define RK_LETTERBOX_REF_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <opencv2/opencv.hpp>

namespace rk {

/* Letterbox 参数: 缩放比例 + 四周填充 */
struct LetterboxParams {
    float scale = 1.f;    /* 实际缩放倍数 (内容放大/缩小) */
    int   pad_x = 0;      /* 左侧填充像素 */
    int   pad_y = 0;      /* 顶部填充像素 */
    int   net_w = 640;
    int   net_h = 640;
};

/* 等比缩放 + 居中补 114 灰边 -> 640x640
 * 入参 src 必须是 BGR(OpenCV 原生顺序), 出参 dst 是 BGR。
 * 送 NPU 前记得再 cvtColor(BGR2RGB)。
 */
inline void letterbox(const cv::Mat &src, cv::Mat &dst,
                       LetterboxParams &p, uint8_t pad_value = 114) {
    p.net_w = dst.cols;
    p.net_h = dst.rows;
    const int h = src.rows, w = src.cols;
    const float s = std::min((float)p.net_h / (float)h,
                             (float)p.net_w / (float)w);
    const int nh = (int)std::round(h * s);
    const int nw = (int)std::round(w * s);
    p.scale = s;
    p.pad_x = (p.net_w - nw) / 2;
    p.pad_y = (p.net_h - nh) / 2;

    cv::Mat resized;
    cv::resize(src, resized, cv::Size(nw, nh), 0, 0, cv::INTER_LINEAR);

    dst = cv::Mat::zeros(p.net_h, p.net_w, CV_8UC3);
    dst.setTo(cv::Scalar(pad_value, pad_value, pad_value));
    resized.copyTo(dst(cv::Rect(p.pad_x, p.pad_y, nw, nh)));
}

/* 直接拉伸(旧行为, 仅在明确知道模型是按拉伸训练/验证时用) */
inline void stretch(const cv::Mat &src, cv::Mat &dst, LetterboxParams &p) {
    p.net_w = dst.cols;
    p.net_h = dst.rows;
    p.scale = 1.f;
    p.pad_x = p.pad_y = 0;
    cv::resize(src, dst, dst.size(), 0, 0, cv::INTER_LINEAR);
}

/* 模型坐标 -> 原图坐标(letterbox) */
inline void unletterbox_box(float &x1, float &y1, float &x2, float &y2,
                             const LetterboxParams &p) {
    x1 = (x1 - (float)p.pad_x) / p.scale;
    x2 = (x2 - (float)p.pad_x) / p.scale;
    y1 = (y1 - (float)p.pad_y) / p.scale;
    y2 = (y2 - (float)p.pad_y) / p.scale;
}

/* 模型坐标 -> 原图坐标(拉伸) */
inline void unstretch_box(float &x1, float &y1, float &x2, float &y2,
                          const LetterboxParams &p, int orig_w, int orig_h) {
    const float sx = (float)orig_w / (float)p.net_w;
    const float sy = (float)orig_h / (float)p.net_h;
    x1 *= sx; x2 *= sx;
    y1 *= sy; y2 *= sy;
}

}  /* namespace rk */

#endif  /* RK_LETTERBOX_REF_H_ */
