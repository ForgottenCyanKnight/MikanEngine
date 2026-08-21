#pragma once
// UndoManager.h - 编辑器撤销/重做(快照栈方案)
// 每帧由 EditorDllApi 调用 UpdateFrameDetection():序列化当前场景与上帧比较,
// 检测到"从稳定变为变化"时把上帧状态(操作前)压入撤销栈;连续变化(如拖动中)
// 自动合并为一步。Ctrl+Z 撤销 / Ctrl+Y 重做:弹出快照并 DeserializeScene 恢复。
// 上限 kMaxUndo 步;栈满移除最旧。撤销/重做均为内存操作,不落盘。
#include <string>
#include <vector>

namespace Editor {

class UndoManager {
public:
    static UndoManager& GetInstance();

    // 每帧调用:检测场景变化并记录撤销点(自动合并连续帧变更)。
    // 性能: 全场景序列化开销随实体数增长, 节流到每 kDetectInterval 帧检测一次
    // (撤销粒度 = 该间隔内的变更合并为一步; 打砖块 60+ 实体时从每帧 1ms 降到 ~0.25ms)
    void UpdateFrameDetection();
    static constexpr int kDetectInterval = 4;

    // 撤销录制开关: 游戏运行态(播放)应关闭,停止后恢复。
    // 关闭时 UpdateFrameDetection 直接跳过(不序列化,不记录);
    // 恢复时重置帧间基线,避免把运行期间的场景变化误记为操作。
    void SetRecordingEnabled(bool enabled);
    bool IsRecordingEnabled() const { return m_recordingEnabled; }

    // 撤销一步:当前状态入重做栈,恢复撤销栈顶快照。成功返回 true。
    bool Undo();
    // 重做一步:当前状态入撤销栈,恢复重做栈顶快照。成功返回 true。
    bool Redo();

    bool CanUndo() const { return !m_undoStack.empty(); }
    bool CanRedo() const { return !m_redoStack.empty(); }
    void Clear() { m_undoStack.clear(); m_redoStack.clear(); }
    size_t GetUndoCount() const { return m_undoStack.size(); }

private:
    UndoManager() = default;
    ~UndoManager() = default;
    UndoManager(const UndoManager&) = delete;
    UndoManager& operator=(const UndoManager&) = delete;

    void PushState(const std::string& preState); // 显式入栈(去重 + 清空重做栈)

    std::vector<std::string> m_undoStack; // 快照 JSON(操作前的场景状态)
    std::vector<std::string> m_redoStack; // 被撤销的状态(供重做)
    static constexpr size_t kMaxUndo = 50;
    bool m_recordingEnabled = true;       // 运行态(播放)时关闭撤销录制

    // 帧间场景比较缓存
    std::string m_prevScene;    // 上一帧场景
    std::string m_prevPrevScene; // 上上帧场景(判断是否连续变化)
    bool m_suppress = true;     // 撤销/恢复后置位:下一帧重置基线,不记录
};

} // namespace Editor
