// mqtt_sub.h —— 极简 MQTT 3.1.1 订阅端
//
// 为什么单独写一个而不复用 mqtt_pub.h 的发布端:
//   发布端只管 CONNECT/PUBLISH, 从不【读】对端的 PUBLISH。
//   要收控制指令, 就得处理"订阅 + 收包 + 保活", 而且收包和发包会【交错】
//   (控制线程收指令, 状态线程发状态)。
//   混在一条连接上, qos=1 的 PUBACK 等待会被对端来的 PUBLISH 插队 ——
//   recv_exact(4) 拿到的是 PUBLISH 的头, 协议状态机直接乱掉。
//   所以: 控制订阅用【独立的一条连接 + 独立 client_id】, 互不干扰。
//
// 实现的报文(够用就行, 不做完整 MQTT):
//   CONNECT / CONNACK / SUBSCRIBE / SUBACK / PUBLISH / PINGREQ / PINGRESP / DISCONNECT
//
// 用途: 订阅 device/<sn>/message/down/control, 接收网页控制台下发的开关指令。
#ifndef RK_MQTT_SUB_H_
#define RK_MQTT_SUB_H_

#include <cstdint>
#include <string>
#include <vector>

namespace rk {

class MqttSubscriber {
public:
    MqttSubscriber() {}
    ~MqttSubscriber() { disconnect(); }

    // 连接 broker 并订阅。成功返回 true。
    // 失败时 last_error() 里有原因(人话)。
    bool connect(const std::string& host, int port, const std::string& client_id,
                 const std::string& topic, int qos = 0, int timeout_ms = 5000);

    void disconnect();
    bool connected() const { return fd_ >= 0; }

    // 取一条报文。
    //   返回 true  : 拿到了一条 PUBLISH, topic/payload 已填好
    //   返回 false : 超时(什么都没发生) 或 对端断开(连接已标记为断开)
    // 用 timeout_ms 控制"最多等多久", 控制线程靠它做保活和退出检查。
    bool poll(std::string* topic, std::string* payload, int timeout_ms);

    const std::string& last_error() const { return err_; }

private:
    bool send_all(const void* d, size_t n);
    bool recv_exact(void* d, size_t n, int timeout_ms);
    // 读一个完整的 MQTT 报文: 1=成功 0=超时 -1=出错
    int  read_packet(uint8_t* raw_header, std::vector<uint8_t>* body, int timeout_ms);
    bool handshake(const std::string& client_id, int timeout_ms);
    bool send_subscribe(const std::string& topic, int qos);
    bool send_pingreq();
    void close_broken();

    int         fd_ = -1;
    std::string err_;
    uint16_t    pid_ = 0;
    // 上次【有动静】的时刻(单调时钟秒) —— 用来判断是不是该发 PINGREQ 了
    double      last_io_ = 0.0;
    // 上一次 recv_exact 是不是【超时】(超时 != 出错, 连接还能继续用)
    bool        timed_out_ = false;
};

}  // namespace rk

#endif  // RK_MQTT_SUB_H_
