//=============================================================================
// protocol.cxx — PEMP2.0 通讯协议引擎实现
//=============================================================================

#include "protocol.h"
#include <cstring>
#include <cstdio>
#include <algorithm>

// ─── CP56time2a ─────────────────────────────────────────────────────────────

CP56time2a CP56time2a::Now()
{
    return FromTimeT(std::time(nullptr));
}

CP56time2a CP56time2a::FromTimeT(std::time_t t)
{
    CP56time2a c56{};
    struct tm ti{};
#ifdef _WIN32
    localtime_s(&ti, &t);
#else
    localtime_r(&t, &ti);
#endif
    uint32_t ms = static_cast<uint32_t>(ti.tm_sec) * 1000;
    c56.ms_low  = static_cast<uint8_t>(ms & 0xFF);
    c56.ms_high = static_cast<uint8_t>((ms >> 8) & 0xFF);
    c56.minute  = static_cast<uint8_t>(ti.tm_min);
    c56.hour    = static_cast<uint8_t>(ti.tm_hour);
    c56.day     = static_cast<uint8_t>(ti.tm_mday);
    c56.month   = static_cast<uint8_t>(ti.tm_mon + 1);
    c56.year    = static_cast<uint8_t>(ti.tm_year - 100);
    return c56;
}

CP56time2a CP56time2a::FromMs(uint64_t msSinceEpoch)
{
    std::time_t sec = static_cast<std::time_t>(msSinceEpoch / 1000);
    uint16_t ms = static_cast<uint16_t>(msSinceEpoch % 1000);
    CP56time2a t = FromTimeT(sec);
    // FromTimeT 已将 tm_sec*1000 存入 ms_low/ms_high（分钟内的毫秒偏移）
    // 加上亚毫秒余数得到完整的 CP56time2a 毫秒值
    uint16_t totalMs = static_cast<uint16_t>(
        (static_cast<uint16_t>(t.ms_low)
       | (static_cast<uint16_t>(t.ms_high) << 8)) + ms);
    t.ms_low  = static_cast<uint8_t>(totalMs & 0xFF);
    t.ms_high = static_cast<uint8_t>((totalMs >> 8) & 0xFF);
    return t;
}

std::time_t CP56time2a::ToTimeT() const
{
    struct tm ti{};
    uint32_t totalMs = static_cast<uint32_t>(ms_low) | (static_cast<uint32_t>(ms_high) << 8);
    ti.tm_sec   = static_cast<int>(totalMs / 1000);
    ti.tm_min   = minute;
    ti.tm_hour  = hour;
    ti.tm_mday  = day;
    ti.tm_mon   = month - 1;
    ti.tm_year  = year + 100;
    ti.tm_isdst = -1;
    return std::mktime(&ti);
}

std::string CP56time2a::ToString() const
{
    uint32_t ms = static_cast<uint32_t>(ms_low) | (static_cast<uint32_t>(ms_high) << 8);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "20%02u-%02u-%02u %02u:%02u ms=%u",
                  static_cast<unsigned>(year),
                  static_cast<unsigned>(month),
                  static_cast<unsigned>(day),
                  static_cast<unsigned>(hour),
                  static_cast<unsigned>(minute),
                  static_cast<unsigned>(ms));
    return buf;
}

// ─── SOEEvent ───────────────────────────────────────────────────────────────

std::string SOEEvent::ToString() const
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "SOE[ch=%u dev=%u id=%u sta=%u %s]",
                  static_cast<unsigned>(channel),
                  static_cast<unsigned>(device),
                  static_cast<unsigned>(soeId),
                  static_cast<unsigned>(status),
                  timestamp.ToString().c_str());
    return buf;
}

// ─── FrameBuilder ───────────────────────────────────────────────────────────

void FrameBuilder::Begin(FunCode fun)
{
    buf_.clear();
    buf_.push_back(FRAME_START);
    buf_.push_back(static_cast<uint8_t>(fun));
    // LEN 占位，稍后 End() 时填入
    buf_.push_back(0);
    buf_.push_back(0);
}

void FrameBuilder::Append(uint8_t byte)
{
    buf_.push_back(byte);
}

