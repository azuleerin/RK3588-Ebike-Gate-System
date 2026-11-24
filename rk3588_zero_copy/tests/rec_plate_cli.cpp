// rec_plate_cli.cpp —— 用 PP-OCRv4 rec 识别给定的车牌图(命令行工具)
//
// 用途: 换检测分辨率之后, 车牌 ROI 的像素变少了 —— 到底还能不能认出来?
//   这个工具直接对 ROI 图跑一次真实识别, 给出文本和置信度。
//
// 用法:
//   ./build/rec_plate_cli --model model/ppocrv4_rec_int8.rknn \
//                          --dict  model/ppocr_keys_v1.txt \
//                          --image roi.jpg [--image roi2.jpg ...]
//   --row top|bottom   两行车牌: 只取上半/下半再认(PP-OCR 是单行模型)
//   --charset "..."    允许的字符集(默认用项目里的车牌字符集)
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include "license_plate_rec.h"

int main(int argc, char** argv) {
    std::string model, dict;
    std::vector<std::string> images;
    std::string row = "all";

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--model") && i + 1 < argc) model = argv[++i];
        else if (!strcmp(argv[i], "--dict") && i + 1 < argc) dict = argv[++i];
        else if (!strcmp(argv[i], "--row") && i + 1 < argc) row = argv[++i];
        else if (!strcmp(argv[i], "--image") && i + 1 < argc) images.push_back(argv[++i]);
    }
    if (model.empty() || images.empty()) {
        fprintf(stderr,
                "用法: %s --model <rknn> [--dict <dict>] --image <img> [--image ...] "
                "[--row all|top|bottom]\n", argv[0]);
        return 1;
    }

    rk::PlateRecConfig cfg;
    cfg.model_path = model;
    cfg.dict_path = dict;
    cfg.allowed_charset = "0123" "456789ABCDEFGHJKLMNPQRSTUVWXYZ"
                          "京津沪渝冀豫云辽黑湘皖鲁新苏浙赣鄂桂甘晋蒙陕吉闽贵粤青藏川宁琼"
                          "港澳学警挂";
    rk::LicensePlateRec rec;
    if (!rec.init(cfg)) {
        fprintf(stderr, "模型初始化失败: %s\n", model.c_str());
        return 1;
    }
    printf("模型输入: %dx%d\n", rec.input_width(), rec.input_height());

    for (const auto& path : images) {
        cv::Mat img = cv::imread(path, cv::IMREAD_COLOR);
        if (img.empty()) {
            printf("%-46s 读图失败\n", path.c_str());
            continue;
        }
        cv::Mat roi = img;
        if (row == "top") {
            roi = img(cv::Rect(0, img.rows * 1 / 8, img.cols, img.rows * 3 / 8));
        } else if (row == "bottom") {
            roi = img(cv::Rect(0, img.rows * 5 / 8, img.cols, img.rows * 3 / 8));
        }
        rk::PlateRecResult r;
        const bool ok = rec.recognize_bgr(roi, &r);
        printf("%-46s %4dx%-4d -> %-12s score=%.4f 字符数=%d %s\n",
               path.c_str(), roi.cols, roi.rows,
               ok ? ("\"" + r.text + "\"").c_str() : "(失败)",
               r.score, r.char_count, ok ? "" : "");
    }
    return 0;
}
