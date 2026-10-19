# 天空解码与光追显示链核对

## 可以确认的部分

独立宿主执行未经修改的 `vox_rt_realtime.comp`，它直接 include 原
`vox_rt_environment.glsl`。两者及 `atmo_sky.comp`、`vox_rt_composite.comp`
在已有渲染报告中的 SHA256 与工作区源码一致，生产 GLSL 没有改写。

| 环节 | 引擎 | 独立入口 |
| --- | --- | --- |
| 大气生产 | AtmosphereLUT + atmo_sky.comp | 相同实现与 Shader |
| 全景格式 | RGBA8 UNORM，LogLuv32 | 相同格式 |
| 全景尺寸 | AtmosphereRenderer 主线 128×64 | 最新入口 128×64；首张历史测试曾为 1024×512 |
| 纹理与 sampler | GetSkyImageView / GetSkySampler，内部为 GetSkyRTView / GetSkyRTSampler | 直接调用 GetSkyRTView / GetSkyRTSampler |
| binding 5 | panorama，shader-read-only | 相同用途及布局 |
| binding 7 | 天空 tint 和 intensity | 单位调制 (1,1,1,1)，不代表所有项目配置 |
| 海拔 | environment.altitudeMeters，主相机 y+200 | 测试相机 y+200 |
| 解码 | vox_rt_environment.glsl::skyBackground | 执行同一函数 |
| 主射线未命中 | 将解码天空写入 FP16 HDR | 同一 Shader 分支 |
| GI / GGX 逃逸场景 | 读取 skyBackground | 同一 Shader 分支 |
| 光照合成 | vox_rt_composite.comp | 同一 Shader |

需要注意，主射线未命中时不是把一张天空 LDR 图以 alpha 混入结果，
而是在光追 Shader 中采样并解码天空，直接写入线性 HDR 基底。
GI/反射中天空以环境辐射度参与传输，合成时再叠加材质调制后的信号。

`skyBackground` 的数学步骤是：采样编码值 v；计算
`Le=255*v.z+v.w`、`Y=exp2((Le-127)/2)`、`Zp=Y/v.y`、`Xp=v.x*Zp`；
乘原逆 LogLuv 矩阵得到线性 RGB，再施加天空 tint/intensity。
此处没有额外做一次 sRGB 解码或 gamma 编码。

## 此前没有对齐的显示出口

此前独立出口从 `tonemap.frag` 提取 ACES2 + pow(1/2.2) + Bayer。
它不能代表项目所用的纯 RT 显示链。
仓库另提供 `hardware_rt_present.frag`，使用
`rtLinearToSrgb(rtDisplayMap(hdr))`，其中 rtDisplayMap 为
`hdr / (1 + max(hdr.r,hdr.g,hdr.b))`，sRGB 编码采用标准分段函数，且不做 Bayer。

最新测试出口改为复用 `vox_rt_color.glsl` 中这两个函数，按原 RT present 的操作顺序执行。
旧图片和旧报告保留原样，其显示曲线记录仍为 ACES2。

但是，`RenderPureRayTracingView` 将 RT HDR 视图交给 `PostProcessChain::Execute`，
实际末端由项目/相机选择的后处理 JSON 决定。`UsesHardwareRayTracing()` 仅检测是否有
启用 Pass 的输入源为 hardware_rt，并不强制末端一定是 hardware_rt_present.frag。
仓库默认 postprocess_chain.json 没有 hardware_rt 输入，不能用它反推出用户实机的纯 RT 链。
因此“仓库提供的 RT present”与“用户实机实际选择的末端”不能直接画等号。

## 当前验证边界

天空解码源码、格式和绑定可以确认相同；没有同配置硬件 GPU 的 LUT、全景图及 HDR 读回，
不能确认 CPU/GPU 的实际数值完全相同，更不能据此将天空条纹归因于 CPU 模拟精度。
完整引擎 TAA/放大和项目后处理仍未整体复现。
之前 HDR 中已检测到天空亮度阶跃，说明最终曲线差异不能单独解释全部条纹。

新的 RT-present 对照输出在 `out/knight-540p-nrd-rtpresent/`。
它保留相同 960×540、2° 俯角、32 帧双信号 NRD，并采用上述明确的 RT present 显示函数。

## 修正显示出口后的实际结果

新的 960×540、32 帧 NRD 渲染已完成，原生总耗时 108.02 秒。
报告显示曲线为 `hardware_rt_present.frag: rtDisplayMap + rtLinearToSrgb`，Vulkan Validation 0 错误、0 警告，
两个 NRD 实例各连续处理 32 帧；天空对照区域在原始/NRD 同帧之间保持逐像素一致。
已目视检查新图，天空块状条纹仍然存在；切换正确的 RT present 函数没有消除这个问题。

最终图：`out/knight-540p-nrd-rtpresent/knight-nrd-last.png`。
报告：`out/knight-540p-nrd-rtpresent/complete-report.json`。
额外用正确的 hardware_rt_present.frag 构建输入重新生成并编译三个导出 Shader，
其生成源码和 SPIR-V 与本次实际执行的二进制逐字节一致。
