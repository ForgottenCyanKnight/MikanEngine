# script_scaffold.ps1 - 为 Agent 生成或落盘玩法脚本
#
# 默认只生成使用 ECS::ScriptContext 的 C++ 脚本模板；-Source 可用于提交已经由
# Agent 生成的完整脚本。写入路径严格限制在 projects/<project>/games/，
# 并校验注册宏、允许的 include 与明显的进程/文件系统调用。
# 覆盖已有文件必须显式传 -Force，且会先写入 out/script_backups/<run-id>/。

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ScriptName,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath,

    [string]$ClassName = "",

    # JSON 数组：[{"name":"speed","type":"float","label":"速度","default":2.0}]
    [string]$FieldsJson = "[]",

    # MCP 使用项目内请求文件传递 JSON，避免 Windows 原生进程参数吞掉引号。
    [string]$FieldsPath = "",

    # 传入时直接使用完整源码；不传则根据 ScriptName/ClassName/FieldsJson 生成模板。
    [string]$Source = "",

    [string]$SourcePath = "",

    [switch]$Force,
    [switch]$Compile
)

$ErrorActionPreference = "Stop"
$root = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot)).TrimEnd('\')

function Fail([string]$message) {
    throw $message
}

function Get-Sha256([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return "" }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $stream = $null
    try {
        $stream = [System.IO.File]::OpenRead($Path)
        return ([System.BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant()
    } finally {
        if ($null -ne $stream) { $stream.Dispose() }
        $sha.Dispose()
    }
}

function Get-ProjectRelativePath([string]$fullPath) {
    $rootPrefix = $root + '\'
    if ($fullPath.Equals($root, [System.StringComparison]::OrdinalIgnoreCase)) {
        return ""
    }
    if (-not $fullPath.StartsWith($rootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        Fail "输出路径必须位于项目根目录内: $OutputPath"
    }
    return $fullPath.Substring($rootPrefix.Length).Replace('/', '\')
}

function Read-ProjectFile([string]$path, [string]$label) {
    if ([string]::IsNullOrWhiteSpace($path)) { return "" }
    $fullPath = if ([System.IO.Path]::IsPathRooted($path)) {
        [System.IO.Path]::GetFullPath($path)
    } else {
        [System.IO.Path]::GetFullPath((Join-Path $root $path))
    }
    [void](Get-ProjectRelativePath $fullPath)
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        Fail "$label 文件不存在: $fullPath"
    }
    return [System.IO.File]::ReadAllText($fullPath, [System.Text.UTF8Encoding]::new($false))
}

function ConvertTo-CppString([string]$value) {
    if ($null -eq $value) { return '""' }
    $escaped = $value.Replace('\', '\\').Replace('"', '\"')
    $escaped = $escaped.Replace("`r", '\r').Replace("`n", '\n').Replace("`t", '\t')
    return '"' + $escaped + '"'
}

function ConvertTo-NumberLiteral($value, [string]$type) {
    if ($null -eq $value) {
        if ($type -eq 'int') { return '0' }
        return '0.0f'
    }
    $number = 0.0
    if (-not [double]::TryParse([string]$value,
            [Globalization.NumberStyles]::Float,
            [Globalization.CultureInfo]::InvariantCulture,
            [ref]$number)) {
        Fail "字段默认值不是有效数字: $value"
    }
    $text = $number.ToString('R', [Globalization.CultureInfo]::InvariantCulture)
    if ($type -eq 'int') { return $text }
    if ($text -notmatch '[\.eE]') { $text += '.0' }
    return $text + 'f'
}

function ConvertTo-FieldDefault($field, [string]$type) {
    $hasDefault = $field.PSObject.Properties.Name -contains 'default'
    $value = if ($hasDefault) { $field.default } else { $null }
    switch ($type) {
        'bool' {
            if ($null -eq $value) { return 'false' }
            if ($value -is [bool]) { return ($(if ($value) { 'true' } else { 'false' })) }
            if ([string]$value -eq 'true' -or [string]$value -eq 'false') { return [string]$value }
            Fail "bool 字段默认值必须是 true/false: $($field.name)"
        }
        'int' { return ConvertTo-NumberLiteral $value 'int' }
        'float' { return ConvertTo-NumberLiteral $value 'float' }
        'string' {
            if ($null -eq $value) { return '""' }
            return ConvertTo-CppString ([string]$value)
        }
        'vec2' {
            $array = @($value)
            if ($array.Count -ne 2) { return 'glm::vec2(0.0f)' }
            return "glm::vec2($(ConvertTo-NumberLiteral $array[0] 'float'), $(ConvertTo-NumberLiteral $array[1] 'float'))"
        }
        'vec3' {
            $array = @($value)
            if ($array.Count -ne 3) { return 'glm::vec3(0.0f)' }
            return "glm::vec3($(ConvertTo-NumberLiteral $array[0] 'float'), $(ConvertTo-NumberLiteral $array[1] 'float'), $(ConvertTo-NumberLiteral $array[2] 'float'))"
        }
        default { Fail "不支持的字段类型: $type" }
    }
}

if ($ScriptName -notmatch '^[A-Za-z][A-Za-z0-9_]{0,63}$') {
    Fail "ScriptName 只能包含字母、数字和下划线，且必须以字母开头"
}
if ([string]::IsNullOrWhiteSpace($ClassName)) { $ClassName = $ScriptName }
if ($ClassName -notmatch '^[A-Za-z][A-Za-z0-9_]{0,63}$') {
    Fail "ClassName 只能包含字母、数字和下划线，且必须以字母开头"
}

$fullOutputPath = if ([System.IO.Path]::IsPathRooted($OutputPath)) {
    [System.IO.Path]::GetFullPath($OutputPath)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $root $OutputPath))
}
$relativeOutputPath = Get-ProjectRelativePath $fullOutputPath
if ([string]::IsNullOrWhiteSpace($relativeOutputPath)) {
    Fail "脚本输出路径不能为空: $relativeOutputPath"
}
$projectMatch = [System.Text.RegularExpressions.Regex]::Match(
    $relativeOutputPath, '^projects\\([^\\]+)\\games\\',
    [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
if (-not $projectMatch.Success) {
    Fail "脚本输出路径必须位于 projects/<project>/games/ 下: $relativeOutputPath"
}
$projectRoot = [System.IO.Path]::GetFullPath((Join-Path $root ("projects\\" + $projectMatch.Groups[1].Value)))
if (-not (Test-Path -LiteralPath (Join-Path $projectRoot 'project.json') -PathType Leaf)) {
    Fail "脚本输出所属项目缺少 project.json: $projectRoot"
}
if ([System.IO.Path]::GetExtension($fullOutputPath).ToLowerInvariant() -ne '.cpp') {
    Fail "脚本输出文件必须是 .cpp: $relativeOutputPath"
}

if ($FieldsPath) { $FieldsJson = Read-ProjectFile $FieldsPath 'FieldsPath' }
if ($SourcePath) { $Source = Read-ProjectFile $SourcePath 'SourcePath' }

try {
    $fields = @((ConvertFrom-Json -InputObject $FieldsJson))
} catch {
    Fail "FieldsJson 不是合法 JSON 数组: $($_.Exception.Message)"
}
if ($null -eq $fields) { $fields = @() }

$declarations = New-Object System.Text.StringBuilder
$fieldRows = New-Object System.Text.StringBuilder
$allowedTypes = @('bool', 'int', 'float', 'vec2', 'vec3', 'string')
foreach ($field in $fields) {
    if ($null -eq $field -or [string]::IsNullOrWhiteSpace([string]$field.name)) {
        Fail "每个字段都必须提供 name"
    }
    $fieldName = [string]$field.name
    $fieldType = ([string]$field.type).ToLowerInvariant()
    if ($fieldName -notmatch '^[A-Za-z][A-Za-z0-9_]{0,63}$') {
        Fail "字段名不合法: $fieldName"
    }
    if ($allowedTypes -notcontains $fieldType) {
        Fail "字段 $fieldName 的类型不支持: $fieldType（支持 bool/int/float/vec2/vec3/string）"
    }
    $label = if ($field.PSObject.Properties.Name -contains 'label' -and -not [string]::IsNullOrWhiteSpace([string]$field.label)) {
        [string]$field.label
    } else { $fieldName }
    $cppType = switch ($fieldType) {
        'bool' { 'bool' }
        'int' { 'int' }
        'float' { 'float' }
        'vec2' { 'glm::vec2' }
        'vec3' { 'glm::vec3' }
        'string' { 'std::string' }
    }
    $fieldToken = switch ($fieldType) {
        'bool' { 'Bool' }
        'int' { 'Int' }
        'float' { 'Float' }
        'vec2' { 'Vec2' }
        'vec3' { 'Vec3' }
        'string' { 'String' }
    }
    [void]$declarations.AppendLine("    $cppType $fieldName = $(ConvertTo-FieldDefault $field $fieldType);")
    [void]$fieldRows.AppendLine("    SCRIPT_FIELD($ClassName, $fieldName, $fieldToken, $(ConvertTo-CppString $label)),")
}

if ([string]::IsNullOrWhiteSpace($Source)) {
    $sourceTemplate = @'
// Generated by tools/script_scaffold.ps1
// Script SDK: ECS::ScriptContext v1
#include "ECS/ScriptContext.h"
#include "ECS/ScriptSystem.h"

#include <algorithm>
#include <glm/glm.hpp>

class __CLASS__ final : public ECS::IScriptBehaviour {
public:
    const char* GetScriptName() const override { return "__SCRIPT__"; }

    void OnStart(ECS::Entity entity) override {
        m_Context.SetSelf(entity);
        m_Context.Log("started");
    }

    void OnUpdate(float deltaTime) override {
        (void)deltaTime;
        if (!m_Context.IsAlive()) return;
        // TODO(Agent): 在这里实现玩法逻辑；优先使用 m_Context 的稳定接口。
    }

    void OnDestroy() override {
        m_Context.SetSelf(ECS::INVALID_ENTITY);
    }

    const ECS::FieldMeta* GetParamFields(int& outCount) const override {
        outCount = __FIELD_COUNT__;
        return outCount > 0 ? s_Fields : nullptr;
    }

__FIELDS__
private:
    static const ECS::FieldMeta s_Fields[];
    ECS::ScriptContext m_Context;
};

const ECS::FieldMeta __CLASS__::s_Fields[] = {
__FIELD_ROWS__
};

REGISTER_SCRIPT(__CLASS__, "__SCRIPT__");
'@
    $Source = $sourceTemplate.Replace('__CLASS__', $ClassName).Replace('__SCRIPT__', $ScriptName)
    $fieldRowsText = $fieldRows.ToString().TrimEnd()
    if ([string]::IsNullOrWhiteSpace($fieldRowsText)) {
        # C++ 不允许零长度数组；用一个 Hidden 哨兵保留稳定的静态数组布局，但不暴露给 Inspector。
        $fieldRowsText = '    { nullptr, nullptr, ECS::FieldType::Hidden, 0 },'
    }
    $Source = $Source.Replace('__FIELD_COUNT__', [string]$fields.Count)
    $Source = $Source.Replace('__FIELDS__', $declarations.ToString().TrimEnd())
    $Source = $Source.Replace('__FIELD_ROWS__', $fieldRowsText)
}

if ([string]::IsNullOrWhiteSpace($Source)) { Fail '源码不能为空' }
if ($Source.Length -gt 200000) { Fail '脚本源码超过 200000 字符限制' }
if ($Source -notmatch 'REGISTER_SCRIPT\s*\(' -or $Source -notmatch 'IScriptBehaviour') {
    Fail '脚本源码必须包含 IScriptBehaviour 和 REGISTER_SCRIPT(...)'
}

$forbidden = @(
    '(?i)\b(system|popen|_popen|_wsystem|WinExec|ShellExecute|CreateProcess)\s*\(',
    '(?i)#\s*include\s*[<"](?:windows\.h|winnt\.h|processthreadsapi\.h|shellapi\.h|fstream|filesystem|cstdio|cstdlib)[>"]'
)
foreach ($pattern in $forbidden) {
    if ($Source -match $pattern) { Fail "脚本源码包含受限调用或 include: $pattern" }
}

$allowedIncludes = @(
    'ECS/ScriptContext.h', 'ECS/ScriptSystem.h', 'ECS/Components.h', 'ECS/SceneECS.h',
    'ECS/Coordinator.h', 'Core/InputSystem.h', 'Core/Log.h', 'glm/glm.hpp',
    'glm/gtc/quaternion.hpp', 'glm/gtx/quaternion.hpp', 'algorithm', 'cmath',
    'cstdint', 'string', 'vector', 'array', 'unordered_map', 'utility', 'limits'
)
foreach ($match in [regex]::Matches($Source, '#\s*include\s*[<"]([^>"]+)[>"]')) {
    $includeName = $match.Groups[1].Value
    if ($allowedIncludes -notcontains $includeName) {
        Fail "脚本 include 不在允许列表中: $includeName"
    }
}

$existing = Test-Path -LiteralPath $fullOutputPath -PathType Leaf
$backupPath = $null
if ($existing -and -not $Force) {
    Fail "目标脚本已存在；如需覆盖请显式传 -Force: $relativeOutputPath"
}
if ($existing -and $Force) {
    $backupId = (Get-Date -Format 'yyyyMMdd_HHmmss_fff') + '-' + ([guid]::NewGuid().ToString('N').Substring(0, 8))
    $backupDir = Join-Path $root (Join-Path 'out\script_backups' $backupId)
    New-Item -ItemType Directory -Path $backupDir -Force | Out-Null
    $backupPath = Join-Path $backupDir ([System.IO.Path]::GetFileName($fullOutputPath))
    Copy-Item -LiteralPath $fullOutputPath -Destination $backupPath -Force
    $backupEntry = [ordered]@{
        originalPath = $relativeOutputPath
        timestamp = (Get-Date).ToString('o')
        size = (Get-Item -LiteralPath $fullOutputPath).Length
        sha256 = Get-Sha256 $fullOutputPath
    }
    $manifestPath = Join-Path $backupDir 'manifest.json'
    [System.IO.File]::WriteAllText($manifestPath,
        (([ordered]@{ operation = 'script-scaffold-force'; files = @($backupEntry) } | ConvertTo-Json -Depth 5)),
        [System.Text.UTF8Encoding]::new($false))
    if ((Get-Sha256 $backupPath) -ne $backupEntry.sha256) {
        Fail "覆盖前备份校验失败，已停止写入: $backupPath"
    }
}

$parentDir = Split-Path -Parent $fullOutputPath
New-Item -ItemType Directory -Path $parentDir -Force | Out-Null
[System.IO.File]::WriteAllText($fullOutputPath, $Source, [System.Text.UTF8Encoding]::new($true))
$writtenHash = Get-Sha256 $fullOutputPath
$writtenBytes = (Get-Item -LiteralPath $fullOutputPath).Length

$compileOutput = $null
$compileExit = $null
if ($Compile) {
    $compileScript = Join-Path $PSScriptRoot 'compile_games.ps1'
    if (-not (Test-Path -LiteralPath $compileScript -PathType Leaf)) { Fail "编译入口不存在: $compileScript" }
    $compileOutput = (& $compileScript -ProjectPath $projectRoot 2>&1 | Out-String).Trim()
    $compileExit = $LASTEXITCODE
    if ($compileExit -ne 0) { Fail "脚本写入成功，但插件编译失败（exit=$compileExit）`n$compileOutput" }
}

$result = [ordered]@{
    success = $true
    tool = 'script_scaffold'
    apiVersion = 1
    scriptName = $ScriptName
    className = $ClassName
    outputPath = $relativeOutputPath
    generated = [string]::IsNullOrWhiteSpace($PSBoundParameters['Source']) -and
                [string]::IsNullOrWhiteSpace($PSBoundParameters['SourcePath'])
    fieldCount = $fields.Count
    bytes = $writtenBytes
    sha256 = $writtenHash
    backupPath = $backupPath
    compile = [bool]$Compile
    compileExitCode = $compileExit
    compileOutput = if ($null -ne $compileOutput -and $compileOutput.Length -gt 8000) { $compileOutput.Substring($compileOutput.Length - 8000) } else { $compileOutput }
}
[Console]::Out.WriteLine(($result | ConvertTo-Json -Depth 8 -Compress))
