# 同步资源文件到 Android assets 目录（UTF-8 BOM 必需）
# 职责分离：engine/（引擎系统资产）拍平到根 + assets/（游戏内容）追加复制
# 不嵌套 engine/：运行时 GetEngineAssetPath 直接返回相对路径从 assets 根解析
# 全量重建：每次清空目标后同步，保证结果一致（引擎资产唯一、无残留）

$engineSourceDir = "$PSScriptRoot\..\engine"
$sourceDir = "$PSScriptRoot\..\assets"
$targetDir = "$PSScriptRoot\app\src\main\assets"

# 清空目标（全量重建）
Write-Host "Clearing $targetDir..." -ForegroundColor Yellow
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "$targetDir\*"
New-Item -ItemType Directory -Path $targetDir -Force | Out-Null

# 1) 引擎系统资产：拍平到 assets 根（fonts/shaders/textures/skybox/postprocess 直接放根）
#    注：/XD 用纯目录名（相对子路径如 shaders\glsl 不被 robocopy 匹配）
Write-Host "Syncing engine assets from $engineSourceDir to $targetDir (flattened)..." -ForegroundColor Green
robocopy $engineSourceDir $targetDir /MIR /XD .git glsl /XF *.tmp

# 2) 游戏内容：追加复制（/E 递归、不清理目标），排除桌面测试大资产
Write-Host "Syncing game content from $sourceDir to $targetDir (append, filtered)..." -ForegroundColor Green
robocopy $sourceDir $targetDir /E `
    /XD .git Bistro_v5_2 engine_20260814_pre_csm 2.0 MetalRoughSpheres IDKEngine `
    /XF *.tmp

if ($LASTEXITCODE -le 7) {
    Write-Host "Assets synced successfully!" -ForegroundColor Green
} else {
    Write-Error "Failed to sync assets"
}
