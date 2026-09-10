# GPU SPH 粒子流体原型

这是一个独立于 CPU `sph-fluid-prototype` 的实验项目。密度/压力计算和积分都由 Vulkan Compute Shader 执行，粒子结果直接写入 `ParticleInstance` 兼容的 GPU vertex/storage buffer，再由引擎 Billboard 粒子管线绘制。

当前原型特性：

- 8192 个粒子，三份状态/密度/渲染 buffer 做环形复用；初始布局为 16×32×16 规则体积；
- 每个固定步包含 density/pressure 与 integrate 两个 compute pass；
- 保留正压 EOS、粘性、短程排斥、边界排斥和速度/加速度限制；
- 暂停/继续和重新播种由引擎的播放/暂停/停止生命周期控制，不占用键盘输入；
- 未播放时也会显示已播种的第一帧，但不会推进模拟；点击播放后才开始消耗游戏时间；
- 容器姿态由场景树中的 `GPU SPH Container` 实体 Transform 驱动；在编辑器或场景数据中修改该节点的旋转即可，不占用相机的 `WASD` 等输入；
- 容器由透明上部腔体、三级阶梯式收缩底部和窄观察段组成，台阶中心留有落料开口，六面保持封闭；粒子在容器局部坐标中积分，阶梯边界会将流体逐级引向底部观察段，旋转时将世界重力转换到局部坐标，因此可以观察粒子流向；
- 参考 `FluidSimulationDemo` 的 SPH 思路，但计算接口和资源生命周期接入 MikanEngine。

这是用于面试展示的 GPU 计算原型，邻居搜索仍是 O(N²)，8192 粒子会比 4096 粒子产生约 4 倍邻居检查开销；下一步优化方向是 GPU uniform grid / spatial hash，再进一步才是 screen-space fluid 表面重建。
