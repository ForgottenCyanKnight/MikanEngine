# VOX 减面统计实测

测试使用引擎原有 `VoxFormat::LoadVoxFile`、`BakeVoxSurfaces` 和
`VoxSurfaceReductionStats`，在 Linux CPU 上完成，不创建 Vulkan 设备。
材质打包函数提取到共享头文件，渲染器和离线工具使用同一实现。

## 模型与结果

模型来自 [ephtracy/voxel-model](https://github.com/ephtracy/voxel-model)，固定提交
`3bff8feaa7d94a6237f2c07dc4ca3c3d268ef06c`，采用 MIT 许可。原始文件、许可证、
来源 URL 和 SHA-256 保存在 `tests/fixtures/vox-mit/`，后续运行无需联网。

| 模型 | 体素数 | 暴露单位面 | 普通合并四边形 | 接触回填后四边形 | 最终减面率 | 三角形数 |
|---|---:|---:|---:|---:|---:|---:|
| 骑士 chr_knight | 398 | 730 | 398 | 313 | 57.12% | 626 |
| 茶壶 teapot | 28,411 | 55,964 | 22,410 | 21,404 | 61.75% | 42,808 |
| 龙 dragon | 40,265 | 78,290 | 34,949 | 32,428 | 58.58% | 64,856 |
| Menger 分形 | 160,000 | 336,384 | 179,724 | 137,988 | 58.98% | 275,976 |

减面率按引擎定义：`1 - 最终四边形数 / 暴露单位面数`。
基准已经去掉相邻实心体素之间的内部面，不是 `体素数 × 6`。

当前普通合并允许不同颜色共用四边形，通过属性数据保留每个单位面的颜色和材质。
骑士若限定同色合并需要 506 个四边形；启用现有跨颜色合并后是 398 个，再经过
接触回填后为 313 个。因此这里只比较几何数量，不能据此断言显存或帧率同比改善。

接触回填仅在两个相邻体素均为普通不透明材质时延展隐藏面，并且仅在候选四边形更少时选用。
四个模型均选择了回填结果；加入的隐藏单位面分别为 532、47,017、63,871、252,942。
这是减少矩形碎片的代价，隐藏区域不计入原始暴露单位面基准。

## 验证内容

- Python 独立解析原始 VOX 文件，以 `6×体素数 - 2×相邻体素对数` 核对暴露面积。
- C++ 将每个合并四边形重新展开，逐个方向核对所有原始暴露单位面：没有漏面或重复覆盖。
- 解码最终属性数据，逐面验证 RGB 和材质编码一致。
- 回填面必须位于两个普通不透明体素之间，不能跨越真实空洞；核对隐藏面统计值。
- 验证发光面几何及材质在回填前后保持一致。
- 合成用例覆盖空模型、单体素、2×2×2 实心体、异色合并、发光/玻璃屏障、接触柱、
  空心体、256 单元编码边界和按实例加权累计。
- 公开模型以 SHA-256 固定，并保存已验证的面数作为回归基准。

工具构建单位面输入后，调用与渲染器相同的生产减面函数。这里验证 CPU 烘焙和统计；
没有执行 `VoxRenderer` 的缓存命中、GPU 上传、绘制和运行时剔除路径。
测试采用普通静态 VOX 的跨颜色合并与接触回填配置，不覆盖实体级强制统一材质/发光的设置。
报告中的 `analysis_ms` 包含三种烘焙及独立审计，不是纯减面耗时或 GPU 性能指标。

## 复现

按 [Linux 构建说明](LINUX.md) 构建 Linux CPU 目标后，在仓库根目录运行：

```sh
./out/build/linux-cpu/MikanVoxModelStats \
  tests/fixtures/vox-mit/chr_knight.vox \
  tests/fixtures/vox-mit/teapot.vox \
  tests/fixtures/vox-mit/dragon.vox \
  tests/fixtures/vox-mit/menger.vox \
  --output out/build/linux-cpu/vox-models-report.json

ctest --test-dir out/build/linux-cpu -R 'vox_model_surface_contracts|vox_public_models' --output-on-failure
```

分析自己的模型：

```sh
./out/build/linux-cpu/MikanVoxModelStats /absolute/path/model.vox --output report.json
```

完整回归已扩展为 14 项；详细结果见构建目录中的 `results.xml`、
`vox-models-report.json` 和 `Testing/Temporary/LastTest.log`。
