//=============================================================================
// pmpc_threads.cxx — 模拟采集/遥控子线程（仅调试/测试用）
// 注意：此文件未加入任何构建目标（CMakeLists.txt / tasks.json 均不包含）
//       如要启用，需在 CMakeLists.txt 和 tasks.json 中添加此文件。
//       g_running 定义在 main.cxx 中，此处仅通过 pmpc.h 获取 extern 声明，
//       不存在重复定义问题。
//=============================================================================

#include "pmpc.h"
#include "soe_queue.h"
#include <iostream>
#include <thread>

// ==================== 模拟采集子程序 ====================

void SubThread_SimCollect()
{
    auto& mgr = RemoteDataMgr::Instance();
    int cnt = 0;

    std::cout << "[采集子线程] 启动" << std::endl;

    while (g_running)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        if (!g_running) break;

        cnt++;

        // ★ 修复：每次循环刷新时间戳
        uint64_t ts = std::chrono::duration_cast<
            std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();

        // 写 DI 通道1设备1点位2（点号1固定为通讯状态，业务遥信从点2开始）
        mgr.SetDi(1, 1, 2, cnt % 2 == 0, ts, true);

        // 写 AI 通道1设备1点位2（递增模拟量变化）
        mgr.SetAi(1, 1, 2, cnt * 1.5);

        // DO 从站反馈值（周期性变化）
        mgr.SetDoSlave(1, 1, 1, cnt % 3 == 0);

        // AO 值（通道2设备1）
        mgr.SetAo(2, 1, 1, cnt * 10.0);

        // 模拟 SOE 事件（每 15 秒产生一次设备通讯状态变化）
        if (cnt % 15 == 0)
        {
            g_soeQueue.PushSimulated(1, 1, 1, cnt % 30 == 0 ? 0x02 : 0x01);
            if (cnt % 30 == 0)
                std::cout << "[采集子线程] SOE: 设备通讯故障 出现" << std::endl;
            else
                std::cout << "[采集子线程] SOE: 设备通讯故障 消失" << std::endl;
        }

        if (cnt % 10 == 0)
            std::cout << "[采集子线程] 已运行 " << cnt << " 秒" << std::endl;
    }

    std::cout << "[采集子线程] 退出" << std::endl;
}

// ==================== 模拟上位遥控子程序 ====================

void SubThread_HmiCtrl()
{
    auto& mgr = RemoteDataMgr::Instance();
    bool flag = false;

    std::cout << "[遥控子线程] 启动" << std::endl;

    while (g_running)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(3000));
        if (!g_running) break;

        // 翻转 DO 主站值（模拟遥控分合闸）
        mgr.SetDoMaster(1, 1, 1, flag);
        std::cout << "[遥控子线程] DO主站值 -> " << (flag ? "合" : "分")
                  << std::endl;
        flag = !flag;
    }

    std::cout << "[遥控子线程] 退出" << std::endl;
}
