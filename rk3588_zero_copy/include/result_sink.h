// result_sink.h —— 业务层输出接口
//
// 设计原则(重要):
//   推理线程【只做入队】, 真正的业务回调(MQTT 发布 / 串口下发)在后台线程里
//   执行。绝不能把阻塞操作(TCP 连接、串口写)放进推理线程 —— 那会让 NPU 等 IO,
//   帧率直接崩掉。
//
// 现有业务链路(见 edge_main.py / edge_controller.py / led_controller.py):
//   臻识相机 --MQTT--> edge_controller.parse_payload(解析 Base64 车牌)
//                        └─> 白名单判定 ─> 门禁继电器 + C5HE 控制卡(串口: 语音/显示)
//   本工程的 C++ 可以作为【上行识别结果的生产者】, 直接替代臻识相机的角色,
//   把自己的检测/测速结果按同一套协议发出去, 现有 Python 侧无需改动。
#ifndef RK_RESULT_SINK_H_
#define RK_RESULT_SINK_H_

#include <cstdint>
#include <string>
#include <vector>

namespace rk {

// 一条检测结果(业务层的输入)
struct DetectResult {
    int    track_id = -1;
    int    cls_id = -1;
    float  score = 0.f;
    float  speed_kmh = 0.f;
    bool   speed_valid = false;
    bool   over_speed = false;
    std::string plate;      // 车牌文本; 当前模型(head/helmet)不产车牌, 为空
    int    frame_w = 0;
    int    frame_h = 0;
    int64_t ts_ms = 0;      // 单调时钟毫秒
};

// 臻识 ivs_result 上行报文的设备信息
//   Topic : device/<sn>/message/up/ivs_result   (QoS 1)
//   Payload 结构与 edge_main.py 里的示例完全一致:
//   { "id","sn","name":"ivs_result","version":"1.0","timestamp",
//     "payload": { "AlarmInfoPlate": { "channel","deviceName","ipaddr","serialno",
//                 "result": { "PlateResult": { "license","plates[]",
//                                             "is_encrypted","confidence" } } } } }
//
// ★ 编码陷阱(与 edge_controller.decode_license 对齐):
//   协议里 license 字段可能是 GBK Base64 也可能是 UTF-8 Base64,
//   接收端先试 UTF-8 再退 GBK/GB18030。这里统一发 UTF-8 Base64。
struct IvsPayloadCfg {
    std::string sn = "rk3588-01";     // 设备序列号(也用于拼 MQTT topic)
    std::string device_name = "0001";
    std::string ipaddr = "192.168.31.77";
    std::string msg_id = "rk3588-01";
    int         channel = 0;
    int         confidence = 98;
};

// 业务层回调接口
class ResultSink {
public:
    virtual ~ResultSink() {}
    // 单条结果
    virtual void on_result(const DetectResult& r) { (void)r; }
    // 一帧的全部结果 —— MQTT 上行是【一帧一条报文】, 所以主回调放在这里
    virtual void on_frame(int frame_idx, const std::vector<DetectResult>& all) {
        (void)frame_idx;
        for (const auto& r : all) on_result(r);
    }
    virtual void flush() {}
};

// ---------------------------------------------------------------- 调试输出
class StdoutSink : public ResultSink {
public:
    void on_result(const DetectResult& r) override;
};

// ---------------------------------------------------------------- MQTT 上行
// 发到 device/<sn>/message/up/ivs_result, 现有 edge_controller 可原样消费
class MqttSink : public ResultSink {
public:
    MqttSink() {}
    ~MqttSink() override;
    bool start(const std::string& host, int port, const std::string& topic,
               const std::string& client_id, int qos = 0, const IvsPayloadCfg* cfg = nullptr);
    void on_frame(int frame_idx, const std::vector<DetectResult>& all) override;
    bool connected() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

// ---------------------------------------------------------------- 串口控制卡
// 屏幕临显 + 语音播报, 协议见 led_serial.h
class LedSink : public ResultSink {
public:
    LedSink() {}
    ~LedSink() override;
    bool start(const std::string& port, int baud = 9600,
               const std::string& speak_text = "", const std::string& display_text = "",
               int duration_sec = 10);
    void on_frame(int frame_idx, const std::vector<DetectResult>& all) override;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

// ---------------------------------------------------------------- 载荷构造
// 构造一帧的 ivs_result JSON; 没有车牌时 PlateResult 里的 license 为空
std::string build_ivs_result_json(const std::vector<DetectResult>& results,
                                   const IvsPayloadCfg& cfg, int64_t ts_sec);

// Base64(标准字母表, 带 = 补齐)
std::string base64_encode(const uint8_t* data, size_t len);
std::string base64_encode(const std::string& s);

}  // namespace rk

#endif  // RK_RESULT_SINK_H_
