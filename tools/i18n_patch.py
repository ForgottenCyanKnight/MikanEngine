#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""补齐 en-US.json 缺失的 40 个 Tr key（2026-09-26 i18n_check 差集结果）。"""

import io
import json
import os

PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..',
                    'engine', 'i18n', 'en-US.json')

ADD = {
    "三角面: %s": "Triangles: %s",
    "上一次 UI 刷新以来的渲染统计（读时清零）。\n":
        "Render stats since the last UI refresh (cleared on read).\n",
    "个人开发者游戏引擎项目": "Indie game engine project",
    "关于 Mikan Engine": "About Mikan Engine",
    "启用三重缓冲以减少输入延迟。\n需要足够的GPU内存。":
        "Enable triple buffering to reduce input latency.\nRequires sufficient GPU memory.",
    "启用垂直同步以限制FPS为显示器刷新率。\n禁用以解除限制FPS渲染":
        "Enable VSync to cap FPS to the display refresh rate.\nDisable to uncap FPS rendering",
    "垂直同步": "VSync",
    "实例数: %s": "Instances: %s",
    "宽度##EngineResolution": "Width##EngineResolution",
    "宽度##ViewportResolution": "Width##ViewportResolution",
    "开发者": "Developer",
    "开发者：被遗忘的青色剑士": "Developer: ForgottenCyanKnight",
    "恢复默认": "Restore Defaults",
    "感谢使用 Mikan Engine。": "Thank you for using Mikan Engine.",
    "技术栈": "Tech Stack",
    "拖拽：%s": "Drag: %s",
    "持续开发中": "In active development",
    "搜索日志消息...": "Search log messages...",
    "显示对象": "Show Object",
    "暂无日志": "No logs yet",
    "模型实例: %s": "Model instances: %s",
    "模型种类: %s": "Model kinds: %s",
    "没有符合筛选条件的日志": "No logs match the filter",
    "滚轮缩放 / 中键平移": "Wheel zoom / middle-drag pan",
    "独占全屏绕开 Windows DWM 合成（窗口模式 present 平台税约 0.6-0.9ms）。\n切换后系统自动重建交换链。":
        "Exclusive fullscreen bypasses Windows DWM composition (~0.6-0.9ms present tax in windowed mode).\nThe system rebuilds the swap chain after switching.",
    "级别": "Level",
    "绘制调用: %s": "Draw calls: %s",
    "编辑器模式: 完整UI，离屏渲染\n游戏模式: 直接渲染到屏幕，更高性能":
        "Editor mode: full UI, offscreen rendering\nGame mode: renders directly to the screen, higher performance",
    "缩放：适应窗口": "Zoom: fit window",
    "范围：640x360 至 7680x4320；默认：1920x1040。":
        "Range: 640x360 to 7680x4320; default: 1920x1040.",
    "范围：640x360 至 7680x4320；默认：1920x1080。":
        "Range: 640x360 to 7680x4320; default: 1920x1080.",
    "负责引擎架构、渲染、编辑器和工具链的设计与实现。":
        "Responsible for engine architecture, rendering, editor and tooling.",
    "重命名": "Rename",
    "间接绘制(体素): %s": "Indirect draws (voxel): %s",
    "隐藏对象": "Hide Object",
    "面向个人开发的 C++ Vulkan 游戏引擎与编辑器。":
        "A C++ Vulkan game engine and editor for indie development.",
    "项目状态": "Project Status",
    "预览图片": "Preview Image",
    "高度##EngineResolution": "Height##EngineResolution",
    "高度##ViewportResolution": "Height##ViewportResolution",
}


def main():
    with io.open(PATH, encoding='utf-8') as f:
        data = json.load(f)
    added = 0
    for key, value in ADD.items():
        if key not in data:
            data[key] = value
            added += 1
    with io.open(PATH, 'w', encoding='utf-8') as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
        f.write('\n')
    print('added %d, dictionary now %d entries' % (added, len(data)))


if __name__ == '__main__':
    main()
