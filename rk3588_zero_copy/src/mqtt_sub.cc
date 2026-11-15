#include "mqtt_sub.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>
#include <vector>

namespace rk {
namespace {

// MQTT 剩余长度变长整数
bool encode_remaining_length(std::vector<uint8_t>& out, size_t len) {
    do {
        uint8_t b = len % 128;
        len /= 128;
        if (len > 0) b |= 0x80;
        out.push_back(b);
    } while (len > 0);
    return true;
}

void put_u16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x & 0xFF));
}

// socket 的收发超时(毫秒)。不设的话, 对端半死不活就能把线程永久卡死。
const int kIoTimeoutMs = 2000;
// keepalive: CONNECT 里报 60 秒, 这里按一半(30 秒)发一次 PINGREQ
const int kKeepAliveSec = 60;
// 单条报文的最大长度(防御用: 字节流错位时长度字段可能是垃圾)
const size_t kMaxPayload = 1u << 20;   // 1 MB

double now_sec() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + ts.tv_nsec * 1e-9;
}

}  // namespace

void MqttSubscriber::close_broken() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool MqttSubscriber::send_all(const void* d, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(d);
    size_t left = n;
    int stalls = 0;
    while (left > 0) {
        ssize_t w = ::send(fd_, p, left, MSG_NOSIGNAL);
        if (w > 0) {
            p += w;
            left -= static_cast<size_t>(w);
            stalls = 0;
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (++stalls > 2) {
                err_ = "发送超时(对端不收数据)";
                close_broken();
                return false;
            }
            continue;
        }
        err_ = std::string("send 失败: ") + strerror(errno);
        close_broken();
        return false;
    }
    last_io_ = now_sec();
    return true;
}

bool MqttSubscriber::recv_exact(void* d, size_t n, int timeout_ms) {
    uint8_t* p = static_cast<uint8_t*>(d);
    size_t left = n;
    timed_out_ = false;
    while (left > 0) {
        struct pollfd pfd = {fd_, POLLIN, 0};
        // ★ 第一个字节用调用方给的超时(用来实现"最多等多久"),
        //   之后的字节给足时间(socket 自身还有 SO_RCVTIMEO 兜底)。
        //
        //   为什么不能一路都用短超时: 读了一半超时就退出去, 字节流就【错位】了 ——
        //   下次再读会从包【中间】开始, 于是把 payload 长度解析成一个巨大的数,
        //   body.resize() 直接抛 std::bad_alloc 把整个进程带走(踩过, 2026-11-15)。
        //   所以: 短超时只用来判断"有没有东西可读", 一旦开始读就必须读完。
        const int tmo = (left == n) ? timeout_ms : kIoTimeoutMs;
        int pr = ::poll(&pfd, 1, tmo);
        if (pr <= 0) {
            if (pr == 0) {
                err_ = "接收超时";
                timed_out_ = true;
            } else {
                err_ = std::string("poll 失败: ") + strerror(errno);
            }
            return false;   // 超时【不】关连接 —— 由调用方决定要不要放弃
        }
        ssize_t r = ::recv(fd_, p, left, 0);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            err_ = (r == 0) ? "连接被对端关闭" : std::string("recv 失败: ") + strerror(errno);
            close_broken();
            return false;
        }
        p += r;
        left -= static_cast<size_t>(r);
    }
    last_io_ = now_sec();
    return true;
}

bool MqttSubscriber::handshake(const std::string& client_id, int timeout_ms) {
    // CONNECT: 0x10 | len | "MQTT" | 0x04 | flags(clean=0x02) | keepalive | clientid
    std::vector<uint8_t> pkt;
    pkt.push_back(0x10);

    std::vector<uint8_t> var;
    var.push_back(0x00); var.push_back(0x04);
    var.push_back('M'); var.push_back('Q'); var.push_back('T'); var.push_back('T');
    var.push_back(0x04);          // protocol level 3.1.1
    var.push_back(0x02);          // clean session
    put_u16(var, static_cast<uint16_t>(kKeepAliveSec));
    put_u16(var, static_cast<uint16_t>(client_id.size()));
    var.insert(var.end(), client_id.begin(), client_id.end());

    encode_remaining_length(pkt, var.size());
    pkt.insert(pkt.end(), var.begin(), var.end());

    if (!send_all(pkt.data(), pkt.size())) return false;

    // CONNACK: 0x20 0x02 <session present> <return code>
    uint8_t head[4];
    if (!recv_exact(head, 4, timeout_ms)) return false;
    if ((head[0] & 0xF0) != 0x20) {
        err_ = "CONNACK 报文类型不对";
        return false;
    }
    if (head[3] != 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "CONNACK 返回码 %d(非 0 表示被拒绝)", head[3]);
        err_ = buf;
        return false;
    }
    return true;
}

