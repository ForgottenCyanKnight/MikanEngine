#pragma once
// AutosaveService.h - 编辑器场景定时快照（防意外丢失）
//
// 与"保存"刻意分离：
//   - 保存（工具栏 / Ctrl+S）→ 写项目场景文件（project.json 的 scene 字段）
//   - 自动快照（本服务）    → 只写引擎生成目录 <引擎根>/out/autosave/
// 快照永远不触碰项目路径下的场景文件，因此即便误操作或崩溃，磁盘上也始终存在
// 一份"没有被破坏性写入污染"的回退副本。
//
// 触发条件（全部满足才写盘）：编辑器模式 + 项目已加载 + 未播放 + 累计编辑时长
// 达到间隔 + 场景内容与上次快照不同。内容比对用序列化结果哈希，空闲时零写盘。
#include "Platform/Export.h"

#include <cstddef>
#include <string>

class MIKAN_API AutosaveService {
public:
    static AutosaveService& GetInstance();

    // 每帧调用。editable=false（游玩 / 项目管理器 / 纯游戏模式）时暂停计时，
    // 避免把运行期的临时场景状态写进快照。返回本帧是否真的写出了快照。
    bool Tick(double deltaSeconds, bool editable);

    // 立刻写一份快照（忽略间隔与内容比对），返回写出的路径；不可写时返回空。
    std::string WriteSnapshotNow();

    // 快照间隔（秒）。默认 5 分钟。
    void SetIntervalSeconds(double seconds);
    double GetIntervalSeconds() const { return m_intervalSeconds; }

    // 最近一次成功写出的快照路径（供界面/日志展示）。
    const std::string& GetLastSnapshotPath() const { return m_lastSnapshotPath; }
    // 距下一次快照还差多少秒可编辑时长。
    double GetSecondsUntilNextSnapshot() const;

private:
    AutosaveService() = default;
    ~AutosaveService() = default;
    AutosaveService(const AutosaveService&) = delete;
    AutosaveService& operator=(const AutosaveService&) = delete;

    // force=true 时跳过内容比对。
    std::string WriteSnapshot(bool force);

    // 轮转既有代次：slot N-1 丢弃，slot i-1 → slot i，为新的 slot 0 腾位。
    static void RotateGenerations(const std::string& snapshotDir,
                                 const std::string& baseName);

    // 保留的历史代次（含最新一份）。回退窗口 = kGenerationCount × 间隔。
    static constexpr int kGenerationCount = 3;
    static constexpr double kDefaultIntervalSeconds = 300.0;

    double m_intervalSeconds = kDefaultIntervalSeconds;
    double m_elapsedSeconds = 0.0;
    std::size_t m_lastSceneHash = 0;
    bool m_hasLastSceneHash = false;
    std::string m_lastSnapshotPath;
};
