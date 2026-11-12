#include "ipm.h"

#include <cmath>
#include <cstring>

namespace rk {
namespace {

// 两条直线 p1p2 与 p3p4 的交点；平行时返回 false
// （对应 speed_demo.py 的 _line_intersect）
bool line_intersect(const Pt& p1, const Pt& p2, const Pt& p3, const Pt& p4, Pt* out) {
    double x1 = p1.x, y1 = p1.y, x2 = p2.x, y2 = p2.y;
    double x3 = p3.x, y3 = p3.y, x4 = p4.x, y4 = p4.y;
    double a1 = y1 - y2, b1 = x2 - x1, c1 = x1 * y2 - x2 * y1;
    double a2 = y3 - y4, b2 = x4 - x3, c2 = x3 * y4 - x4 * y3;
    double det = a1 * b2 - a2 * b1;
    if (std::fabs(det) < 1e-9) return false;
    out->x = (b1 * c2 - b2 * c1) / det;
    out->y = (c1 * a2 - c2 * a1) / det;
    return true;
}

void mat_mul3(const double a[9], const double b[9], double out[9]) {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out[r * 3 + c] = a[r * 3 + 0] * b[0 * 3 + c] +
                             a[r * 3 + 1] * b[1 * 3 + c] +
                             a[r * 3 + 2] * b[2 * 3 + c];
        }
    }
}

void mat_vec3(const double m[9], const double v[3], double out[3]) {
    for (int r = 0; r < 3; ++r) {
        out[r] = m[r * 3 + 0] * v[0] + m[r * 3 + 1] * v[1] + m[r * 3 + 2] * v[2];
    }
}

// 便捷重载: 直接用齐次坐标 (x, y, 1)
void mat_vec3_h(const double m[9], double x, double y, double out[3]) {
    const double v[3] = {x, y, 1.0};
    mat_vec3(m, v, out);
}