void FrameBuilder::Append(const uint8_t* data, size_t len)
{
    buf_.insert(buf_.end(), data, data + len);
}

void FrameBuilder::AppendU16(uint16_t val)
{
    buf_.push_back(static_cast<uint8_t>(val & 0xFF));
    buf_.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
}

void FrameBuilder::AppendU32(uint32_t val)
{
    buf_.push_back(static_cast<uint8_t>(val & 0xFF));
    buf_.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    buf_.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    buf_.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
}

void FrameBuilder::AppendI32(int32_t val)
{
    AppendU32(static_cast<uint32_t>(val));
}

std::vector<uint8_t> FrameBuilder::End()
{
    // 填入数据区长度 (位于 buf_[2..3])
    size_t dataLen = buf_.size() - FRAME_HEADER;
    buf_[2] = static_cast<uint8_t>(dataLen & 0xFF);
    buf_[3] = static_cast<uint8_t>((dataLen >> 8) & 0xFF);
    // 帧尾
    buf_.push_back(FRAME_END);
    return std::move(buf_);
}

std::vector<uint8_t> FrameBuilder::MakeSimple(FunCode fun)
{
    FrameBuilder fb;
    fb.Begin(fun);
    return fb.End();
}

std::vector<uint8_t> FrameBuilder::Make1Byte(FunCode fun, uint8_t data)
{
    FrameBuilder fb;
    fb.Begin(fun);
    fb.Append(data);
    return fb.End();
}

std::vector<uint8_t> FrameBuilder::MakeTelemetryFrame(
    FunCode fun, uint8_t ch, uint8_t dev,
    const int32_t* values, uint16_t count)
{
    FrameBuilder fb;
    fb.Begin(fun);
    fb.Append(ch);
    fb.Append(dev);
    fb.AppendU16(count);
    for (uint16_t i = 0; i < count; i++)
        fb.AppendI32(values[i]);
    return fb.End();
}

std::vector<uint8_t> FrameBuilder::MakeTeleIndFrame(
    FunCode fun, uint8_t ch, uint8_t dev,
    uint16_t totalPoints, const uint8_t* packedBytes, uint16_t byteCount)
{
    FrameBuilder fb;
    fb.Begin(fun);
    fb.Append(ch);
    fb.Append(dev);
    fb.AppendU16(totalPoints);
    fb.Append(packedBytes, byteCount);
    return fb.End();
}

std::vector<uint8_t> FrameBuilder::MakeErrorFrame(FunCode origFun, ErrorCode err)
{
    FrameBuilder fb;
    fb.Begin(FunCode::ErrorFrame);
    fb.AppendU16(static_cast<uint16_t>(origFun));
    fb.Append(static_cast<uint8_t>(err));
    return fb.End();
}

std::vector<uint8_t> FrameBuilder::MakeSOEFrame(
    FunCode fun, const SOEEvent* events, uint16_t count)
{
    FrameBuilder fb;
    fb.Begin(fun);
    fb.AppendU16(count);
    for (uint16_t i = 0; i < count; i++)
    {
        const auto& e = events[i];
        fb.Append(e.channel);
        fb.Append(e.device);
        fb.AppendU16(e.soeId);
        fb.Append(e.timestamp.ms_low);
        fb.Append(e.timestamp.ms_high);
        fb.Append(e.timestamp.minute);
        fb.Append(e.timestamp.hour);
        fb.Append(e.timestamp.day);
        fb.Append(e.timestamp.month);
        fb.Append(e.timestamp.year);
        fb.Append(e.status);
    }
    return fb.End();
}

// ─── FrameParser ────────────────────────────────────────────────────────────

void FrameParser::Reset()
{
    buffer_.clear();
}