bool MqttSubscriber::send_subscribe(const std::string& topic, int qos) {
    if (qos < 0) qos = 0;
    if (qos > 1) qos = 1;

    std::vector<uint8_t> pkt;
    pkt.push_back(0x82);           // SUBSCRIBE(固定标志 0x02)

    std::vector<uint8_t> var;
    const uint16_t pid = ++pid_;
    put_u16(var, pid);
    put_u16(var, static_cast<uint16_t>(topic.size()));
    var.insert(var.end(), topic.begin(), topic.end());
    var.push_back(static_cast<uint8_t>(qos));

    encode_remaining_length(pkt, var.size());
    pkt.insert(pkt.end(), var.begin(), var.end());

    if (!send_all(pkt.data(), pkt.size())) return false;

    // ---- 收 SUBACK ----
    //
    // ★ 用【通用的读报文】而不是"硬读 5 个字节":
    //   SUBACK 的长度是可变的(有的 broker 会在后面带原因字符串), 硬读 5 个字节
    //   就会剩下半个报文在流里 —— 下次读从包【中间】开始, 字节流直接错位,
    //   于是控制指令永远收不到(2026-11-15 踩的就是这个)。
    uint8_t type = 0;
    std::vector<uint8_t> body;
    const int pr = read_packet(&type, &body, 3000);
    if (pr <= 0) {
        if (err_.empty()) err_ = "收 SUBACK 失败";
        return false;
    }
    if ((type & 0xF0) != 0x90) {
        err_ = "SUBACK 报文类型不对";
        return false;
    }
    // body: <packet id 2字节> <return code>
    if (body.size() < 3) {
        err_ = "SUBACK 报文太短";
        return false;
    }
    if (body[2] == 0x80) {
        err_ = "订阅被拒绝(返回码 0x80)";
        return false;
    }
    return true;
}

bool MqttSubscriber::send_pingreq() {
    // PINGREQ 发出去就行, 【不要】就地等 PINGRESP。
    //
    // ★ 为什么不等:
    //   等 PINGRESP 的时候, broker 很可能正好送来一条 PUBLISH(控制指令),
    //   那 2 个字节就不是 PINGRESP 了 —— 同步等待会把协议状态机搅乱。
    //   正确的做法是: 发出去, 然后在读报文的那条路上把 PINGRESP 当"别的报文"吃掉。
    const uint8_t ping[2] = {0xC0, 0x00};
    return send_all(ping, 2);
}

bool MqttSubscriber::connect(const std::string& host, int port,
                              const std::string& client_id,
                              const std::string& topic, int qos, int timeout_ms) {
    disconnect();

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char portbuf[16];
    snprintf(portbuf, sizeof(portbuf), "%d", port);
    struct addrinfo* res = nullptr;
    int gai = ::getaddrinfo(host.c_str(), portbuf, &hints, &res);
    if (gai != 0 || !res) {
        err_ = std::string("解析地址失败: ") + gai_strerror(gai);
        return false;
    }

    for (struct addrinfo* ai = res; ai; ai = ai->ai_next) {
        int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;

        int fl = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS) {
            struct pollfd pfd = {fd, POLLOUT, 0};
            if (::poll(&pfd, 1, timeout_ms) <= 0) {
                ::close(fd);
                continue;
            }
            int soerr = 0;
            socklen_t sl = sizeof(soerr);
            ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
            if (soerr != 0) {
                ::close(fd);
                continue;
            }
        } else if (rc != 0) {
            ::close(fd);
            continue;
        }
        ::fcntl(fd, F_SETFL, fl);      // 恢复阻塞
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        struct timeval tv;
        tv.tv_sec = kIoTimeoutMs / 1000;
        tv.tv_usec = (kIoTimeoutMs % 1000) * 1000;
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        fd_ = fd;
        break;
    }
    ::freeaddrinfo(res);

    if (fd_ < 0) {
        err_ = "连接 " + host + " 失败";
        return false;
    }
    last_io_ = now_sec();
    if (!handshake(client_id, timeout_ms)) {
        close_broken();
        return false;
    }
    if (!send_subscribe(topic, qos)) {
        close_broken();
        return false;
    }
    err_.clear();
    return true;
}

