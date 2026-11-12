// ipm.h —— 逆透视标定与鸟瞰投影
//
// 移植自 Python 侧 speed_demo.py 的 GroundPlane / BevView，
// 目的是让板端的测速口径和 Python 基线完全一致。
//
// 原理（与 Python 版逐行对应）：
//   梯形的两组对边在图像里分别交于两个灭点 v1(沿路) / v2(横跨)。
//   这两个方向在世界里互相垂直，因此 K^-1 v1 与 K^-1 v2 正交，
//   在主点已知的前提下可以直接解出焦距 f：  f^2 = -[(v1-c)·(v2-c)]
//   有了 f 就能求出地面法线 n = d1 × d2，
//   平面坐标 (X, Y) = (p·d1, p·d2)，单位是"相机高度 c"。
//   只要再给定任意一个真实尺寸（宽度或长度），就能把 c 求出来。
#ifndef RK_IPM_H_
#define RK_IPM_H_

#include <string>

namespace rk {

struct Pt {
    double x = 0.0, y = 0.0;
};

// 地面平面解算
class GroundPlane {
public:
    // quad: A,B,C,D 四个图像点(顺序: 沿路方向 A->B, 横跨方向 A->D)
    // focal > 0 表示已知焦距; focal <= 0 时由两个灭点反解
    static bool solve(const Pt quad[4], int img_w, int img_h, double focal,
                      GroundPlane* out, std::string* err);

    // 图像点 -> 归一化地面平面坐标(X 沿路, Y 横跨, 单位=归一化)
    bool to_plane(double px, double py, double* X, double* Y) const;

    // 归一化 -> 米（scale_c = 米 / 归一化单位）
    void world_corners(double scale_c, Pt out[4]) const;

    double f = 0.0;             // 焦距(px)
    double len_norm = 0.0;      // 沿路方向归一化长度
    double width_norm = 0.0;    // 横跨方向归一化宽度
    double rect_err = 0.0;      // 矩形性误差, 0 = 完美

private:
    double M_[9] = {0};         // 图像 -> 归一化地面平面
    Pt plane_[4] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};
};

// 把随手点的 4 个点修正成路面上真正的"矩形"。
//
// 做法(移植自 speed_demo.py 的 correct_quad_to_rectangle):
//   * A->B 和 C->D 是两条平行的纵向标线 -> 交点就是"沿路方向"灭点 v1
//   * 已知焦距 f, 就能算出相机俯仰角, 进而算出"横跨路面方向"在图像里的灭点 v2
//   * 真正垂直于路面的那条线必须从 A、B 分别指向 v2; 它与另一条标线相交得到 D、C
//   * 这样得到的四边形才是世界上的矩形, 鸟瞰图才是正射的
bool correct_quad_to_rectangle(const Pt quad[4], double focal, int img_w, int img_h,
                                Pt out[4], Pt* v1_out, Pt* v2_out, std::string* err);

// 鸟瞰视图（单应变换）
class BevView {
public:
    // 用解算出的世界坐标定 dst，保证 BEV 正射（两个方向等比例）
    static bool build(const Pt quad[4], const GroundPlane& gp, double scale_c,
                      double ppm, BevView* out, std::string* err);

    // 图像点 -> BEV 坐标
    bool project(double px, double py, double* bx, double* by) const;

    // 点是否落在标定梯形内（落在外面投影出来的坐标没有意义）
    bool contains(double px, double py) const;

    double meters_per_pixel = 0.0;   // = 1/ppm
    double width_m = 0.0;            // 标定区横跨宽度(米)
    double length_m = 0.0;           // 标定区沿路长度(米)
    int    dst_w = 0, dst_h = 0;

private:
    double H_[9] = {0};
    Pt     quad_[4] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};
};

// 两点间距离
inline double pt_dist(const Pt& a, const Pt& b) {
    double dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy;
}

}  // namespace rk

#endif  // RK_IPM_H_
