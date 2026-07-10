#ifndef PROTOCOL_H
#define PROTOCOL_H

//=============================================================================
// protocol.h — PEMP2.0 通讯协议引擎
//
// 帧格式:  7BH | FUN | LEN(2B小端) | DATA | 7DH
// FUN:
//   01H 查询状态    02H 切换工作状态  03H 召唤遥测
//   04H 召唤历史SOE 06H 执行遥控      08H 同步时钟
//   09H 召唤遥信    51H 上传遥信      52H 上传SOE
//   53H 上传遥测    55H 异常帧
//=============================================================================

#include <cstdint>
#include <vector>
#include <string>
#include <ctime>

// ─── 协议常量 ──────────────────────────────────────────────────────────────

constexpr uint8_t  FRAME_START  = 0x7B;
constexpr uint8_t  FRAME_END    = 0x7D;
constexpr size_t   FRAME_HEADER = 4;   // START + FUN + LEN(2)
constexpr size_t   FRAME_TAIL   = 1;   // END
constexpr size_t   FRAME_OVERHEAD = FRAME_HEADER + FRAME_TAIL;  // 5

// ─── 功能码 ────────────────────────────────────────────────────────────────

enum class FunCode : uint8_t {
    QueryStatus      = 0x01,
    SwitchWorkState  = 0x02,
    CallTelemetry    = 0x03,
    CallHistorySOE   = 0x04,
    CallSetting      = 0x05,
    ExecRemoteCtrl   = 0x06,
    DownloadSetting  = 0x07,
    SyncClock        = 0x08,
    CallTeleInd      = 0x09,
    UploadTeleInd    = 0x51,
    UploadSOE        = 0x52,
    UploadTelemetry  = 0x53,
    UploadFile       = 0x54,
    ErrorFrame       = 0x55,
};

// ─── 工作状态请求码 (02H) ──────────────────────────────────────────────────

enum class WorkStateReq : uint8_t {
    SwitchToStandby = 0x00,
    SwitchToMaster  = 0x01,
    StopAutoUpload  = 0x02,
    StartAutoUpload = 0x03,
};

// ─── 遥控执行结果 (06H) ────────────────────────────────────────────────────

enum class CtrlResult : uint8_t {
    Failed     = 0x00,
    Success    = 0xFF,
    BadPwd     = 0x01,
};

// ─── 异常码 (55H) ──────────────────────────────────────────────────────────

enum class ErrorCode : uint8_t {
    Busy           = 0x01,
    BadCommand     = 0x02,
    UnknownFun     = 0x03,
    DeviceError    = 0x04,
};

// ─── CP56time2a 时间格式 (7字节) ──────────────────────────────────────────

struct CP56time2a {
    uint8_t ms_low;     // 毫秒低字节
    uint8_t ms_high;    // 毫秒高字节 (0-59999)
    uint8_t minute;     // 分 (0-59)
    uint8_t hour;       // 时 (0-23)
    uint8_t day;        // 日 (1-31)
    uint8_t month;      // 月 (1-12)
    uint8_t year;       // 年 - 2000

    static CP56time2a Now();
    static CP56time2a FromTimeT(std::time_t t);
    static CP56time2a FromMs(uint64_t msSinceEpoch);
    std::time_t ToTimeT() const;
    std::string ToString() const;
};

// ─── SOE 事件 ──────────────────────────────────────────────────────────────

struct SOEEvent {
    uint8_t     channel;
    uint8_t     device;
    uint16_t    soeId;
    CP56time2a  timestamp;
    uint8_t     status;     // 01=开(OFF), 02=合(ON), 00=未使用

    std::string ToString() const;
};

// ─── 帧构建器 ──────────────────────────────────────────────────────────────

class FrameBuilder {
public:
    FrameBuilder() = default;

    /// 开始新帧: 写入 FRAME_START + FUN
    void Begin(FunCode fun);

    /// 追加字节数据
    void Append(uint8_t byte);
    void Append(const uint8_t* data, size_t len);

    /// 追加 uint16 (小端)
    void AppendU16(uint16_t val);

    /// 追加 uint32 (小端)
    void AppendU32(uint32_t val);

    /// 追加 int32 (小端)
    void AppendI32(int32_t val);

    /// 结束帧: 写入 FRAME_END，返回完整帧
    std::vector<uint8_t> End();

    /// 构建简单帧 (无数据区): 7B FUN 00 00 7D
    static std::vector<uint8_t> MakeSimple(FunCode fun);

    /// 构建含 1 字节数据的帧: 7B FUN 01 00 DATA 7D
    static std::vector<uint8_t> Make1Byte(FunCode fun, uint8_t data);

    /// 构建 03H/53H 遥测响应帧
    static std::vector<uint8_t> MakeTelemetryFrame(
        FunCode fun, uint8_t ch, uint8_t dev,
        const int32_t* values, uint16_t count);

    /// 构建 09H/51H 遥信帧 (位打包)
    static std::vector<uint8_t> MakeTeleIndFrame(
        FunCode fun, uint8_t ch, uint8_t dev,
        uint16_t totalPoints, const uint8_t* packedBytes, uint16_t byteCount);

    /// 构建异常帧 55H
    static std::vector<uint8_t> MakeErrorFrame(FunCode origFun, ErrorCode err);

    /// 构建 SOE 帧 (04H/52H)
    static std::vector<uint8_t> MakeSOEFrame(
        FunCode fun, const SOEEvent* events, uint16_t count);

private:
    std::vector<uint8_t> buf_;
};

// ─── 帧解析器 ──────────────────────────────────────────────────────────────

class FrameParser {
public:
    FrameParser() = default;

    /// 清空内部缓冲区
    void Reset();

    /// 追加接收到的字节数据，尝试提取完整帧
    /// @return 提取到的帧列表（可能 0 或 1 或多帧粘连）
    std::vector<std::vector<uint8_t>> Feed(const uint8_t* data, size_t len);

    // ── 从完整帧中提取字段 ──

    /// 取 FUN 码 (帧第1字节)
    static FunCode GetFun(const std::vector<uint8_t>& frame);

    /// 取数据区长度 (帧第2-3字节，小端)
    static uint16_t GetDataLen(const std::vector<uint8_t>& frame);

    /// 取数据区首指针
    static const uint8_t* GetData(const std::vector<uint8_t>& frame);

    /// 读第 i 个字节 (相对数据区起始)
    static uint8_t  ReadByte(const std::vector<uint8_t>& frame, size_t offset);

    /// 读 uint16 (小端，相对数据区起始)
    static uint16_t ReadU16(const std::vector<uint8_t>& frame, size_t offset);

    /// 读 uint32 (小端)
    static uint32_t ReadU32(const std::vector<uint8_t>& frame, size_t offset);

    /// 读 int32 (小端)
    static int32_t  ReadI32(const std::vector<uint8_t>& frame, size_t offset);

    /// 读 CP56time2a (7字节，相对数据区起始)
    static CP56time2a ReadCP56(const std::vector<uint8_t>& frame, size_t offset);

    /// 帧校验：长度/帧头帧尾/数据区长度一致性
    static bool Validate(const std::vector<uint8_t>& frame);

private:
    std::vector<uint8_t> buffer_;
};

#endif // PROTOCOL_H
