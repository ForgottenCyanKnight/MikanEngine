# 内置 ReSTIR

Enhanced 是唯一内置 ReSTIR 实现。架构、参数、资源合同及移植边界见
[ReSTIR_PT_Enhanced.md](ReSTIR_PT_Enhanced.md)。

2026-10-11 清理了旧 DI/GI 实验、PSS 整路径重放对照及 RTXDI SDK 分支。
控制面板保留 NEE/MIS 基线与 ReSTIR Enhanced；启动 Enhanced 使用
`MIKAN_HWRT_RESTIR_PT=1`。旧分支不再作为回退；资源或管线不可用时回退 NEE。

早期性能对照保存在 `out/enhanced_port_20261011/REPORT.md`，其中 PSS 是历史
版本，已不能在当前运行时选择。辐照度缓存与降噪/重建独立于 ReSTIR。
