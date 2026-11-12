// publish_demo.cpp —— 用【板端 C++ 真实代码】发一条 ivs_result 报文
//
// 用途: 板子暂时跑不起来时, 在 x86 容器里用同一份 src/mqtt_pub.cc + src/result_sink.cc
//       生成报文, 喂给 Web 端(vision_server/mqtt_bridge.py)。
//       —— Web 端收到的报文, 是板端代码真实发出的, 不是 Python 模拟的。
//
// 编译(容器内):
//   g++ -O2 -std=c++17 -o /tmp/pub tests/publish_demo.cpp \
//       src/result_sink.cc src/mqtt_pub.cc src/led_serial.cc -Iinclude
// 运行:
//   /tmp/pub <broker_host> <broker_port> [车牌] [速度km/h]
#include "result_sink.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    const std::string host = (argc > 1) ? argv[1] : "127.0.0.1";
    const int         port = (argc > 2) ? std::atoi(argv[2]) : 1883;
    const std::string plate = (argc > 3) ? argv[3] : "深圳HG3381";
    const float       speed  = (argc > 4) ? (float)std::atof(argv[4]) : 27.4f;

    rk::IvsPayloadCfg cfg;
    cfg.sn           = "rk3588-01";
    cfg.device_name  = "0001";
    cfg.ipaddr       = "192.168.31.77";
    cfg.msg_id       = "rk3588-01";
    cfg.channel      = 0;
    cfg.confidence   = 98;

    rk::MqttSink sink;
    const std::string topic = "device/" + cfg.sn + "/message/up/ivs_result";
    if (!sink.start(host, port, topic, cfg.sn, 1, &cfg)) {
        fprintf(stderr, "[demo] MQTT 连接失败 %s:%d\n", host.c_str(), port);
        return 1;
    }
    fprintf(stderr, "[demo] 已连接 %s:%d, topic=%s\n", host.c_str(), port, topic.c_str());

    std::vector<rk::DetectResult> rs(1);
    rs[0].track_id    = 1;
    rs[0].cls_id      = 0;
    rs[0].score       = 0.91f;
    rs[0].speed_kmh   = speed;
    rs[0].speed_valid = true;
    rs[0].over_speed  = speed > 25.0f;
    rs[0].plate       = plate;
    rs[0].frame_w     = 1920;
    rs[0].frame_h     = 1080;
    rs[0].ts_ms       = 0;

    sink.on_frame(1, rs);
    fprintf(stderr, "[demo] 报文已发送: plate=%s speed=%.1f\n", plate.c_str(), speed);

    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    return 0;
}
