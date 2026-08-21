# validate_scene.ps1 - 场景 JSON 离线校验（AI 写场景后的即时纠错）
# ------------------------------------------------------------------
# 用引擎导出的 schema（tools/scene_schema.json，由 --dump-schema 生成，组件变更后重新生成）
# 静态检查场景文件，无需启动引擎。
#
# 用法（项目根执行）：
#   powershell -NoProfile -File tools\validate_scene.ps1 assets\contact2d.json
#   powershell -NoProfile -File tools\validate_scene.ps1 my_scene.json -Schema tools\scene_schema.json -CheckAssets
#
# 退出码：0 = 通过（可能有警告），1 = 有错误
#
# 检查项：
#   1. JSON 可解析；顶层 entities 数组必需
#   2. 实体 id 唯一/整数；name 组件必需
#   3. transform 结构（position/scale 3 元、rotation 4 元）
#   4. hierarchy.parent 引用存在的实体（4294967295 = 无父）
#   5. 组件键 ∈ schema serializeKey 白名单；字段 ∈ 反射字段表（反射表非空时）
#   6. -CheckAssets：String 资源字段（texture/textureName/modelPath/voxPath/tmxPath…）指向的文件存在
# ------------------------------------------------------------------
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)][string]$ScenePath,
    [string]$Schema = "",
    [switch]$CheckAssets
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $Schema) { $Schema = Join-Path $PSScriptRoot "scene_schema.json" }

$script:errors = @()
$script:warns = @()
function Add-Err([string]$msg) { $script:errors += "[ERROR] $msg" }
function Add-Warn([string]$msg) { $script:warns += "[WARN]  $msg" }

# ---- 0. 读 schema ----
if (-not (Test-Path $Schema)) {
    Write-Host "[ERROR] schema 不存在: $Schema（先运行: EngineMain.exe --dump-schema tools\scene_schema.json）"
    exit 1
}
$schemaJson = [System.IO.File]::ReadAllText($Schema, [System.Text.UTF8Encoding]::new($false)) | ConvertFrom-Json
$serializeKeys = @{}   # serializeKey -> @{ fieldName = fieldType }
foreach ($c in $schemaJson.components) {
    if ($c.serializeKey) {
        $serializeKeys[$c.serializeKey] = @{}
        foreach ($fd in $c.fields) { $serializeKeys[$c.serializeKey][$fd.name] = $fd.type }
    }
}
$baseKeys = @("id", "name", "transform", "hierarchy")   # 基础键单独处理
$assetFields = @("texture", "textureName", "modelPath", "voxPath", "tmxPath", "tilemapFile", "textureOverride", "font")

# ---- 1. 读场景 ----
if (-not (Test-Path $ScenePath)) { Write-Host "[ERROR] 场景文件不存在: $ScenePath"; exit 1 }
$raw = [System.IO.File]::ReadAllText($ScenePath, [System.Text.UTF8Encoding]::new($false))
try { $scene = $raw | ConvertFrom-Json }
catch { Write-Host "[ERROR] JSON 解析失败: $($_.Exception.Message)"; exit 1 }

if ($null -eq $scene.entities) {
    Add-Err "缺少顶层 'entities' 数组"
    Write-Host ($script:errors -join "`n"); Write-Host "[VALIDATE] ${ScenePath}: $($script:errors.Count) 错误"; exit 1
}
if ($null -ne $scene.game -and $scene.game -isnot [string]) { Add-Err "'game' 必须是字符串" }
foreach ($k in @($scene.PSObject.Properties.Name)) {
    if ($k -notin @("game", "entities")) { Add-Warn "未知顶层键: '$k'（合法: game, entities）" }
}

# ---- 2. 实体 id 收集（引用检查用）----
$ids = @{}
foreach ($e in $scene.entities) {
    $idNum = [int64]0
    if ($null -eq $e.id -or -not [int64]::TryParse([string]$e.id, [ref]$idNum)) {
        Add-Err "实体缺少整数 id"; continue
    }
    if ($ids.ContainsKey($idNum)) { Add-Err "实体 id 重复: $idNum" } else { $ids[$idNum] = $e }
}

