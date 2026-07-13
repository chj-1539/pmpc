#ifndef DATA_RECORDER_HELPERS_H
#define DATA_RECORDER_HELPERS_H

//=============================================================================
// data_recorder_helpers.h — DataRecorder 纯逻辑辅助函数
//
// 目的：把 TimerThread 的配置健壮化算法（CR-2 修复配套）抽到独立头文件里，
// 让测试可零依赖调用，不用拉 mysql.h。data_recorder.h 内的 static inline
// 成员函数也来源于此，见 CR-2 修复记录（CLAUDE.md）。
//
// 不能放到 data_recorder.h 是因为该头 include <mysql.h>，而 CI 环境和普通
// 测试环境都没有 MySQL 头。
//=============================================================================

namespace pmpc {
namespace data_recorder {

// aiIntervalMs 若为 0 / 负 → sleep_for(0ms) 忙等 100% CPU + 后续
// `120000 / interval` 除零 UB。一律 clamp 到 ≥ 100ms。
inline int ClampAiIntervalMs(int cfg) {
    return cfg < 100 ? 100 : cfg;
}

// 清理周期（tick 数）= 120000 / 有效间隔（ms）。有效间隔已 clamp ≥100
// → 最大 1200；但若外部直接传入极大值（> 120000）会得 0 → % 0 UB。
// 因此再兜底一次 ≥ 1。
inline int ComputeCleanupPeriodTicks(int effectiveIntervalMs) {
    int p = (effectiveIntervalMs > 0) ? (120000 / effectiveIntervalMs) : 1;
    return p < 1 ? 1 : p;
}

} // namespace data_recorder
} // namespace pmpc

#endif // DATA_RECORDER_HELPERS_H