std::vector<std::vector<uint8_t>> FrameParser::Feed(const uint8_t* data, size_t len)
{
    buffer_.insert(buffer_.end(), data, data + len);
    std::vector<std::vector<uint8_t>> frames;

    while (buffer_.size() >= FRAME_OVERHEAD)
    {
        // 找帧头
        auto it = std::find(buffer_.begin(), buffer_.end(), FRAME_START);
        if (it == buffer_.end())
        {
            buffer_.clear();
            break;
        }

        // 丢弃帧头之前的字节
        if (it != buffer_.begin())
            buffer_.erase(buffer_.begin(), it);

        // 最少需要 FRAME_OVERHEAD 字节
        if (buffer_.size() < FRAME_OVERHEAD)
            break;

        // 读数据区长度
        unsigned int rawLen = static_cast<unsigned int>(buffer_[2])
                            | (static_cast<unsigned int>(buffer_[3]) << 8);
        uint16_t dataLen = static_cast<uint16_t>(rawLen);

        // 防止畸形帧声明超大长度导致内存膨胀
        constexpr size_t MAX_FRAME_SIZE = 4096;
        if (dataLen > MAX_FRAME_SIZE) {
            buffer_.erase(buffer_.begin());  // 丢弃帧头继续搜索
            continue;
        }

        size_t totalLen = FRAME_OVERHEAD + dataLen; // 5 + dataLen
        if (buffer_.size() < totalLen)
            break;  // 数据不足，等待更多

        // 校验帧尾
        if (buffer_[totalLen - 1] != FRAME_END)
        {
            // 帧尾错误，丢弃帧头继续找
            buffer_.erase(buffer_.begin());
            continue;
        }

        // 提取完整帧
        std::vector<uint8_t> frame(buffer_.begin(), buffer_.begin() + totalLen);
        frames.push_back(std::move(frame));
        buffer_.erase(buffer_.begin(), buffer_.begin() + totalLen);
    }

    return frames;
}

FunCode FrameParser::GetFun(const std::vector<uint8_t>& frame)
{
    return static_cast<FunCode>(frame[1]);
}

uint16_t FrameParser::GetDataLen(const std::vector<uint8_t>& frame)
{
    unsigned int raw = static_cast<unsigned int>(frame[2])
                     | (static_cast<unsigned int>(frame[3]) << 8);
    return static_cast<uint16_t>(raw);
}

const uint8_t* FrameParser::GetData(const std::vector<uint8_t>& frame)
{
    return frame.data() + FRAME_HEADER;
}

uint8_t FrameParser::ReadByte(const std::vector<uint8_t>& frame, size_t offset)
{
    return frame[FRAME_HEADER + offset];
}

uint16_t FrameParser::ReadU16(const std::vector<uint8_t>& frame, size_t offset)
{
    size_t pos = FRAME_HEADER + offset;
    unsigned int raw = static_cast<unsigned int>(frame[pos])
                     | (static_cast<unsigned int>(frame[pos + 1]) << 8);
    return static_cast<uint16_t>(raw);
}

uint32_t FrameParser::ReadU32(const std::vector<uint8_t>& frame, size_t offset)
{
    size_t pos = FRAME_HEADER + offset;
    return static_cast<uint32_t>(frame[pos])
         | (static_cast<uint32_t>(frame[pos + 1]) << 8)
         | (static_cast<uint32_t>(frame[pos + 2]) << 16)
         | (static_cast<uint32_t>(frame[pos + 3]) << 24);
}

int32_t FrameParser::ReadI32(const std::vector<uint8_t>& frame, size_t offset)
{
    return static_cast<int32_t>(ReadU32(frame, offset));
}

CP56time2a FrameParser::ReadCP56(const std::vector<uint8_t>& frame, size_t offset)
{
    size_t pos = FRAME_HEADER + offset;
    CP56time2a c56{};
    c56.ms_low  = frame[pos];
    c56.ms_high = frame[pos + 1];
    c56.minute  = frame[pos + 2];
    c56.hour    = frame[pos + 3];
    c56.day     = frame[pos + 4];
    c56.month   = frame[pos + 5];
    c56.year    = frame[pos + 6];
    return c56;
}

bool FrameParser::Validate(const std::vector<uint8_t>& frame)
{
    if (frame.size() < FRAME_OVERHEAD) return false;
    if (frame[0] != FRAME_START) return false;
    if (frame.back() != FRAME_END) return false;
    uint16_t dataLen = GetDataLen(frame);
    if (frame.size() != FRAME_OVERHEAD + dataLen) return false;
    return true;
}
