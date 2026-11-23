// debug_decode.cc —— 找出两个解码器不一致的具体原因
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <arm_neon.h>

#include "license_plate_rec.h"

static inline float h2f(uint16_t h) { __fp16 v; memcpy(&v, &h, 2); return (float)v; }

int main() {
    const int T = 4, C = 64;
    std::vector<std::string> classes;
    for (int i = 0; i < C; ++i) classes.push_back(std::string(1, (char)('a' + (i % 26))));
    std::vector<uint16_t> h((size_t)T * C);
    std::vector<float> f((size_t)T * C);
    uint32_t s = 0xC0FFEEu;
    auto rnd = [&s]() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; };

    int shown = 0;
    for (int r = 0; r < 2000 && shown < 5; ++r) {
        for (size_t i = 0; i < h.size(); ++i) {
            h[i] = (uint16_t)(rnd() & 0xFFFFu);
            f[i] = h2f(h[i]);
        }
        rk::PlateRecResult a, b;
        rk::LicensePlateRec::ctc_greedy_decode(f.data(), T, C, classes, nullptr, &a);
        rk::LicensePlateRec::ctc_greedy_decode_f16(h.data(), T, C, classes, nullptr, &b);
        if (a.text == b.text && memcmp(&a.score, &b.score, sizeof(float)) == 0) continue;
        ++shown;
        fprintf(stderr, "--- 轮次 %d: 文本 '%s' vs '%s'  分数 %.9g vs %.9g\n",
                r, a.text.c_str(), b.text.c_str(), a.score, b.score);
        // 逐时间步对比
        for (int n = 0; n < T; ++n) {
            const float* row = f.data() + (size_t)n * C;
            float smax = row[0]; int sidx = 0;
            for (int c = 1; c < C; ++c) if (row[c] > smax) { smax = row[c]; sidx = c; }
            // NEON 版
            float best = -3.0e38f; int c = 0;
            for (; c + 8 <= C; c += 8) {
                const float16x8_t hh = vld1q_f16(reinterpret_cast<const __fp16*>(h.data() + (size_t)n * C + c));
                const float32x4_t lo = vcvt_f32_f16(vget_low_f16(hh));
                const float32x4_t hi = vcvt_f32_f16(vget_high_f16(hh));
                const float32x4_t mx = vmaxq_f32(vmaxq_f32(lo, hi), vdupq_n_f32(best));
                best = vmaxvq_f32(mx);
            }
            int nidx = -1;
            const float32x4_t target = vdupq_n_f32(best);
            const uint32x4_t lane_idx = {0u, 1u, 2u, 3u};
            const uint32x4_t kNoHit = vdupq_n_u32(0xFFFFFFFFu);
            c = 0;
            for (; c + 8 <= C; c += 8) {
                const float16x8_t hh = vld1q_f16(reinterpret_cast<const __fp16*>(h.data() + (size_t)n * C + c));
                const float32x4_t lo = vcvt_f32_f16(vget_low_f16(hh));
                const float32x4_t hi = vcvt_f32_f16(vget_high_f16(hh));
                const uint32_t m_lo = vminvq_u32(vbslq_u32(vceqq_f32(lo, target), lane_idx, kNoHit));
                if (m_lo != 0xFFFFFFFFu) { nidx = c + (int)m_lo; break; }
                const uint32_t m_hi = vminvq_u32(vbslq_u32(vceqq_f32(hi, target), lane_idx, kNoHit));
                if (m_hi != 0xFFFFFFFFu) { nidx = c + 4 + (int)m_hi; break; }
            }
            fprintf(stderr, "   步%d: 标量 max=%.9g idx=%d | NEON max=%.9g idx=%d | row[0]=%a (NaN=%d)\n",
                    n, smax, sidx, best, nidx, row[0], (int)std::isnan(row[0]));
        }
    }
    return 0;
}
