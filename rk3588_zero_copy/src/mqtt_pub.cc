#include "mqtt_pub.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
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

// socket 的收发超时(毫秒)。
//
// ★ 为什么必须设:
// ★ 为什么必须设:
//   socket 默认是【阻塞】的, send()/recv() 可以一直卡着 —— 对端不收数据时
//   send() 会把内核发送缓冲区填满然后永远阻塞, recv() 在 broker 不回 PUBACK
//   时也会一直等。没有超时的话, 一个半死不活的 broker 就能把后台线程卡死。
const int kIoTimeoutMs = 2000;

}  // namespace

void MqttPublisher::close_broken() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool MqttPublisher::send_all(const void* d, size_t n) {
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
            // 发送超时(SO_SNDTIMEO 到了): 对端不收数据。
            // 重试几次还是不行就放弃 —— 绝不能在这里死循环。
            if (++stalls > 2) {
                err_ = "发送超时(对端不收数据)";
                close_broken();
                return false;
            }
            continue;
        }
        err_ = std::string("send 失败: ") + strerror(errno);
        close_broken();      // 连接已经不可用了, 关掉好让上层重连
        return false;
    }
    return true;
}

bool MqttPublisher::recv_exact(void* d, size_t n, int timeout_ms) {
    uint8_t* p = static_cast<uint8_t*>(d);
    size_t left = n;
    while (left > 0) {
        struct pollfd pfd = {fd_, POLLIN, 0};
        int pr = ::poll(&pfd, 1, timeout_ms);
        if (pr <= 0) {
            err_ = (pr == 0) ? "接收超时" : std::string("poll 失败: ") + strerror(errno);
            close_broken();
            return false;
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
    return true;
}

bool MqttPublisher::handshake(const std::string& client_id, int timeout_ms) {
    // CONNECT: 0x10 | len | "MQTT" | 0x04 | flags(clean=0x02) | keepalive(60) | clientid
    std::vector<uint8_t> pkt;
    pkt.push_back(0x10);

    std::vector<uint8_t> var;
    var.push_back(0x00); var.push_back(0x04);
    var.push_back('M'); var.push_back('Q'); var.push_back('T'); var.push_back('T');
    var.push_back(0x04);          // protocol level 3.1.1
    var.push_back(0x02);          // connect flags: clean session
    put_u16(var, 60);             // keepalive
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

bool MqttPublisher::connect(const std::string& host, int port, const std::string& client_id,
                             int timeout_ms) {
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

        // 非阻塞 connect + poll 超时
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
        // 收发超时: 防止对端半死不活时把调用线程卡死
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
    if (!handshake(client_id, timeout_ms)) {
        close_broken();
        return false;
    }
    err_.clear();
    return true;
}

bool MqttPublisher::ensure_connected(const std::string& host, int port,
                                     const std::string& client_id, int timeout_ms) {
    if (fd_ >= 0) return true;      // 已经连上了
    return connect(host, port, client_id, timeout_ms);
}

void MqttPublisher::disconnect() {
    if (fd_ >= 0) {
        // 发一个 DISCONNECT(0xE0 0x00) 再关, 让 broker 干净地清掉会话
        uint8_t bye[2] = {0xE0, 0x00};
        ::send(fd_, bye, 2, MSG_NOSIGNAL);
        ::close(fd_);
        fd_ = -1;
    }
}

bool MqttPublisher::publish(const std::string& topic, const std::string& payload, int qos) {
    if (fd_ < 0) {
        err_ = "未连接";
        return false;
    }
    if (qos < 0) qos = 0;
    if (qos > 1) qos = 1;

    std::vector<uint8_t> pkt;
    pkt.push_back(static_cast<uint8_t>(0x30 | (qos << 1)));

    std::vector<uint8_t> var;
    put_u16(var, static_cast<uint16_t>(topic.size()));
    var.insert(var.end(), topic.begin(), topic.end());
    uint16_t pid = 0;
    if (qos > 0) {
        pid = ++pid_;
        put_u16(var, pid);
    }
    var.insert(var.end(), payload.begin(), payload.end());

    encode_remaining_length(pkt, var.size());
    pkt.insert(pkt.end(), var.begin(), var.end());

    if (!send_all(pkt.data(), pkt.size())) return false;

    if (qos > 0) {
        uint8_t ack[4];
        if (!recv_exact(ack, 4, 3000)) return false;
        if ((ack[0] & 0xF0) != 0x40) {
            err_ = "未收到 PUBACK";
            return false;
        }
    }
    return true;
}

}  // namespace rk
