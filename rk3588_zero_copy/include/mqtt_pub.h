// mqtt_pub.h —— 极简 MQTT 3.1.1 发布端
//
// 为什么自己写而不引 paho-mqtt-c:
//   板子上没有 paho 开发包, 现场编译会卡在依赖上。这里只实现
//   CONNECT / CONNACK / PUBLISH(+PUBACK) 三条报文, 200 行搞定, 零依赖。
//
// 用途: 把本工程的检测结果按"臻识 ivs_result"协议发到 EMQX,
//       让现有的 edge_controller(edge_main.py)原样订阅消费。
#ifndef RK_MQTT_PUB_H_
#define RK_MQTT_PUB_H_

#include <cstdint>
#include <string>

namespace rk {

class MqttPublisher {
public:
    MqttPublisher() {}
    ~MqttPublisher() { disconnect(); }

    // 连接 broker。成功返回 true。
    bool connect(const std::string& host, int port, const std::string& client_id,
                 int timeout_ms = 5000);

    // 没连上就试一次(带超时), 已经连上直接返回 true。
    // 供后台线程做重连用: broker 晚起 / 网络抖动 / 被踢下线都能自己恢复,
    // 不用重启整个进程。
    bool ensure_connected(const std::string& host, int port, const std::string& client_id,
                          int timeout_ms = 3000);

    void disconnect();
    bool connected() const { return fd_ >= 0; }

    // 发布。qos 支持 0 和 1(1 会等 PUBACK)。
    bool publish(const std::string& topic, const std::string& payload, int qos = 0);

    const std::string& last_error() const { return err_; }

private:
    bool send_all(const void* d, size_t n);
    bool recv_exact(void* d, size_t n, int timeout_ms);
    bool handshake(const std::string& client_id, int timeout_ms);
    // 连接已经不可用时关掉 socket(让 connected() 变 false, 触发重连)
    void close_broken();

    int         fd_ = -1;
    std::string err_;
    uint16_t    pid_ = 0;
};

}  // namespace rk

#endif  // RK_MQTT_PUB_H_
