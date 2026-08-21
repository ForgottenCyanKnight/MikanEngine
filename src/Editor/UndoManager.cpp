// UndoManager.cpp - 编辑器撤销/重做(快照栈方案)
// 快照 = SceneSerializer::SerializeScene() 序列化的场景 JSON 字符串(内存,不落盘)。
// 注意:恢复快照会重建全部实体(实体句柄变化),故撤销/重做后清空选中实体。
#include "Editor/UndoManager.h"
#include "SceneSerializer.h"
#include "ECS/SceneECS.h"

#include <iostream>

namespace Editor {

UndoManager& UndoManager::GetInstance() {
    static UndoManager instance;
    return instance;
}

void UndoManager::PushState(const std::string& preState) {
    if (m_undoStack.empty() || m_undoStack.back() != preState) {
        m_undoStack.push_back(preState);
        if (m_undoStack.size() > kMaxUndo)
            m_undoStack.erase(m_undoStack.begin()); // 淘汰最旧
        m_redoStack.clear(); // 新操作使重做分支失效
    }
}

void UndoManager::UpdateFrameDetection() {
    // 运行态(播放)不检测: 游戏每帧位移/实体变化不属于编辑器操作, 也省掉序列化开销
    if (!m_recordingEnabled) return;

    // 节流: 全场景序列化开销大, 每 kDetectInterval 帧检测一次
    static int s_frameCounter = 0;
    if ((++s_frameCounter % kDetectInterval) != 0) return;

    ECS::SceneSerializer serializer;
    std::string cur = serializer.SerializeScene();

    if (m_suppress) {
        // 撤销/重做/加载后:重置基线,避免把恢复动作误记为操作
        m_prevScene = cur;
        m_prevPrevScene = cur;
        m_suppress = false;
        return;
    }

    // 场景从"稳定"变为"变化" → 上帧状态即操作前状态,入撤销栈。
    // 连续变化(上一帧也在变,如拖动中)不重复入栈 → 一次拖动合并为一步。
    if (cur != m_prevScene && m_prevScene == m_prevPrevScene) {
        PushState(m_prevScene);
    }

    m_prevPrevScene = m_prevScene;
    m_prevScene = cur;
}

void UndoManager::SetRecordingEnabled(bool enabled) {
    if (m_recordingEnabled == enabled) return;
    m_recordingEnabled = enabled;
    if (enabled) {
        // 恢复记录: 重置帧间基线,避免把运行期间的变化误记为操作
        m_prevScene.clear();
        m_prevPrevScene.clear();
        m_suppress = true; // 下一帧以当前场景为基线
        std::cout << "[UndoManager] Recording resumed" << std::endl;
    } else {
        std::cout << "[UndoManager] Recording paused (game running)" << std::endl;
    }
}

bool UndoManager::Undo() {
    if (m_undoStack.empty()) return false;

    ECS::SceneSerializer serializer;
    // 当前状态进重做栈
    m_redoStack.push_back(serializer.SerializeScene());

    // 恢复撤销栈顶快照
    std::string target = std::move(m_undoStack.back());
    m_undoStack.pop_back();
    if (!serializer.DeserializeScene(target)) {
        m_redoStack.pop_back();
        std::cerr << "[UndoManager] Undo failed: snapshot could not be restored" << std::endl;
        return false;
    }
    // 恢复后实体全部重建,旧选中句柄失效
    ECS::SceneECS::GetInstance().SetSelectedEntity(ECS::INVALID_ENTITY);
    m_suppress = true; // 下一帧重置帧间缓存
    std::cout << "[UndoManager] Undo (" << m_undoStack.size() << " left)" << std::endl;
    return true;
}

bool UndoManager::Redo() {
    if (m_redoStack.empty()) return false;

    ECS::SceneSerializer serializer;
    // 当前状态进撤销栈
    m_undoStack.push_back(serializer.SerializeScene());

    std::string target = std::move(m_redoStack.back());
    m_redoStack.pop_back();
    if (!serializer.DeserializeScene(target)) {
        m_undoStack.pop_back();
        std::cerr << "[UndoManager] Redo failed: snapshot could not be restored" << std::endl;
        return false;
    }
    ECS::SceneECS::GetInstance().SetSelectedEntity(ECS::INVALID_ENTITY);
    m_suppress = true;
    std::cout << "[UndoManager] Redo (" << m_redoStack.size() << " left)" << std::endl;
    return true;
}

} // namespace Editor
