# KTX2 素材生成指南

> 引擎已集成 KTX2 + BasisU：同名 `.ktx2` 存在时自动加载压缩纹理（桌面转 BC7 / 真机转 ASTC），
> 无需改代码。本指南教你自己把 PNG 转成 ktx2。

## 0. 工具位置

转换工具已随项目自带（无需安装）：

```
D:\Engine project\vulkan engine\third_party\ktx\bin\ktx.exe
```

## 1. 一条命令转换（复制即用）

```bat
"D:\Engine project\vulkan engine\third_party\ktx\bin\ktx.exe" create ^
  --format R8G8B8A8_SRGB --encode uastc --generate-mipmap ^
  input.png output.ktx2
```

- `--format R8G8B8A8_SRGB`：颜色纹理（贴图/漫反射）用 **SRGB**；
  **数据纹理**（法线贴图、LUT、遮罩）用 `R8G8B8A8_UNORM`（不做 gamma 解码）
- `--encode uastc`：高质量编码（BC7 级保真，推荐）
- `--generate-mipmap`：内嵌 mip 链（远处清晰，推荐）
- **不要加** `--assign-tf linear`（会改颜色）；**不要用** `--codec`（create 的正确参数是 `--encode`）

## 2. 三种质量档（按需求选）

| 档位 | 命令参数 | baka 8192² 文件大小 | 质量 | 适用 |
|------|---------|-------------------|------|------|
| 高质量 | `--encode uastc` | ~85MB | 近原图 | 角色/主材质、在意画质 |
| 折中 | `--encode uastc --uastc-rdo --uastc-rdo-l 0.5` | ~20-40MB | 好（略降） | **移动端推荐** |
| 小文件 | `--encode basis-lz` | ~6MB | 粗糙（块状伪影） | UI/小纹理/不敏感场合 |

> 运行时显存（转码后）三种档位一样：桌面 BC7 4:1、真机 ASTC 4:1。
> 差异只在**文件大小**（加载带宽）和**转码后画质**。

## 3. 放置规则（重要）

**同名放旁边，自动生效**：

```
assets/models/baka.png   ← 原始（可删可留）
assets/models/baka.ktx2  ← 转换产物（引擎自动优先加载这个）
```

- 加载时探测 `同名.ktx2`（如 `baka.png → baka.ktx2`），存在就走压缩路径
- 想回退原图：删掉 `.ktx2` 即可
- **无需处理 Y 轴翻转**（引擎加载时自动翻转，`FlipKtx2Vertically` 已内置）

## 3b. glTF + KTX2 素材（零改动直接可用）

引擎 ModelLoader 会自动解析 glTF 的纹理引用（images/textures/materials，含 `KHR_texture_basisu` 扩展），
**无需同目录 png、无需改 gltf 文件**。示例（IDKEngine 素材已在 `assets/models/IDKEngine/` 验证）：

```
Sponza.gltf + Sponza.bin + 123456.ktx2 × N   ← 直接放，引擎自动读纹理（走压缩）
```

- assimp 不认 .ktx2 引用 → 引擎用内置 JsonLite 解析 glTF json 补上纹理路径
- 材质索引与 glTF materials 顺序对齐（Sponza 25 材质验证通过）
- 已注入槽位：diffuse / normal / roughness / metallic（ao/emissive 暂不注入模型渲染路径）

## 4. 批量转换脚本（推荐）

项目自带脚本，一条命令转整个目录：

```powershell
# 转单个文件或整个目录，同名 .ktx2 输出到源目录
powershell -ExecutionPolicy Bypass -File "D:\Engine project\vulkan engine\tools\ktx2_convert.ps1" `
  -InputPath "D:\Engine project\vulkan engine\assets\models" -Quality uastc
```

参数：

| 参数 | 说明 | 默认 |
|------|------|------|
| `-InputPath` | PNG 文件或目录（必填） | — |
| `-Quality` | `uastc` / `rdo` / `etc1s` | `uastc` |
| `-NoMip` | 不生成 mip（默认生成） | 关 |
| `-ColorSpace unorm` | 数据纹理用（默认 srgb） | `srgb` |
| `-OutDir` | 输出目录（默认与源同目录） | — |

## 5. 常见问题

- **转换后模型上下颠倒** → 不会（引擎已内置翻转）；若出现说明用了旧版引擎，更新后重试
- **颜色发暗/发灰** → 转的时候加了 `--assign-tf linear`，去掉重转
- **很粗糙（块状）** → 用了 `basis-lz`（ETC1S），换 `uastc`
- **远处模糊** → 没加 `--generate-mipmap`，加上
- **非 2 的幂尺寸** → ktx 会自动填充到块对齐（4 的倍数），可以转；但 2 的幂最省显存
