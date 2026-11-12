#include "led_serial.h"

#include <errno.h>
#include <fcntl.h>
#include <iconv.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <cstdio>

namespace rk {
namespace {

constexpr uint8_t kPacketHead[2] = {0xAA, 0x55};
constexpr uint8_t kPacketTail = 0xAF;
constexpr uint8_t kDeviceAddr = 0x64;
constexpr uint8_t kTtsHead = 0xFD;
constexpr uint8_t kTtsCmdSpeak = 0x01;
constexpr uint8_t kTtsEncGbk = 0x01;
constexpr size_t   kTtsMaxFrame = 255;

speed_t baud_to_speed(int baud) {
    switch (baud) {
        case 9600:  return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
        default: return B9600;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
uint16_t LedSerial::crc16(const uint8_t* d, size_t n, uint16_t init) {
    uint16_t crc = init & 0xFFFF;
    for (size_t i = 0; i < n; ++i) {
        crc ^= d[i];
        for (int b = 0; b < 8; ++b) {
            if (crc & 1) crc = static_cast<uint16_t>((crc >> 1) ^ 0xA001);
            else        crc = static_cast<uint16_t>(crc >> 1);
        }
    }
    return crc;
}

bool LedSerial::utf8_to_gbk(const std::string& in, std::string& out) {
    iconv_t cd = ::iconv_open("GBK", "UTF-8");
    if (cd == (iconv_t)-1) return false;
    std::vector<char> buf(in.size() * 2 + 16);
    char* inbuf = const_cast<char*>(in.data());
    size_t inleft = in.size();
    char* outbuf = buf.data();
    size_t outleft = buf.size();
    size_t rc = ::iconv(cd, &inbuf, &inleft, &outbuf, &outleft);
    ::iconv_close(cd);
    if (rc == (size_t)-1) return false;
    out.assign(buf.data(), buf.size() - outleft);
    return true;
}

// ---------------------------------------------------------------------------
bool LedSerial::open(const std::string& port, int baud) {
    close();
    fd_ = ::open(port.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd_ < 0) {
        fprintf(stderr, "[串口] 打开 %s 失败: %s\n", port.c_str(), strerror(errno));
        return false;
    }

    struct termios tio;
    memset(&tio, 0, sizeof(tio));
    if (::tcgetattr(fd_, &tio) != 0) {
        fprintf(stderr, "[串口] tcgetattr 失败: %s\n", strerror(errno));
        close();
        return false;
    }
    ::cfmakeraw(&tio);                       // 裸模式: 不做回环/换行转换
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~static_cast<tcflag_t>(CSTOPB);
    tio.c_cflag &= ~static_cast<tcflag_t>(PARENB);
    tio.c_cflag &= ~static_cast<tcflag_t>(CSIZE);
    tio.c_cflag |= CS8;                      // 8 位
    tio.c_cflag &= ~static_cast<tcflag_t>(PARENB);
    ::cfsetispeed(&tio, baud_to_speed(baud));
    ::cfsetospeed(&tio, baud_to_speed(baud));
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    if (::tcsetattr(fd_, TCSANOW, &tio) != 0) {
        fprintf(stderr, "[串口] tcsetattr 失败: %s\n", strerror(errno));
        close();
        return false;
    }
    ::tcflush(fd_, TCIOFLUSH);
    fprintf(stderr, "[串口] 已打开 %s @ %d 8N1\n", port.c_str(), baud);
    return true;
}

void LedSerial::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool LedSerial::write_raw(const uint8_t* d, size_t n) {
    if (fd_ < 0) return false;
    size_t left = n;
    const uint8_t* p = d;
    while (left > 0) {
        ssize_t w = ::write(fd_, p, left);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            fprintf(stderr, "[串口] 写失败: %s\n", strerror(errno));
            return false;
        }
        p += w;
        left -= static_cast<size_t>(w);
    }
    ::tcdrain(fd_);      // 等发送完
    return true;
}

// ---------------------------------------------------------------------------
std::vector<uint8_t> LedSerial::build_packet(uint8_t cmd, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> body;
    body.push_back(static_cast<uint8_t>(seq_ >> 8));
    body.push_back(static_cast<uint8_t>(seq_ & 0xFF));
    body.push_back(kDeviceAddr);
    body.push_back(0x00);                  // 保留
    body.push_back(0x00);
    body.push_back(cmd);
    body.push_back(static_cast<uint8_t>((data.size() >> 8) & 0xFF));
    body.push_back(static_cast<uint8_t>(data.size() & 0xFF));
    body.insert(body.end(), data.begin(), data.end());

    // 校验位参与计算, 用占位字节代替
    body.push_back(0x00);
    body.push_back(0x00);
    const uint16_t crc = crc16(body.data(), body.size());
    body[body.size() - 2] = static_cast<uint8_t>(crc >> 8);   // 高字节在前
    body[body.size() - 1] = static_cast<uint8_t>(crc & 0xFF);

    std::vector<uint8_t> frame;
    frame.insert(frame.end(), kPacketHead, kPacketHead + 2);
    frame.insert(frame.end(), body.begin(), body.end());
    frame.push_back(kPacketTail);
    ++seq_;
    return frame;
}

std::vector<uint8_t> LedSerial::build_tts_frame(const std::string& text_utf8) {
    std::string gbk;
    if (!utf8_to_gbk(text_utf8, gbk)) {
        fprintf(stderr, "[串口] 文本无法转成 GBK: %s\n", text_utf8.c_str());
        return {};
    }
    std::vector<uint8_t> data_area;
    data_area.push_back(kTtsCmdSpeak);
    data_area.push_back(kTtsEncGbk);
    data_area.insert(data_area.end(), gbk.begin(), gbk.end());

    std::vector<uint8_t> frame;
    frame.push_back(kTtsHead);
    frame.push_back(static_cast<uint8_t>((data_area.size() >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>(data_area.size() & 0xFF));
    frame.insert(frame.end(), data_area.begin(), data_area.end());
    if (frame.size() > kTtsMaxFrame) {
        fprintf(stderr, "[串口] TTS 帧 %zu 字节超过上限 %zu, 请缩短播报内容\n",
                frame.size(), kTtsMaxFrame);
        return {};
    }
    return frame;
}

bool LedSerial::speak(const std::string& text_utf8) {
    auto frame = build_tts_frame(text_utf8);
    if (frame.empty()) return false;
    // 指令之间至少留 25ms(控制卡转发给语音模块的内部间隔), 这里留 60ms
    ::usleep(60000);
    return write_raw(frame.data(), frame.size());
}

bool LedSerial::show_text(const std::string& text_utf8, int duration_sec, int row, uint8_t color) {
    std::string gbk;
    if (!utf8_to_gbk(text_utf8, gbk)) {
        fprintf(stderr, "[串口] 临显文本无法转成 GBK: %s\n", text_utf8.c_str());
        return false;
    }
    if (gbk.size() > kLedTextMaxBytes) {
        gbk.resize(kLedTextMaxBytes);       // 协议上限, 超出部分截断
    }
    std::vector<uint8_t> data;
    data.push_back(static_cast<uint8_t>(row));            // 控制字1: 行号
    data.push_back(static_cast<uint8_t>(duration_sec));   // 控制字2: 时长(秒), 0=长期
    data.push_back(color);                                // 控制字3: 颜色
    data.push_back(0x00);                                 // 控制字4
    data.insert(data.end(), gbk.begin(), gbk.end());

    auto frame = build_packet(kLedCmdShowText, data);
    return write_raw(frame.data(), frame.size());
}

}  // namespace rk
