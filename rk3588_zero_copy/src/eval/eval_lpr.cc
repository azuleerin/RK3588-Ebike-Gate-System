// eval_lpr.cc —— 车牌识别精度评测工具
//
// 用途: 量化回归 —— 同一批车牌 ROI, 分别用
//   ① int8 量化码喂(生产默认)
//   ② float32 喂(量化误差的"上半段": 输入侧的量化)
// 比对识别结果, 给出逐字符准确率 / 完全一致率。
//
// 用法:
//   rk_lpr_eval --model <lprnet.rknn> --images <dir> --out <out.json> \
//               [--crop x,y,w,h] [--save-rois <dir>] [--feed int8|float]
//
// 输出 JSON:
//   {"model":"...","feed":"int8","results":[{"file":"x.jpg","plate":"ABC123"}]}
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "lprnet.h"

namespace {

std::vector<std::string> list_images(const std::string& dir) {
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (struct dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() > 4 && (n.compare(n.size() - 4, 4, ".jpg") == 0 ||
                             n.compare(n.size() - 4, 4, ".png") == 0)) {
            out.push_back(dir + "/" + n);
        }
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    std::string model, images, out, save_rois;
    int crop_x = -1, crop_y = -1, crop_w = 0, crop_h = 0;
    std::string feed = "int8";
    bool auto_green = false;
    for (int i = 1; i < argc; i++) {
        std::string k = argv[i];
        auto next = [&]() { return (i + 1 < argc) ? std::string(argv[++i]) : std::string(); };
        if (k == "--model") model = next();
        else if (k == "--images") images = next();
        else if (k == "--out") out = next();
        else if (k == "--save-rois") save_rois = next();
        else if (k == "--feed") feed = next();
        else if (k == "--green-plate") auto_green = true;
        else if (k == "--crop") {
            std::string v = next();
            sscanf(v.c_str(), "%d,%d,%d,%d", &crop_x, &crop_y, &crop_w, &crop_h);
        } else { fprintf(stderr, "未知参数: %s\n", k.c_str()); return 1; }
    }
    if (model.empty() || images.empty()) {
        fprintf(stderr, "用法: rk_lpr_eval --model <rknn> --images <dir> --out <json>\n");
        return 1;
    }

    rk::LprNet lpr;
    if (!lpr.init(model, rk::lpr_default_charset())) {
        fprintf(stderr, "LPRNet 初始化失败: %s\n", model.c_str());
        return 1;
    }
    if (feed == "float") lpr.set_feed(rk::LprFeed::kFloat32);

    std::vector<std::string> files = list_images(images);
    if (files.empty()) { fprintf(stderr, "目录里没有图片\n"); return 1; }
    fprintf(stderr, "[lpr] 模型=%s feed=%s 图片=%zu 张\n", model.c_str(), feed.c_str(),
            files.size());

    FILE* outf = out.empty() ? nullptr : fopen(out.c_str(), "w");
    if (outf) fprintf(outf, "{\"model\":\"%s\",\"feed\":\"%s\",\"results\":[", model.c_str(),
                      feed.c_str());
    if (!save_rois.empty()) mkdir(save_rois.c_str(), 0755);

    int n = 0, n_ok = 0;
    double sum_ms = 0;
    for (const auto& path : files) {
        cv::Mat img = cv::imread(path);
        if (img.empty()) continue;
        // ---- 自动找绿牌(新能源绿牌是绿底, 用 HSV 阈值最省事) ----
        // 用途: 没有人工框的时候, 直接从整帧里把车牌抠出来。
        if (auto_green) {
            cv::Mat hsv;
            cv::cvtColor(img, hsv, cv::COLOR_BGR2HSV);
            cv::Mat mask;
            cv::inRange(hsv, cv::Scalar(35, 50, 40), cv::Scalar(95, 255, 255), mask);
            std::vector<std::vector<cv::Point>> contours;
            cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
            double best = 0;
            cv::Rect bestr;
            for (const auto& c : contours) {
                cv::Rect r = cv::boundingRect(c);
                const double area = static_cast<double>(r.width) * r.height;
                if (area > best && r.width > 40 && r.height > 15) { best = area; bestr = r; }
            }
            if (best <= 0) continue;          // 这帧没找到绿牌
            const int mx = bestr.width / 6, my = bestr.height / 6;   // 留点边
            cv::Rect r(bestr.x - mx, bestr.y - my,
                       bestr.width + 2 * mx, bestr.height + 2 * my);
            r &= cv::Rect(0, 0, img.cols, img.rows);
            if (r.width <= 0 || r.height <= 0) continue;
            img = img(r).clone();
        }
        if (crop_w > 0 && crop_h > 0) {
            cv::Rect r(crop_x, crop_y, crop_w, crop_h);
            r &= cv::Rect(0, 0, img.cols, img.rows);
            if (r.width <= 0 || r.height <= 0) continue;
            img = img(r).clone();
        }
        cv::Mat rgb;
        cv::cvtColor(img, rgb, cv::COLOR_BGR2RGB);
        cv::Mat roi;
        cv::resize(rgb, roi, cv::Size(rk::kLprW, rk::kLprH));

        if (!save_rois.empty()) {
            const size_t slash = path.find_last_of('/');
            const std::string fname =
                (slash == std::string::npos) ? path : path.substr(slash + 1);
            cv::imwrite(save_rois + "/" + fname, img);
        }

        std::string plate;
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = lpr.infer(roi.data, &plate);
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0).count();
        sum_ms += ms;
        n++;
        if (ok) n_ok++;
        if (outf) {
            const size_t slash = path.find_last_of('/');
            const std::string fname =
                (slash == std::string::npos) ? path : path.substr(slash + 1);
            fprintf(outf, "%s{\"file\":\"%s\",\"plate\":\"%s\"}", n > 1 ? "," : "",
                    fname.c_str(), plate.c_str());
        }
        if (n <= 8) fprintf(stderr, "  %s -> %s (%.1f ms)\n", path.c_str(), plate.c_str(), ms);
    }
    if (outf) { fprintf(outf, "]}\n"); fclose(outf); }
    fprintf(stderr, "[lpr] 完成: %d 张, 识别成功 %d, 平均 %.2f ms/张\n", n, n_ok,
            n ? sum_ms / n : 0.0);
    return 0;
}
