# ktx2_convert.ps1 — PNG → KTX2 批量转换
# 用法:
#   powershell -ExecutionPolicy Bypass -File ktx2_convert.ps1 -InputPath "assets\models" -Quality uastc
#   powershell -ExecutionPolicy Bypass -File ktx2_convert.ps1 -InputPath "tex.png" -Quality rdo
# 说明:
#   - 输出同名 .ktx2 到源目录（引擎自动优先加载同名 .ktx2）
#   - Quality: uastc(高质量) / rdo(折中,移动端推荐) / etc1s(小文件,粗糙)
#   - 无需处理 Y 轴翻转（引擎加载时自动翻转）
param(
    [Parameter(Mandatory = $true)][string]$InputPath,
    [ValidateSet("uastc", "rdo", "etc1s")][string]$Quality = "uastc",
    [switch]$NoMip,
    [ValidateSet("srgb", "unorm")][string]$ColorSpace = "srgb",
    [string]$OutDir = ""
)

$ktx = Join-Path $PSScriptRoot "ktx\bin\ktx.exe"
if (-not (Test-Path $ktx)) {
    Write-Error "ktx.exe 未找到: $ktx"
    exit 1
}

# 收集输入文件（目录则递归取 .png）
$files = @()
if (Test-Path $InputPath -PathType Container) {
    $files = Get-ChildItem -Path $InputPath -Recurse -Filter "*.png" -File
} elseif (Test-Path $InputPath -PathType Leaf) {
    $files = @(Get-Item $InputPath)
} else {
    Write-Error "输入不存在: $InputPath"
    exit 1
}

if ($files.Count -eq 0) {
    Write-Host "没有找到 .png 文件"
    exit 0
}

$fmt = if ($ColorSpace -eq "srgb") { "R8G8B8A8_SRGB" } else { "R8G8B8A8_UNORM" }
$encode = switch ($Quality) {
    "uastc"  { @("--encode", "uastc") }
    "rdo"    { @("--encode", "uastc", "--uastc-rdo", "--uastc-rdo-l", "0.5") }
    "etc1s"  { @("--encode", "basis-lz") }
}
$mip = @("--mipmap-filter", "lanczos4")
if (-not $NoMip) { $mip = @("--generate-mipmap", "--mipmap-filter", "lanczos4") }

$ok = 0; $fail = 0
foreach ($f in $files) {
    $outName = [System.IO.Path]::ChangeExtension($f.Name, ".ktx2")
    $outPath = if ($OutDir) { Join-Path $OutDir $outName } else { Join-Path $f.DirectoryName $outName }
    $args = @("create", "--format", $fmt) + $encode + $mip + @("`"$($f.FullName)`"", "`"$outPath`"")
    Write-Host "转换: $($f.Name) -> $outName  [$Quality$(if(-not $NoMip){' +mip'})]"
    & $ktx @args 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0 -and (Test-Path $outPath)) {
        $mb = [math]::Round((Get-Item $outPath).Length / 1MB, 1)
        Write-Host "  OK  ($mb MB)"
        $ok++
    } else {
        Write-Host "  失败!"
        $fail++
    }
}

Write-Host ""
Write-Host "完成: 成功 $ok, 失败 $fail"
if ($fail -gt 0) { exit 1 } else { exit 0 }
