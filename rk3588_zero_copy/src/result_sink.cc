#include "result_sink.h"

#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstring>

#include "led_serial.h"
#include "mqtt_pub.h"

namespace rk {
namespace {

const char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// JSON 字符串转义(车牌/设备名里可能有引号反斜杠)
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
std::string base64_encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
        out += kB64[(v >> 18) & 0x3F];
        out += kB64[(v >> 12) & 0x3F];
        out += kB64[(v >> 6) & 0x3F];
        out += kB64[v & 0x3F];
    }
    if (i < len) {
        uint32_t v = uint32_t(data[i]) << 16;
        const bool two = (i + 1 < len);
        if (two) v |= uint32_t(data[i + 1]) << 8;
        out += kB64[(v >> 18) & 0x3F];
        out += kB64[(v >> 12) & 0x3F];
        out += two ? kB64[(v >> 6) & 0x3F] : '=';
        out += '=';
    }
    return out;
}

std::string base64_encode(const std::string& s) {
    return base64_encode(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

// ---------------------------------------------------------------------------
std::string build_ivs_result_json(const std::vector<DetectResult>& results,
                                   const IvsPayloadCfg& cfg, int64_t ts_sec) {
    int confidence = results.empty() ? 0 : cfg.confidence;
    std::string plate;
    float plate_score = 0.f;
    float plate_speed = 0.f;
    int   frame_w = 0, frame_h = 0;
    for (const auto& r : results) {
        if (frame_w == 0 && r.frame_w > 0) { frame_w = r.frame_w; frame_h = r.frame_h; }
        if (r.plate.empty()) continue;
        if (plate.empty()) {
            plate = r.plate;
            plate_score = r.score;
            plate_speed = r.speed_kmh;
        }
    }

    std::string license_b64;
    std::string plates_json;
    if (!plate.empty()) {
        license_b64 = base64_encode(plate);   // UTF-8 Base64
        plates_json = "[{\"license\":\"" + license_b64 + "\"}]";
    } else {
        plates_json = "[]";
    }

    // ---- Web 前端那一路的字段(明文, 不做 Base64) ------------------------
    //
    // PC 端的 Web 前端不直接订阅 MQTT, 它连的是后端的 WebSocket(/ws),
    // 后端把 MQTT 报文转成 {type:"plate", plate:{...}} 推给前端:
    //     plate.full_plate / plate.text -> 车牌文本
    //     plate.source                   -> 'board' 时前端显示"板端 C++"
    //     plate.score / plate.time
    // 所以这里【同时】给出明文车牌和前端认识的字段名, 后端那边把报文原样
    // 转发(或按这几个字段名映射)就能点亮前端。
    //
    // ★ 臻识协议那一段(license / plates[])保持原样 —— edge_controller 还在用,
    //   不能因为前端要明文就把 Base64 换掉。
    std::string type_field;
    std::string plate_obj;
    if (!plate.empty()) {
        type_field = "\"type\":\"plate\",";
        char tbuf[32];
        std::time_t tt = static_cast<std::time_t>(ts_sec);
        struct tm tmv;
        ::localtime_r(&tt, &tmv);
        strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tmv);

        plate_obj = "\"plate\":{\"full_plate\":\"" + json_escape(plate) +
                    "\",\"text\":\"" + json_escape(plate) +
                    "\",\"source\":\"board\",\"time\":\"" + std::string(tbuf) +
                    "\",\"score\":" + std::to_string(plate_score) +
                    ",\"speed\":" + std::to_string(plate_speed) + "},";
    }

    char buf[2048];
    snprintf(buf, sizeof(buf),
             "{\"id\":\"%s\",\"sn\":\"%s\",\"name\":\"ivs_result\",\"version\":\"1.0\","
             "\"timestamp\":%lld,%s"
             "\"plate_no\":\"%s\",\"frame_w\":%d,\"frame_h\":%d,%s"
             "\"payload\":{\"AlarmInfoPlate\":{"
             "\"channel\":%d,\"deviceName\":\"%s\",\"ipaddr\":\"%s\",\"serialno\":\"%s\","
             "\"result\":{\"PlateResult\":{"
             "\"license\":\"%s\",\"plates\":%s,"
             "\"is_encrypted\":0,\"confidence\":%d}}}}}",
             json_escape(cfg.msg_id).c_str(),
             json_escape(cfg.sn).c_str(),
             static_cast<long long>(ts_sec),
             type_field.c_str(),
             json_escape(plate).c_str(),
             frame_w, frame_h,
             plate_obj.c_str(),
             cfg.channel,
             json_escape(cfg.device_name).c_str(),
             json_escape(cfg.ipaddr).c_str(),
             json_escape(cfg.sn).c_str(),
             license_b64.c_str(),
             plates_json.c_str(),
             confidence);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
void StdoutSink::on_result(const DetectResult& r) {
    char buf[256];
    std::string extra;
    if (r.speed_valid) {
        snprintf(buf, sizeof(buf), " | %.1f km/h%s", r.speed_kmh, r.over_speed ? " 超速" : "");
        extra = buf;
    }
    if (!r.plate.empty()) {
        fprintf(stderr, "[结果] track=%d cls=%d score=%.2f plate=%s%s\n",
                r.track_id, r.cls_id, r.score, r.plate.c_str(), extra.c_str());
    } else {
        fprintf(stderr, "[结果] track=%d cls=%d score=%.2f%s\n",
                r.track_id, r.cls_id, r.score, extra.c_str());
    }
}

// ===========================================================================
// MQTT 上行
// ===========================================================================
struct MqttSink::Impl {
    MqttPublisher pub;
    std::string topic;
    int         qos = 0;
    IvsPayloadCfg cfg;

    // ---- 重连用的连接参数 ----
    std::string host;
    int         port = 1883;
    std::string client_id;

    // ---- 节流 ----
    int64_t last_try_ms = 0;      // 上次尝试(重)连的时刻
    int64_t last_err_ms = 0;      // 上次打印错误的时刻
    int64_t sent_count = 0;       // 已成功发出的报文数

    // 重连间隔: broker 不可用时最多每 5 秒试一次, 不给网络添乱
    static constexpr int64_t kRetryIntervalMs = 5000;
    // 错误提示最多每 10 秒一条, 免得刷屏
    static constexpr int64_t kErrPrintIntervalMs = 10000;
};

namespace {

int64_t epoch_ms() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

}  // namespace

MqttSink::~MqttSink() { delete impl_; }

bool MqttSink::start(const std::string& host, int port, const std::string& topic,
                     const std::string& client_id, int qos, const IvsPayloadCfg* cfg) {
    delete impl_;
    impl_ = new Impl();
    impl_->topic = topic;
    impl_->qos = qos;
    impl_->host = host;
    impl_->port = port;
    impl_->client_id = client_id;
    if (cfg) impl_->cfg = *cfg;

    // 先试一次(短超时, 别拖慢启动)。
    // ★ 连不上【不】就此放弃 MQTT —— broker 晚起 / 网络抖动 / 被人踢下线,
    //   都交给后台线程按间隔重连。视频流水线完全不受影响(它在另一个线程)。
    if (impl_->pub.ensure_connected(host, port, client_id, 2000)) {
        fprintf(stderr, "[MQTT] 连接成功 %s:%d, topic=%s (QoS=%d)\n", host.c_str(), port,
                impl_->topic.c_str(), impl_->qos);
    } else {
        fprintf(stderr, "[MQTT] 连接 %s:%d 失败: %s\n", host.c_str(), port,
                impl_->pub.last_error().c_str());
        fprintf(stderr, "[MQTT] 将每 %lld 秒自动重连, 不影响视频流水线\n",
                static_cast<long long>(Impl::kRetryIntervalMs / 1000));
    }
    // 永远返回 true: sink 一直挂着, 重连由后台线程负责
    return true;
}

bool MqttSink::connected() const { return impl_ && impl_->pub.connected(); }

void MqttSink::on_frame(int frame_idx, const std::vector<DetectResult>& all) {
    if (!impl_) return;
    const int64_t now = epoch_ms();

    // ---- 没连上 -> 按间隔重试 ----
    if (!impl_->pub.connected()) {
        if (now - impl_->last_try_ms < Impl::kRetryIntervalMs) return;
        impl_->last_try_ms = now;
        if (impl_->pub.ensure_connected(impl_->host, impl_->port, impl_->client_id, 3000)) {
            fprintf(stderr, "[MQTT] 连接成功 %s:%d (重连), topic=%s (QoS=%d)\n",
                    impl_->host.c_str(), impl_->port, impl_->topic.c_str(), impl_->qos);
            impl_->last_err_ms = 0;
        } else {
            if (now - impl_->last_err_ms >= Impl::kErrPrintIntervalMs) {
                impl_->last_err_ms = now;
                fprintf(stderr, "[MQTT] 重连失败: %s\n", impl_->pub.last_error().c_str());
            }
            return;
        }
    }

    const int64_t ts_sec = now / 1000;
    const std::string payload = build_ivs_result_json(all, impl_->cfg, ts_sec);
    if (!impl_->pub.publish(impl_->topic, payload, impl_->qos)) {
        if (now - impl_->last_err_ms >= Impl::kErrPrintIntervalMs) {
            impl_->last_err_ms = now;
            fprintf(stderr, "[MQTT] 报文发送失败(frame %d): %s\n", frame_idx,
                    impl_->pub.last_error().c_str());
        }
        return;     // 连接已经断了, 下一帧会走重连分支
    }

    // 成功: 第一条打一下, 之后每 100 条再打一次(免得刷屏)
    if (impl_->sent_count == 0 || (frame_idx % 100) == 0) {
        fprintf(stderr, "[MQTT] 报文发送成功 (frame %d, topic=%s, %zu 字节)\n", frame_idx,
                impl_->topic.c_str(), payload.size());
    }
    impl_->sent_count++;
}

// ===========================================================================
// 串口控制卡
// ===========================================================================
struct LedSink::Impl {
    LedSerial ser;
    std::string speak_text;
    std::string display_text;
    int     duration = 10;
    int64_t last_act_ms = 0;
    int64_t min_interval_ms = 1500;   // 控制卡指令最小间隔
};

LedSink::~LedSink() { delete impl_; }

bool LedSink::start(const std::string& port, int baud, const std::string& speak_text,
                    const std::string& display_text, int duration_sec) {
    delete impl_;
    impl_ = new Impl();
    impl_->speak_text = speak_text;
    impl_->display_text = display_text;
    impl_->duration = duration_sec;
    return impl_->ser.open(port, baud);
}

void LedSink::on_frame(int frame_idx, const std::vector<DetectResult>& all) {
    if (!impl_ || !impl_->ser.is_open() || all.empty()) return;

    // 控制卡对指令间隔有要求, 这里做节流, 避免刷屏
    const int64_t now_ms = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    if (impl_->last_act_ms != 0 && now_ms - impl_->last_act_ms < impl_->min_interval_ms) return;
    impl_->last_act_ms = now_ms;

    // 有车牌就显示/播报车牌, 否则用固定文案
    std::string text = impl_->display_text;
    std::string say = impl_->speak_text;
    for (const auto& r : all) {
        if (!r.plate.empty()) { text = r.plate; say = r.plate; break; }
    }
    if (text.empty()) return;
    impl_->ser.show_text(text, impl_->duration);
    if (!say.empty()) impl_->ser.speak(say);
}

}  // namespace rk