# ---- 3. 逐实体检查 ----
foreach ($e in $scene.entities) {
    $idNum = [int64]0
    if (-not [int64]::TryParse([string]$e.id, [ref]$idNum)) { continue }   # id 错误已在上面报
    $ek = @($e.PSObject.Properties.Name)

    # name 组件
    if (-not $ek.Contains("name")) { Add-Err "实体 $idNum 缺少 'name' 组件" }
    elseif ($null -eq $e.name.name -or $e.name.name -isnot [string]) { Add-Err "实体 $idNum 的 name 组件缺 'name' 字符串字段（格式: {""name"": {""name"": ""xxx""}}）" }

    # transform 结构
    if ($ek.Contains("transform")) {
        $t = $e.transform
        foreach ($req in @("position", "rotation", "scale")) {
            if ($null -eq $t.$req) { Add-Err "实体 $idNum 的 transform 缺 '$req'" }
            elseif ($t.$req -isnot [array]) { Add-Err "实体 $idNum 的 transform.$req 必须是数组" }
            else {
                $len = @($t.$req).Count
                $exp = if ($req -eq "rotation") { 4 } else { 3 }
                if ($len -ne $exp) { Add-Err "实体 $idNum 的 transform.$req 长度 $len != $exp" }
            }
        }
    } else {
        Add-Err "实体 $idNum 缺少 'transform' 组件"
    }

    # hierarchy.parent 引用
    if ($ek.Contains("hierarchy") -and $null -ne $e.hierarchy.parent) {
        $pNum = [int64]0
        if ([int64]::TryParse([string]$e.hierarchy.parent, [ref]$pNum)) {
            if ($pNum -ne 4294967295 -and -not $ids.ContainsKey($pNum)) {
                Add-Err "实体 $idNum 的 hierarchy.parent=$pNum 引用不存在的实体"
            }
        }
    }

    # 组件键白名单 + 字段白名单
    foreach ($k in $ek) {
        if ($k -in $baseKeys) { continue }
        if (-not $serializeKeys.ContainsKey($k)) {
            Add-Err "实体 $idNum 含未知组件键 '$k'（schema 白名单: $(($serializeKeys.Keys | Sort-Object) -join ', ')）"
            continue
        }
        $fields = $serializeKeys[$k]
        if ($fields.Count -gt 0 -and $null -ne $e.$k) {
            foreach ($fk in @($e.$k.PSObject.Properties.Name)) {
                if (-not $fields.ContainsKey($fk)) {
                    Add-Err "实体 $idNum 的 '$k' 组件含未知字段 '$fk'（合法: $(($fields.Keys | Sort-Object) -join ', ')）"
                }
            }
        }
    }

    # 资源引用（可选）
    if ($CheckAssets) {
        foreach ($k in $ek) {
            if (-not $serializeKeys.ContainsKey($k)) { continue }
            foreach ($fk in @($serializeKeys[$k].Keys)) {
                if ($fk -notin $assetFields) { continue }
                $val = $e.$k.$fk
                if ($null -eq $val -or [string]$val -eq "") { continue }
                $candidates = @(
                    (Join-Path $root ([string]$val).TrimStart('/')),
                    (Join-Path (Join-Path $root "assets") ([string]$val).TrimStart('/'))
                )
                if (-not ($candidates | Where-Object { Test-Path $_ })) {
                    Add-Warn "实体 $idNum 的 '$k.$fk' 引用可能不存在的资源: '$val'（检查过: $($candidates -join ' / ')）"
                }
            }
        }
    }
}

# ---- 4. 输出 ----
if ($script:errors.Count -gt 0) {
    Write-Host ($script:errors -join "`n")
    Write-Host ""
    Write-Host "[VALIDATE] ${ScenePath}: $($script:errors.Count) 错误, $($script:warns.Count) 警告 —— 未通过"
    foreach ($w in $script:warns) { Write-Host $w }
    exit 1
}
Write-Host "[VALIDATE] ${ScenePath}: 通过（$($scene.entities.Count) 实体, $($script:warns.Count) 警告）"
foreach ($w in $script:warns) { Write-Host $w }
exit 0