void MqttSubscriber::disconnect() {
    if (fd_ >= 0) {
        const uint8_t bye[2] = {0xE0, 0x00};   // DISCONNECT
        ::send(fd_, bye, 2, MSG_NOSIGNAL);
        ::close(fd_);
        fd_ = -1;
    }
}

// ---------------------------------------------------------------------------
// 读一个完整的 MQTT 报文
//
// 返回: 1 = 成功, 0 = 超时(没东西可读, 连接还在), -1 = 出错(连接已断)
// ---------------------------------------------------------------------------
int MqttSubscriber::read_packet(uint8_t* raw_header, std::vector<uint8_t>* body, int timeout_ms) {
    if (fd_ < 0) {
        err_ = "未连接";
        return -1;
    }

    // ---- 固定头: 1 字节(类型+标志) ----
    uint8_t header[1];
    if (!recv_exact(header, 1, timeout_ms)) {
        if (timed_out_) {
            err_ = "接收超时";
            return 0;               // 超时不算错误, 连接继续用
        }
        if (fd_ >= 0) close_broken();
        return -1;
    }
    *raw_header = header[0];

    // ---- 剩余长度(变长整数, 最多 4 字节) ----
    size_t remaining = 0;
    int mult = 1;
    for (int i = 0; i < 4; ++i) {
        uint8_t b = 0;
        if (!recv_exact(&b, 1, kIoTimeoutMs)) {
            if (fd_ >= 0) close_broken();   // 读到一半断了 = 字节流错位, 必须重连
            return -1;
        }
        remaining += static_cast<size_t>(b & 0x7F) * mult;
        mult *= 128;
        if ((b & 0x80) == 0) break;
    }

    // ★ 长度上限: 正常报文的 payload 最多几百字节(控制指令就 50 上下)。
    //   万一字节流错位了(理论上不该发生), 也不至于被一个荒谬的长度撑爆内存
    //   —— 直接判定为协议错乱, 断开重连(比抛 bad_alloc 把进程带走强得多)。
    if (remaining > kMaxPayload) {
        err_ = "报文长度异常(疑似字节流错位), 断开重连";
        close_broken();
        return -1;
    }

    body->resize(remaining);
    if (remaining > 0 && !recv_exact(body->data(), remaining, kIoTimeoutMs)) {
        if (fd_ >= 0) close_broken();
        return -1;
    }
    return 1;
}

bool MqttSubscriber::poll(std::string* topic, std::string* payload, int timeout_ms) {
    if (fd_ < 0) {
        err_ = "未连接";
        return false;
    }
    if (!topic || !payload) return false;

    // ---- 空闲太久就发一次 PINGREQ, 别让 broker 把我们踢掉 ----
    const double idle = now_sec() - last_io_;
    if (idle > kKeepAliveSec / 2) {
        if (!send_pingreq()) {
            close_broken();
            return false;
        }
    }

    // ---- 读一个完整的报文 ----
    //
    // ★ 超时【不算错误】, 连接继续用。
    //   以前这里一超时就 close_broken(), 于是控制线程每 500ms 断一次重连 ——
    //   正好把"指令刚发出来"的那个窗口给错过了, 指令永远收不到(踩过, 2026-11-15)。
    uint8_t type = 0;
    std::vector<uint8_t> body;
    const int pr = read_packet(&type, &body, timeout_ms);
    if (pr <= 0) {
        if (pr == 0) return false;      // 超时, 正常
        return false;                   // 出错, 连接已断
    }

    // ---- 只处理 PUBLISH, 其余(SUBACK/PINGRESP/...)直接吃掉 ----
    if ((type & 0xF0) != 0x30) {
        err_.clear();
        return false;
    }
    if (body.size() < 2) return false;

    const uint16_t tlen = static_cast<uint16_t>((body[0] << 8) | body[1]);
    size_t off = 2;
    if (body.size() < off + tlen) return false;
    topic->assign(reinterpret_cast<const char*>(body.data() + off), tlen);
    off += tlen;

    // QoS > 0 的 PUBLISH 带 packet id
    const int qos = (type & 0x06) >> 1;
    if (qos > 0) off += 2;
    if (off > body.size()) return false;
    payload->assign(reinterpret_cast<const char*>(body.data() + off), body.size() - off);
    err_.clear();
    return true;
}

}  // namespace rk
