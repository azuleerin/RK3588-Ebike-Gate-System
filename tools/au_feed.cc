// tools/au_feed.cc —— 绕过 FFmpeg, 把访问单元直接喂给 MPP
//
// 用途(step/05 引爆测试的诊断):
//   MPP 在实时 RTSP 下能解码, 但任何文件形式(裸 .h264 / MP4 / TS)都解不出帧。
//   本工具验证最后一个假设: MPP 是否要求【整个访问单元在一个包里】。
//   文件形式必然要把 113KB 的 IDR 访问单元拆开(PES 长度上限 64KB),
//   而实时 RTSP 是整包送达的。
//
// 用法: au_feed <in.aus> [最大AU数]
//   in.aus: es_dump --aus 产出的"4字节长度 + 访问单元"序列
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "mpp_decoder.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "用法: %s <in.aus> [最大AU数]\n", argv[0]);
        return 2;
    }
    const int max_au = (argc > 2) ? atoi(argv[2]) : 100000;

    FILE* f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "[au_feed] 打不开 %s\n", argv[1]);
        return 1;
    }
    std::vector<std::string> aus;
    while (true) {
        uint32_t len = 0;
        if (fread(&len, 1, 4, f) != 4) break;
        std::string buf(len, '\0');
        if (len && fread(&buf[0], 1, len, f) != len) break;
        aus.push_back(std::move(buf));
        if ((int)aus.size() >= max_au) break;
    }
    fclose(f);
    fprintf(stderr, "[au_feed] 读入 %zu 个访问单元\n", aus.size());
    if (aus.empty()) return 1;

    rk::MppDecoder dec;
    if (!dec.init(false, 0, 0)) {
        fprintf(stderr, "[au_feed] MppDecoder 初始化失败: %s\n", dec.last_error());
        return 1;
    }

    int frames = 0;
    int64_t pts = 0;
    for (size_t i = 0; i < aus.size(); ++i) {
        rk::Nv12Frame nf;
        if (dec.decode((const uint8_t*)aus[i].data(), aus[i].size(), pts, &nf)) {
            ++frames;
            if (frames <= 5 || frames % 50 == 0) {
                fprintf(stderr,
                        "[au_feed] 出帧 #%d  au=%zu  %dx%d stride=%dx%d fd=%d\n",
                        frames, aus[i].size(), nf.width, nf.height, nf.wstride,
                        nf.hstride, nf.fd);
            }
        }
        pts += 40;
    }
    fprintf(stderr, "[au_feed] 完成: %zu 个 AU -> %d 帧\n", aus.size(), frames);
    return frames > 0 ? 0 : 1;
}