void cross3(const double a[3], const double b[3], double out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

double norm3(const double v[3]) {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

// 解 8x8 线性方程组求单应矩阵 (src -> dst)，h33 = 1
// 与 cv2.getPerspectiveTransform 等价（同样是最小二乘意义下的精确解）
bool solve_homography(const Pt src[4], const Pt dst[4], double H[9]) {
    double A[8][9];
    memset(A, 0, sizeof(A));
    for (int i = 0; i < 4; ++i) {
        double x = src[i].x, y = src[i].y, X = dst[i].x, Y = dst[i].y;
        A[2 * i + 0][0] = x;  A[2 * i + 0][1] = y;  A[2 * i + 0][2] = 1.0;
        A[2 * i + 0][6] = -x * X; A[2 * i + 0][7] = -y * X; A[2 * i + 0][8] = X;
        A[2 * i + 1][3] = x;  A[2 * i + 1][4] = y;  A[2 * i + 1][5] = 1.0;
        A[2 * i + 1][6] = -x * Y; A[2 * i + 1][7] = -y * Y; A[2 * i + 1][8] = Y;
    }
    // 高斯消元(列主元)
    for (int col = 0; col < 8; ++col) {
        int piv = col;
        for (int r = col + 1; r < 8; ++r)
            if (std::fabs(A[r][col]) > std::fabs(A[piv][col])) piv = r;
        if (std::fabs(A[piv][col]) < 1e-12) return false;
        if (piv != col)
            for (int c = 0; c < 9; ++c) { double t = A[col][c]; A[col][c] = A[piv][c]; A[piv][c] = t; }
        for (int r = 0; r < 8; ++r) {
            if (r == col) continue;
            double f = A[r][col] / A[col][col];
            if (f == 0.0) continue;
            for (int c = col; c < 9; ++c) A[r][c] -= f * A[col][c];
        }
    }
    double h[8];
    for (int i = 0; i < 8; ++i) h[i] = A[i][8] / A[i][i];
    H[0] = h[0]; H[1] = h[1]; H[2] = h[2];
    H[3] = h[3]; H[4] = h[4]; H[5] = h[5];
    H[6] = h[6]; H[7] = h[7]; H[8] = 1.0;
    return true;
}

// 点是否在凸四边形内（叉积符号一致）
bool point_in_quad(const Pt q[4], double x, double y) {
    int sign = 0;
    for (int i = 0; i < 4; ++i) {
        const Pt& a = q[i];
        const Pt& b = q[(i + 1) % 4];
        double cr = (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
        if (std::fabs(cr) < 1e-12) continue;
        int s = (cr > 0.0) ? 1 : -1;
        if (sign == 0) sign = s;
        else if (s != sign) return false;
    }
    return true;
}

// 直线 (a, b, c): a*x + b*y + c = 0
struct Line { double a = 0, b = 0, c = 0; };

Line line_from_points(const Pt& p, const Pt& q) {
    Line l;
    l.a = p.y - q.y;
    l.b = q.x - p.x;
    l.c = p.x * q.y - q.x * p.y;
    return l;
}

bool line_line_intersect(const Line& l1, const Line& l2, Pt* out) {
    double det = l1.a * l2.b - l2.a * l1.b;
    if (std::fabs(det) < 1e-9) return false;
    out->x = (l1.b * l2.c - l2.b * l1.c) / det;
    out->y = (l1.c * l2.a - l2.c * l1.a) / det;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
bool correct_quad_to_rectangle(const Pt quad[4], double focal, int img_w, int img_h,
                                Pt out[4], Pt* v1_out, Pt* v2_out, std::string* err) {
    const double cx = img_w / 2.0;
    const double cy = img_h / 2.0;

    // A->B 和 C->D 是两条平行的纵向标线, 交点就是"沿路方向"灭点 v1
    Line l_ab = line_from_points(quad[0], quad[1]);
    Line l_cd = line_from_points(quad[2], quad[3]);
    Pt v1;
    if (!line_line_intersect(l_ab, l_cd, &v1)) {
        if (err) *err = "两条纵向标线互相平行, 无法求沿路灭点（重新选点）";
        return false;
    }

    double Ki[9] = {1.0 / focal, 0.0, -cx / focal,
                     0.0, 1.0 / focal, -cy / focal,
                     0.0, 0.0, 1.0};
    double d1[3], tmp[3];
    mat_vec3_h(Ki, v1.x, v1.y, tmp);
    double nn = norm3(tmp);
    for (int i = 0; i < 3; ++i) d1[i] = tmp[i] / nn;

    // 沿路方向是水平的世界方向 -> 相机俯仰角 tan(theta) = d1_y / d1_z
    const double tan_theta = (std::fabs(d1[2]) > 1e-9) ? (d1[1] / d1[2]) : 1e9;
    const double theta = std::atan(tan_theta);
    double n[3] = {0.0, -std::cos(theta), std::sin(theta)};

    double d2[3];
    cross3(d1, n, d2);
    double nd = norm3(d2);
    if (nd < 1e-9) {
        if (err) *err = "无法确定横跨路面方向";
        return false;
    }
    for (int i = 0; i < 3; ++i) d2[i] /= nd;

    // v2 = K * d2
    double v2x = focal * d2[0] + cx * d2[2];
    double v2y = focal * d2[1] + cy * d2[2];
    double v2z = d2[2];
    if (std::fabs(v2z) < 1e-12) {
        if (err) *err = "横跨方向灭点在无穷远";
        return false;
    }
    Pt v2{v2x / v2z, v2y / v2z};

    // D、C: 另一条标线分别与"过 A / 过 B 且指向 v2"的直线相交
    Pt D, C;
    if (!line_line_intersect(l_cd, line_from_points(quad[0], v2), &D) ||
        !line_line_intersect(l_cd, line_from_points(quad[1], v2), &C)) {
        if (err) *err = "求横跨方向交点失败（重新选点）";
        return false;
    }

    out[0] = quad[0];
    out[1] = quad[1];
    out[2] = C;
    out[3] = D;
    if (v1_out) *v1_out = v1;
    if (v2_out) *v2_out = v2;
    return true;
}

// ---------------------------------------------------------------------------
bool GroundPlane::solve(const Pt quad[4], int img_w, int img_h, double focal,
                         GroundPlane* out, std::string* err) {
    const double cx = img_w / 2.0;
    const double cy = img_h / 2.0;

    Pt v1, v2;
    if (!line_intersect(quad[0], quad[1], quad[2], quad[3], &v1) ||
        !line_intersect(quad[1], quad[2], quad[3], quad[0], &v2)) {
        if (err) *err = "标定梯形的对边互相平行，无法做逆透视（换个视频或重新选点）";
        return false;
    }

    double f = focal;
    if (f <= 0.0) {
        // 两个灭点在主点坐标系下应当张成钝角
        double d = (v1.x - cx) * (v2.x - cx) + (v1.y - cy) * (v2.y - cy);
        if (d >= 0.0) {
            if (err) *err = "两个灭点解不出正的焦距，说明选的四点不构成路面上的矩形";
            return false;
        }
        f = std::sqrt(-d);
    }

    // K^-1
    double Ki[9] = {1.0 / f, 0.0, -cx / f,
                     0.0, 1.0 / f, -cy / f,
                     0.0, 0.0, 1.0};

    double d1[3], d2[3], n[3], tmp[3];
    mat_vec3_h(Ki, v1.x, v1.y, tmp);
    double n1 = norm3(tmp);
    for (int i = 0; i < 3; ++i) d1[i] = tmp[i] / n1;
    mat_vec3_h(Ki, v2.x, v2.y, tmp);
    double n2 = norm3(tmp);
    for (int i = 0; i < 3; ++i) d2[i] = tmp[i] / n2;
    cross3(d1, d2, n);

    // 图像 -> 归一化地面平面坐标
    // 射线方向要用归一化的 K^-1 p，而不是像素 p 本身，否则尺度会差一个 f
    double M[9];
    auto rebuild = [&](const double dd1[3], const double dd2[3]) {
        double nn[3];
        cross3(dd1, dd2, nn);
        double rows[9] = {dd1[0], dd1[1], dd1[2],
                          dd2[0], dd2[1], dd2[2],
                          nn[0],  nn[1],  nn[2]};
        mat_mul3(rows, Ki, M);
        for (int i = 0; i < 4; ++i) {
            double p[3] = {quad[i].x, quad[i].y, 1.0};
            double q[3];
            mat_vec3_h(M, p[0], p[1], q);
            out->plane_[i].x = q[0] / q[2];
            out->plane_[i].y = q[1] / q[2];
        }
    };

    rebuild(d1, d2);
    // 统一方向约定：X 沿 A->B 增大, Y 沿 A->D 增大
    // 翻转 d2 会让 X 变号(Y 不变); 翻转 d1 会让 Y 变号(X 不变)
    if (out->plane_[1].x < out->plane_[0].x) {
        for (int i = 0; i < 3; ++i) d2[i] = -d2[i];
        rebuild(d1, d2);
    }
    if (out->plane_[3].y < out->plane_[0].y) {
        for (int i = 0; i < 3; ++i) d1[i] = -d1[i];
        rebuild(d1, d2);
    }

    memcpy(out->M_, M, sizeof(M));
    out->f = f;

    double xs[4], ys[4];
    for (int i = 0; i < 4; ++i) { xs[i] = out->plane_[i].x; ys[i] = out->plane_[i].y; }
    double minx = xs[0], maxx = xs[0], miny = ys[0], maxy = ys[0];
    for (int i = 1; i < 4; ++i) {
        minx = std::min(minx, xs[i]); maxx = std::max(maxx, xs[i]);
        miny = std::min(miny, ys[i]); maxy = std::max(maxy, ys[i]);
    }
    out->len_norm = std::fabs(maxx - minx);     // 沿路方向
    out->width_norm = std::fabs(maxy - miny);   // 横跨方向
    if (out->len_norm < 1e-9 || out->width_norm < 1e-9) {
        if (err) *err = "标定梯形退化，重新选点";
        return false;
    }

    // 矩形性检查：世界矩形的投影里，对边在平面坐标下应该严格平行
    double e[4];
    e[0] = std::fabs(out->plane_[0].y - out->plane_[1].y) / std::max(out->width_norm, 1e-9);
    e[1] = std::fabs(out->plane_[3].y - out->plane_[2].y) / std::max(out->width_norm, 1e-9);
    e[2] = std::fabs(out->plane_[0].x - out->plane_[3].x) / std::max(out->len_norm, 1e-9);
    e[3] = std::fabs(out->plane_[1].x - out->plane_[2].x) / std::max(out->len_norm, 1e-9);
    out->rect_err = std::max(std::max(e[0], e[1]), std::max(e[2], e[3]));
    return true;
}

bool GroundPlane::to_plane(double px, double py, double* X, double* Y) const {
    double p[3] = {px, py, 1.0};
    double q[3];
    mat_vec3(M_, p, q);
    if (std::fabs(q[2]) < 1e-12) return false;
    *X = q[0] / q[2];
    *Y = q[1] / q[2];
    return true;
}

void GroundPlane::world_corners(double scale_c, Pt out[4]) const {
    for (int i = 0; i < 4; ++i) {
        out[i].x = plane_[i].x * scale_c;
        out[i].y = plane_[i].y * scale_c;
    }
}

// ---------------------------------------------------------------------------
bool BevView::build(const Pt quad[4], const GroundPlane& gp, double scale_c,
                    double ppm, BevView* out, std::string* err) {
    Pt wc[4];
    gp.world_corners(scale_c, wc);

    double along_px = (wc[1].x - wc[0].x) * ppm;    // 沿路方向长度(像素)
    double across_px = (wc[3].y - wc[0].y) * ppm;   // 横跨方向宽度(像素)

    Pt dst[4] = {{0.0, 0.0},
                  {0.0, along_px},
                  {across_px, along_px},
                  {across_px, 0.0}};

    double H[9];
    if (!solve_homography(quad, dst, H)) {
        if (err) *err = "单应矩阵求解失败，标定点退化";
        return false;
    }
    memcpy(out->H_, H, sizeof(H));
    memcpy(out->quad_, quad, sizeof(out->quad_));

    out->dst_w = std::max(2, static_cast<int>(std::lround(std::fabs(across_px))));
    out->dst_h = std::max(2, static_cast<int>(std::lround(std::fabs(along_px))));
    out->meters_per_pixel = 1.0 / ppm;
    out->width_m = std::fabs(wc[3].y - wc[0].y);
    out->length_m = std::fabs(wc[1].x - wc[0].x);
    return true;
}

bool BevView::project(double px, double py, double* bx, double* by) const {
    double X = H_[0] * px + H_[1] * py + H_[2];
    double Y = H_[3] * px + H_[4] * py + H_[5];
    double W = H_[6] * px + H_[7] * py + H_[8];
    if (std::fabs(W) < 1e-12) return false;
    *bx = X / W;
    *by = Y / W;
    return true;
}

bool BevView::contains(double px, double py) const {
    return point_in_quad(quad_, px, py);
}

}  // namespace rk
