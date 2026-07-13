#ifndef IEC101_SLAVE_H
#define IEC101_SLAVE_H

//=============================================================================
// iec101_slave.h — IEC 60870-5-101 从站模块
//
// 串口监听 IEC 101 主站请求
// 支持总召唤 GI、遥控、时钟同步
// DI 变化自动上送
//=============================================================================

#include "comm_io.h"
#include "pmpc.h"
#include "iec104_master.h"  // 复用 IecType, IecCOT
#include "event_bus.h"
#include "module_manager.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <cstdint>

struct Slave101DIMapping { uint32_t ioa; uint16_t ch, dev, point; };
struct Slave101AIMapping { uint32_t ioa; uint8_t type = IecType::M_ME_NC_1; uint16_t ch, dev, point; double scale = 1.0, offset = 0.0; };
struct Slave101DOMapping { uint32_t ioa; int val; uint16_t ch, dev, point; };
struct Slave101DeviceConfig { uint16_t linkAddr; uint16_t coa; std::string desc;
    std::map<uint32_t, Slave101DIMapping> diMap; std::map<uint32_t, Slave101AIMapping> aiMap;
    std::map<uint32_t, std::vector<Slave101DOMapping>> doMap; };
struct Slave101Config {
    std::string portName = "COM1"; int baud = 9600; std::string parity = "even";
    int dataBits = 8; int stopBits = 1; int verbose = 1;
    std::vector<Slave101DeviceConfig> devices;
};

class Iec101Slave {
public:
    Iec101Slave(); ~Iec101Slave();
    bool LoadConfig(const std::string& path); bool Start(); void Stop();
    bool IsRunning() const { return running_; }

    // IEC101 帧完整性判定（L11 修复）。抽成 inline 静态，方便直接测。
    // 固定帧: [START_FIX=0x10, CTRL, ADDR, CS, END=0x16] 恰 5 字节。
    static inline bool IsCompleteFixedFrame(const uint8_t* buf, size_t pos) {
        constexpr uint8_t START_FIX = 0x10;
        constexpr uint8_t END       = 0x16;
        return pos == 5 && buf[0] == START_FIX && buf[4] == END;
    }
    // 可变帧（pmpc 简化格式）: [START_VAR, LEN, payload..., END]
    // 老代码判定 `pos >= buf[1] + 2 && buf[pos-1] == END`。L11 修复：额外
    // 要求 buf[1] >= 4 —— 长度字段至少要能容纳 CTRL(1) + ADDR(2) + CS(1)，
    // 否则是明显的畸形帧（buf[1]=0 会让 pos>=2 的极短噪声流被误判）。
    static inline bool IsCompleteVariableFrame(const uint8_t* buf, size_t pos) {
        constexpr uint8_t START_VAR = 0x68;
        constexpr uint8_t END       = 0x16;
        if (pos < 4)                 return false;
        if (buf[0] != START_VAR)     return false;
        if (buf[1] < 4)              return false;   // L11: 长度字段下限
        if (pos < static_cast<size_t>(buf[1]) + 2) return false;
        return buf[pos-1] == END;
    }

    // CR-6（第二轮）修复配套：可变帧 LEN 字段值。规约要求
    //   LEN = CTRL(1) + ADDR(2) + N_asdu = 3 + asduLen
    // 老代码写成 `4 + asduLen` 让所有从站发出的帧多 1 字节 → CS 校验失败。
    // 抽 inline helper 供测试断言。
    static inline uint8_t VariableFrameLen(size_t asduLen) {
        return static_cast<uint8_t>(3 + asduLen);
    }

    // CR-5（第二轮）修复配套：给定起始字节，判断"CTRL 从 buf 的哪个偏移读"。
    //   固定帧: buf[0]=0x10, CTRL 在 buf[1]
    //   可变帧: buf[0]=0x68, buf[1..3]=LEN,LEN,0x68, CTRL 在 buf[4]
    // 老代码不分帧类型统一读 buf[1]，可变帧下会误把 LEN 当 CTRL，让
    // LEN 低 4 位刚好 =0/A/B 时误发 Reset ACK / Class1 数据。
    // 返回 CTRL 偏移；未知起始符返回 SIZE_MAX。
    static inline size_t CtrlOffsetForStart(uint8_t startByte) {
        if (startByte == 0x10) return 1;   // START_FIX
        if (startByte == 0x68) return 4;   // START_VAR
        return static_cast<size_t>(-1);
    }

    // 101-C（第二轮）修复配套：SendACK 生成的固定帧字节序列。规约要求
    //   [0x10 CTRL ADDR_L ADDR_H CS 0x16]  共 6 字节
    //   CS = CTRL + ADDR_L + ADDR_H（3 字节 mod 256）
    // 老代码只写 1 字节 addr（5 字节帧），可变帧却写 2 字节 → 位宽不一致；
    // 且 linkAddr > 255 时高字节丢失。inline helper 供测试断言字节布局，
    // 与生产 SendACK 共享同一 CalcCS。
    static inline void BuildFixedAck(uint16_t linkAddr, uint8_t out[6]) {
        constexpr uint8_t START_FIX = 0x10;
        constexpr uint8_t END       = 0x16;
        out[0] = START_FIX;
        out[1] = 0x03;                                          // CTRL: primary+fun=3 (ACK)
        out[2] = static_cast<uint8_t>(linkAddr & 0xFF);         // ADDR low
        out[3] = static_cast<uint8_t>((linkAddr >> 8) & 0xFF);  // ADDR high
        out[4] = CalcCS(out + 1, 3);                            // CS covers CTRL+ADDR_L+ADDR_H
        out[5] = END;
    }
    // 供 BuildFixedAck 复用，与 iec101_slave.cxx 里的 static CalcCS 语义一致
    static inline uint8_t CalcCS(const uint8_t* data, size_t len) {
        uint8_t cs = 0;
        for (size_t i = 0; i < len; i++) cs = static_cast<uint8_t>(cs + data[i]);
        return cs;
    }
private:
    void PortThread();
    void HandleFrame(CommIO& io, const uint8_t* buf, size_t len);
    void SendGIRsp(CommIO& io, uint16_t linkAddr, uint16_t coa);
    void SendACK(CommIO& io, uint16_t linkAddr);
    bool FindAndExecDO(const uint8_t* asdu, size_t len);

    // CalcCS 已在 public 段定义为 inline —— 101-C 修复配套让 BuildFixedAck
    // 也能零依赖调用。私有段的重复声明已删除。
    static int SafeStoi(const std::string& s, int def = 0);

    Slave101Config config_; std::atomic<bool> running_{false}; std::thread portThr_;
    size_t tokenDI_ = 0;
};

class Iec101SlaveModule : public AppModule {
public:
    Iec101SlaveModule(); ~Iec101SlaveModule() override;
    const char* Name() const override { return "iec101_slave"; }
    bool LoadConfig(const std::string& cfgPath) override;
    bool ValidateConfig(const std::string& cfgPath, std::vector<std::string>& errors) override;
    bool Start() override; void Stop() override; bool IsRunning() const override;
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

#endif
