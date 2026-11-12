// led_serial.h —— C5HE LED 显示语音控制卡 (串口)
//
// 协议移植自 Python 侧 led_controller.py(同项目内), 板端可以直接驱动
// 门禁屏和语音模块, 不再依赖 Python 进程。
//
// [通用指令帧]
//   AA 55 | 流水号(2) | 地址(0x64) | 保留(2) | 命令字 | 长度(2,高在前) | 数据 | CRC16(2,高在前) | AF
//   CRC16 = 标准 Modbus (poly 0xA001, init 0xFFFF), 计算范围是
//   [流水号 .. 数据末尾 + 2 字节校验占位], 校验位高字节在前。
//
// [语音合成帧] (0xFD 开头, 控制卡原样转发给 SYN6658 语音模块)
//   FD | 数据区长度(2,高在前) | 0x01=语音合成 | 0x01=GBK编码 | GBK 文本
//   注意: 这类帧不需要 AA55 包头, 也不需要 CRC 和 AF 结束符。
#ifndef RK_LED_SERIAL_H_
#define RK_LED_SERIAL_H_

#include <cstdint>
#include <string>
#include <vector>

namespace rk {

// 命令字(与 led_controller.py 一致)
enum LedCmd : uint8_t {
    kLedCmdQueryVersion = 0x01,
    kLedCmdCancelTemp   = 0x21,
    kLedCmdPlayVoice    = 0x22,
    kLedCmdLoadAd       = 0x25,
    kLedCmdShowText     = 0x27,
    kLedCmdSetVolume    = 0xF0,
};

// 临显(0x27)控制字
enum LedColor : uint8_t { kLedColorRed = 1, kLedColorGreen = 2, kLedColorYellow = 3 };
constexpr int kLedRowFirst = 1;
constexpr size_t kLedTextMaxBytes = 60;   // 协议上限 60 字节(30 个汉字)

class LedSerial {
public:
    ~LedSerial() { close(); }

    // 打开串口(Linux: /dev/ttyUSB0; 默认 9600 8N1)
    bool open(const std::string& port, int baud = 9600);
    void close();
    bool is_open() const { return fd_ >= 0; }

    bool write_raw(const uint8_t* d, size_t n);

    // 组一帧通用指令(不发送)
    std::vector<uint8_t> build_packet(uint8_t cmd, const std::vector<uint8_t>& data);

    // 语音播报帧(0xFD)
    std::vector<uint8_t> build_tts_frame(const std::string& text_utf8);

    // 播报一句话
    bool speak(const std::string& text_utf8);
    // 屏幕临显一行
    bool show_text(const std::string& text_utf8, int duration_sec = 10,
                   int row = kLedRowFirst, uint8_t color = kLedColorRed);

    static uint16_t crc16(const uint8_t* d, size_t n, uint16_t init = 0xFFFF);
    // UTF-8 -> GBK(GB18030 兜底); 失败返回 false
    static bool utf8_to_gbk(const std::string& in, std::string& out);

private:
    int      fd_ = -1;
    uint16_t seq_ = 0;
};

}  // namespace rk

#endif  // RK_LED_SERIAL_H_
