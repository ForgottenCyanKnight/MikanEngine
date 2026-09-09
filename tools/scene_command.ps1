# scene_command.ps1 - MikanEngine 结构化场景命令执行器
# ------------------------------------------------------------------
# 目标：
#   1) 给 Agent 一个受约束的场景编辑接口，而不是让 Agent 直接重写整份 JSON；
#   2) 默认输出副本，先做离线校验，再提交到目标路径；
#   3) 可选调用统一回归入口，对生成场景跑玩法或 Vulkan 渲染测试；
#   4) 每次执行保留 commands、候选场景、校验日志、回归结果和修改前备份。
#
# 用法：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\scene_command.ps1 -ScenePath projects\third-person-navigation\scenes\main.json -CommandsPath tools\scene_command.example.json -TestLayer gameplay -SkipTestBuild
#
# 默认行为：输出到 out\scene_commands\<run-id>\scene.generated.json，不覆盖源场景。
# 原地更新必须显式增加 -InPlace；覆盖前会保存 source.before.json。
# ------------------------------------------------------------------
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ScenePath,
    [Parameter(Mandatory = $true)][string]$CommandsPath,
    [string]$OutputPath = "",
    [switch]$InPlace,
    [switch]$CheckAssets,
    [ValidateSet("none", "validate", "gameplay", "render", "all")]
    [string]$TestLayer = "none",
    [switch]$SkipTestBuild,
    [int]$TestTimeoutSeconds = 120,
    [int]$BuildTimeoutSeconds = 600
)

$ErrorActionPreference = "Stop"
$root = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$schemaPath = Join-Path $PSScriptRoot "scene_schema.json"
$validateScript = Join-Path $PSScriptRoot "validate_scene.ps1"
$testScript = Join-Path $PSScriptRoot "test.ps1"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$sentinelParent = [int64]4294967295
$runId = (Get-Date -Format "yyyyMMdd-HHmmssfff") + "-" + ([Guid]::NewGuid().ToString("N").Substring(0, 8))
$runDir = Join-Path $root (Join-Path "out\scene_commands" $runId)
New-Item -ItemType Directory -Path $runDir -Force | Out-Null
$runLogPath = Join-Path $runDir "run.log"
$resultPath = Join-Path $runDir "result.json"
[System.IO.File]::WriteAllText($runLogPath, "", $utf8NoBom)

$script:operations = New-Object 'System.Collections.Generic.List[object]'
$script:entities = New-Object 'System.Collections.Generic.List[object]'
$script:aliases = @{}
$script:allowedComponents = @{}
$script:sourcePath = ""
$script:finalPath = ""
$script:stagePath = Join-Path $runDir "candidate.json"

function Write-RunMessage([string]$Message) {
    $line = "$(Get-Date -Format o) $Message"
    Write-Host $line
    [System.IO.File]::AppendAllText($runLogPath, $line + [System.Environment]::NewLine, $utf8NoBom)
}

function Write-JsonFile([string]$Path, $Value) {
    $json = $Value | ConvertTo-Json -Depth 40
    [System.IO.File]::WriteAllText($Path, $json, $utf8NoBom)
}

function Get-PropertyValue($Object, [string]$Name) {
    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function Test-Property($Object, [string]$Name) {
    if ($null -eq $Object) { return $false }
    return ($null -ne $Object.PSObject.Properties[$Name])
}

function Require-Property($Object, [string]$Name, [string]$Context) {
    if (-not (Test-Property $Object $Name)) {
        throw "$Context 缺少必填字段 '$Name'"
    }
    $value = Get-PropertyValue $Object $Name
    if ($null -eq $value) {
        throw "$Context 字段 '$Name' 不能为 null"
    }
    return $value
}

function Copy-JsonValue($Value) {
    if ($null -eq $Value) { return $null }
    return ($Value | ConvertTo-Json -Depth 40 | ConvertFrom-Json)
}

function Resolve-ProjectPath([string]$Path, [bool]$MustExist) {
    if ([string]::IsNullOrWhiteSpace($Path)) { throw "路径不能为空" }
    $candidate = if ([System.IO.Path]::IsPathRooted($Path)) { $Path } else { Join-Path $root $Path }
    $full = [System.IO.Path]::GetFullPath($candidate)
    $prefix = $root.TrimEnd("\", "/") + [System.IO.Path]::DirectorySeparatorChar
    if (-not $full.Equals($root, [System.StringComparison]::OrdinalIgnoreCase) -and
        -not $full.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "路径必须位于项目目录内: $Path"
    }
    if ($MustExist -and -not (Test-Path -LiteralPath $full -PathType Leaf)) {
        throw "文件不存在: $full"
    }
    return $full
}

function Get-RelativePath([string]$Path) {
    $full = [System.IO.Path]::GetFullPath($Path)
    $prefix = $root.TrimEnd("\", "/") + [System.IO.Path]::DirectorySeparatorChar
    if ($full.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $full.Substring($prefix.Length).Replace("\", "/")
    }
    return $full.Replace("\", "/")
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

function Set-ObjectProperty($Object, [string]$Name, $Value) {
    if (Test-Property $Object $Name) {
        $Object.$Name = $Value
    } else {
        $Object | Add-Member -MemberType NoteProperty -Name $Name -Value $Value
    }
}

function Remove-ObjectProperty($Object, [string]$Name) {
    if (Test-Property $Object $Name) {
        $Object.PSObject.Properties.Remove($Name)
    }
}

function Assert-ObjectValue($Value, [string]$Context) {
    if ($null -eq $Value -or $Value -is [array] -or
        ($Value -isnot [pscustomobject] -and $Value -isnot [hashtable])) {
        throw "$Context 必须是 JSON 对象"
    }
}

function Assert-ComponentKey([string]$Component, [string]$Context) {
    if ([string]::IsNullOrWhiteSpace($Component)) { throw "$Context 的组件名不能为空" }
    if ($Component -in @("id", "name", "transform", "hierarchy")) {
        throw "$Context 不能通过组件命令修改基础键 '$Component'"
    }
    if (-not $script:allowedComponents.ContainsKey($Component)) {
        throw "$Context 使用了未知组件 '$Component'，请以 tools/scene_schema.json 为准"
    }
}

function Assert-ComponentValue([string]$Component, $Value, [string]$Context) {
    Assert-ObjectValue $Value "$Context.$Component"
    $fieldSet = $script:allowedComponents[$Component]
    if ($fieldSet.Count -eq 0) { return }
    foreach ($field in @($Value.PSObject.Properties.Name)) {
        if (-not $fieldSet.ContainsKey($field)) {
            throw "$Context.$Component 含未知字段 '$field'"
        }
    }
}

function Get-NumberArray($Value, [int]$Length, [string]$Context) {
    if ($null -eq $Value -or $Value -isnot [array]) {
        throw "$Context 必须是长度为 $Length 的数字数组"
    }
    $items = @($Value)
    if ($items.Count -ne $Length) {
        throw "$Context 长度 $($items.Count) != $Length"
    }
    $numbers = New-Object 'System.Collections.Generic.List[object]'
    foreach ($item in $items) {
        $number = [double]0
        $parsed = [double]::TryParse(
            [string]$item,
            [System.Globalization.NumberStyles]::Float,
            [System.Globalization.CultureInfo]::InvariantCulture,
            [ref]$number)
        if (-not $parsed -or [double]::IsNaN($number) -or [double]::IsInfinity($number)) {
            throw "$Context 必须只包含有限数字"
        }
        [void]$numbers.Add($number)
    }
    return $numbers.ToArray()
}

function Get-EntityId($Entity) {
    if ($null -eq $Entity) { return $null }
    $value = [int64]0
    if (-not [int64]::TryParse([string](Get-PropertyValue $Entity "id"), [ref]$value)) {
        throw "场景中存在无法解析的实体 id"
    }
    return $value
}

function Find-EntityById([int64]$Id) {
    foreach ($entity in $script:entities.ToArray()) {
        if ((Get-EntityId $entity) -eq $Id) { return $entity }
    }
    return $null
}

function Get-EntityName($Entity) {
    $name = Get-PropertyValue $Entity "name"
    if ($null -ne $name -and $null -ne (Get-PropertyValue $name "name")) {
        return [string](Get-PropertyValue $name "name")
    }
    return ""
}

function Resolve-Entity($Target, [string]$Context) {
    if ($null -eq $Target) { throw "$Context 的 target 不能为空" }
    if (($Target -is [pscustomobject] -or $Target -is [hashtable]) -and
        (Test-Property $Target "id" -or Test-Property $Target "name")) {
        if (Test-Property $Target "id") {
            $id = [int64]0
            if (-not [int64]::TryParse([string](Get-PropertyValue $Target "id"), [ref]$id)) {
                throw "$Context.target.id 必须是整数"
            }
            $entityById = Find-EntityById $id
            if ($null -eq $entityById) { throw "$Context 找不到实体 id=$id" }
            return $entityById
        }
        $Target = Get-PropertyValue $Target "name"
    }

    $targetText = [string]$Target
    if ([string]::IsNullOrWhiteSpace($targetText)) { throw "$Context 的 target 不能为空" }
    if ($script:aliases.ContainsKey($targetText)) {
        $aliasId = [int64]$script:aliases[$targetText]
        $aliasEntity = Find-EntityById $aliasId
        if ($null -eq $aliasEntity) { throw "$Context 使用的 ref '$targetText' 已失效" }
        return $aliasEntity
    }

    $numericId = [int64]0
    if ([int64]::TryParse($targetText, [ref]$numericId)) {
        $entityByNumericId = Find-EntityById $numericId
        if ($null -ne $entityByNumericId) { return $entityByNumericId }
    }

    $matches = @($script:entities.ToArray() | Where-Object { (Get-EntityName $_) -eq $targetText })
    if ($matches.Count -eq 1) { return $matches[0] }
    if ($matches.Count -gt 1) { throw "$Context 的实体名称 '$targetText' 不唯一，请使用 id 或 ref" }
    throw "$Context 找不到实体 '$targetText'"
}

function Ensure-Hierarchy($Entity) {
    $hierarchy = Get-PropertyValue $Entity "hierarchy"
    if ($null -eq $hierarchy) {
        $hierarchy = [pscustomobject][ordered]@{ parent = $sentinelParent; children = @() }
        Set-ObjectProperty $Entity "hierarchy" $hierarchy
    }
    if (-not (Test-Property $hierarchy "parent")) { Set-ObjectProperty $hierarchy "parent" $sentinelParent }
    if (-not (Test-Property $hierarchy "children")) { Set-ObjectProperty $hierarchy "children" @() }
    return $hierarchy
}

function Get-ParentId($Entity) {
    $hierarchy = Ensure-Hierarchy $Entity
    $parentId = [int64]0
    if (-not [int64]::TryParse([string]$hierarchy.parent, [ref]$parentId)) {
        throw "实体 id=$((Get-EntityId $Entity)) 的 hierarchy.parent 不是整数"
    }
    return $parentId
}

function Set-Children($Entity, [int64[]]$Children) {
    if ($null -eq $Entity) { return }
    $hierarchy = Ensure-Hierarchy $Entity
    $unique = New-Object 'System.Collections.Generic.List[object]'
    $seen = @{}
    foreach ($child in @($Children)) {
        $childId = [int64]$child
        if (-not $seen.ContainsKey($childId)) {
            $seen[$childId] = $true
            [void]$unique.Add($childId)
        }
    }
    Set-ObjectProperty $hierarchy "children" $unique.ToArray()
}

function Test-WouldCreateCycle($Child, $Parent) {
    if ($null -eq $Parent) { return $false }
    $childId = Get-EntityId $Child
    $cursor = $Parent
    $seen = @{}
    while ($null -ne $cursor) {
        $cursorId = Get-EntityId $cursor
        if ($cursorId -eq $childId) { return $true }
        if ($seen.ContainsKey($cursorId)) { return $true }
        $seen[$cursorId] = $true
        $parentId = Get-ParentId $cursor
        if ($parentId -eq $sentinelParent) { break }
        $cursor = Find-EntityById $parentId
        if ($null -eq $cursor) { break }
    }
    return $false
}

function Set-EntityParent($Child, $Parent, [string]$Context) {
    if (Test-WouldCreateCycle $Child $Parent) {
        throw "$Context 会创建层级环"
    }
    $childId = Get-EntityId $Child
    $oldParentId = Get-ParentId $Child
    $newParentId = $sentinelParent
    if ($null -ne $Parent) { $newParentId = Get-EntityId $Parent }

    if ($oldParentId -ne $sentinelParent) {
        $oldParent = Find-EntityById $oldParentId
        if ($null -ne $oldParent) {
            $oldChildren = @((Ensure-Hierarchy $oldParent).children | ForEach-Object {
                if ([int64]$_ -ne $childId) { [int64]$_ }
            })
            Set-Children $oldParent $oldChildren
        }
    }

    $childHierarchy = Ensure-Hierarchy $Child
    Set-ObjectProperty $childHierarchy "parent" $newParentId
    if ($null -ne $Parent) {
        $parentHierarchy = Ensure-Hierarchy $Parent
        $parentChildren = @($parentHierarchy.children | ForEach-Object { [int64]$_ })
        if (-not (@($parentChildren | Where-Object { $_ -eq $childId }).Count -gt 0)) {
            $parentChildren = @($parentChildren + @($childId))
        }
        Set-Children $Parent $parentChildren
    }
}

function Find-NextEntityId {
    $used = @{}
    foreach ($entity in $script:entities.ToArray()) { $used[(Get-EntityId $entity)] = $true }
    $candidate = [int64]0
    while ($used.ContainsKey($candidate) -and $candidate -lt $sentinelParent) { $candidate++ }
    if ($candidate -ge $sentinelParent) { throw "没有可用的实体 id" }
    return $candidate
}

function Set-EntityComponent($Entity, [string]$Component, $Value, [string]$Context) {
    Assert-ComponentKey $Component $Context
    Assert-ComponentValue $Component $Value $Context
    Set-ObjectProperty $Entity $Component (Copy-JsonValue $Value)
}

function Remove-EntityComponent($Entity, [string]$Component, [string]$Context) {
    Assert-ComponentKey $Component $Context
    Remove-ObjectProperty $Entity $Component
}

function Apply-TransformFields($Entity, $Source, [string]$Context) {
    Assert-ObjectValue $Source $Context
    $transform = Get-PropertyValue $Entity "transform"
    if ($null -eq $transform) {
        $transform = [pscustomobject][ordered]@{
            position = @(0.0, 0.0, 0.0)
            rotation = @(1.0, 0.0, 0.0, 0.0)
            scale = @(1.0, 1.0, 1.0)
        }
        Set-ObjectProperty $Entity "transform" $transform
    }
    $changed = $false
    foreach ($field in @("position", "rotation", "scale")) {
        if (Test-Property $Source $field) {
            $length = if ($field -eq "rotation") { 4 } else { 3 }
            $values = Get-NumberArray (Get-PropertyValue $Source $field) $length "$Context.$field"
            Set-ObjectProperty $transform $field $values
            $changed = $true
        }
    }
    if (-not $changed) { throw "$Context 至少要提供 position、rotation、scale 之一" }
}

function New-DefaultRender {
    return [pscustomobject][ordered]@{
        visible = $true
        castShadow = $true
        receiveShadow = $true
        showAABB = $false
        showOBB = $false
        doubleSided = $false
        wireframe = $false
    }
}

function New-DefaultMaterial {
    return [pscustomobject][ordered]@{
        albedoPath = ""
        normalPath = ""
        roughnessPath = ""
        metallicPath = ""
        aoPath = ""
        emissivePath = ""
        albedoColor = @(1.0, 1.0, 1.0)
        metallic = 0.0
        roughness = 0.5
        ao = 1.0
        useAlbedoTexture = $false
        useNormalTexture = $false
        useRoughnessTexture = $false
        useMetallicTexture = $false
        useAOTexture = $false
        useEmissiveTexture = $false
        albedoSamplerType = 0
        normalSamplerType = 0
        roughnessSamplerType = 0
        metallicSamplerType = 0
        aoSamplerType = 0
        emissiveSamplerType = 0
    }
}

function Add-PrimitiveDefaults($Entity, [string]$Primitive, [string]$Context) {
    if ([string]::IsNullOrWhiteSpace($Primitive) -or $Primitive -eq "empty") { return }
    $normalized = $Primitive.ToLowerInvariant()
    $modelPrimitives = @("cube", "sphere", "plane", "cylinder", "cone", "capsule", "torus", "pyramid")
    if ($modelPrimitives -contains $normalized) {
        $mesh = [pscustomobject][ordered]@{
            type = 4
            modelPath = "engine/models/Base Model/$normalized.glb"
        }
        Set-EntityComponent $Entity "mesh" $mesh $Context
        Set-EntityComponent $Entity "render" (New-DefaultRender) $Context
        Set-EntityComponent $Entity "material" (New-DefaultMaterial) $Context
        return
    }
    if ($normalized -eq "camera") {
        $camera = [pscustomobject][ordered]@{
            fov = 60.0
            nearPlane = 0.1
            farPlane = 500.0
            isMainCamera = $false
            isOrthographic = $false
            orthographicSize = 5.0
            enableFrustumCulling = $true
            showFrustumWireframe = $false
            useSubMeshCulling = $false
            showBVHWireframe = $false
        }
        Set-EntityComponent $Entity "camera" $camera $Context
        return
    }
    if ($normalized -eq "light") {
        $light = [pscustomobject][ordered]@{
            type = 0
            color = @(1.0, 1.0, 1.0)
            intensity = 1.0
            range = 10.0
            spotAngle = 45.0
            castShadow = $true
        }
        Set-EntityComponent $Entity "light" $light $Context
        return
    }
    throw "$Context 不支持 primitive '$Primitive'；支持 empty、cube、sphere、plane、cylinder、cone、capsule、torus、pyramid、camera、light"
}

function Apply-CreateEntity($Command, [int]$Index) {
    $context = "command[$Index] create_entity"
    $nameValue = if (Test-Property $Command "name") { [string](Get-PropertyValue $Command "name") } else { "Entity" }
    if ([string]::IsNullOrWhiteSpace($nameValue)) { throw "$context 的 name 不能为空" }

    $ref = ""
    if (Test-Property $Command "ref") { $ref = [string](Get-PropertyValue $Command "ref") }
    elseif (Test-Property $Command "id") { $ref = [string](Get-PropertyValue $Command "id") }
    if ($ref -and $script:aliases.ContainsKey($ref)) { throw "$context 的 ref '$ref' 已存在" }

    $newId = Find-NextEntityId
    $entity = [pscustomobject][ordered]@{
        id = $newId
        name = [pscustomobject][ordered]@{ name = $nameValue }
        transform = [pscustomobject][ordered]@{
            position = @(0.0, 0.0, 0.0)
            rotation = @(1.0, 0.0, 0.0, 0.0)
            scale = @(1.0, 1.0, 1.0)
        }
        hierarchy = [pscustomobject][ordered]@{
            parent = $sentinelParent
            children = @()
        }
    }
    [void]$script:entities.Add($entity)

    if (Test-Property $Command "primitive") {
        Add-PrimitiveDefaults $entity ([string](Get-PropertyValue $Command "primitive")) $context
    }
    if (Test-Property $Command "transform") {
        Apply-TransformFields $entity (Get-PropertyValue $Command "transform") $context
    }
    if (Test-Property $Command "components") {
        $components = Get-PropertyValue $Command "components"
        Assert-ObjectValue $components "$context.components"
        foreach ($component in @($components.PSObject.Properties.Name)) {
            Set-EntityComponent $entity $component (Get-PropertyValue $components $component) $context
        }
    }
    if ($ref) { $script:aliases[$ref] = $newId }
    if (Test-Property $Command "parent") {
        $parent = Resolve-Entity (Get-PropertyValue $Command "parent") $context
        Set-EntityParent $entity $parent $context
    }
    return [ordered]@{ entityId = $newId; ref = $ref; name = $nameValue }
}

function Collect-SubtreeIds($RootEntity) {
    $ids = New-Object 'System.Collections.Generic.List[object]'
    $stack = New-Object 'System.Collections.Generic.Stack[object]'
    $seen = @{}
    $stack.Push($RootEntity)
    while ($stack.Count -gt 0) {
        $entity = $stack.Pop()
        $entityId = Get-EntityId $entity
        if ($seen.ContainsKey($entityId)) { continue }
        $seen[$entityId] = $true
        [void]$ids.Add($entityId)
        foreach ($childIdValue in @((Ensure-Hierarchy $entity).children)) {
            $childId = [int64]0
            if ([int64]::TryParse([string]$childIdValue, [ref]$childId)) {
                $child = Find-EntityById $childId
                if ($null -ne $child) { $stack.Push($child) }
            }
        }
    }
    return $ids.ToArray()
}

function Apply-DeleteEntity($Command, [int]$Index) {
    $context = "command[$Index] delete_entity"
    $entity = Resolve-Entity (Require-Property $Command "target" $context) $context
    $recursive = $true
    if (Test-Property $Command "recursive") { $recursive = [bool](Get-PropertyValue $Command "recursive") }
    $rootId = Get-EntityId $entity
    $removeIds = if ($recursive) { @(Collect-SubtreeIds $entity) } else { @($rootId) }
    if (-not $recursive) {
        foreach ($childIdValue in @((Ensure-Hierarchy $entity).children)) {
            $childId = [int64]0
            if ([int64]::TryParse([string]$childIdValue, [ref]$childId)) {
                $child = Find-EntityById $childId
                if ($null -ne $child) { Set-EntityParent $child $null $context }
            }
        }
    }
    Set-EntityParent $entity $null $context
    $script:entities = @($script:entities.ToArray() | Where-Object {
        $currentId = Get-EntityId $_
        -not (@($removeIds | Where-Object { [int64]$_ -eq $currentId }).Count -gt 0)
    })
    foreach ($alias in @($script:aliases.Keys)) {
        if (@($removeIds | Where-Object { [int64]$_ -eq [int64]$script:aliases[$alias] }).Count -gt 0) {
            $script:aliases.Remove($alias)
        }
    }
    return [ordered]@{ entityId = $rootId; removedEntityIds = @($removeIds); recursive = $recursive }
}

function Apply-RenameEntity($Command, [int]$Index) {
    $context = "command[$Index] rename_entity"
    $entity = Resolve-Entity (Require-Property $Command "target" $context) $context
    $name = [string](Require-Property $Command "name" $context)
    if ([string]::IsNullOrWhiteSpace($name)) { throw "$context 的 name 不能为空" }
    $nameComponent = Get-PropertyValue $entity "name"
    if ($null -eq $nameComponent) {
        $nameComponent = [pscustomobject][ordered]@{ name = $name }
        Set-ObjectProperty $entity "name" $nameComponent
    } else {
        Set-ObjectProperty $nameComponent "name" $name
    }
    return [ordered]@{ entityId = Get-EntityId $entity; name = $name }
}

function Apply-SetTransform($Command, [int]$Index) {
    $context = "command[$Index] set_transform"
    $entity = Resolve-Entity (Require-Property $Command "target" $context) $context
    $source = if (Test-Property $Command "transform") { Get-PropertyValue $Command "transform" } else { $Command }
    Apply-TransformFields $entity $source $context
    return [ordered]@{ entityId = Get-EntityId $entity; transform = (Copy-JsonValue (Get-PropertyValue $entity "transform")) }
}

function Apply-SetParent($Command, [int]$Index) {
    $context = "command[$Index] set_parent"
    $child = Resolve-Entity (Require-Property $Command "child" $context) $context
    $parent = $null
    if (Test-Property $Command "parent") {
        $parentValue = Get-PropertyValue $Command "parent"
        if ($null -ne $parentValue -and [string]$parentValue -ne "") {
            $parent = Resolve-Entity $parentValue $context
        }
    }
    Set-EntityParent $child $parent $context
    return [ordered]@{ childId = Get-EntityId $child; parentId = if ($null -eq $parent) { $sentinelParent } else { Get-EntityId $parent } }
}

function Apply-SetComponent($Command, [int]$Index) {
    $context = "command[$Index] set_component"
    $entity = Resolve-Entity (Require-Property $Command "target" $context) $context
    $component = [string](Require-Property $Command "component" $context)
    $value = Require-Property $Command "value" $context
    Set-EntityComponent $entity $component $value $context
    return [ordered]@{ entityId = Get-EntityId $entity; component = $component }
}

function Apply-PatchComponent($Command, [int]$Index) {
    $context = "command[$Index] patch_component"
    $entity = Resolve-Entity (Require-Property $Command "target" $context) $context
    $component = [string](Require-Property $Command "component" $context)
    $patch = Require-Property $Command "patch" $context
    Assert-ComponentKey $component $context
    Assert-ObjectValue $patch "$context.patch"
    $current = Get-PropertyValue $entity $component
    if ($null -eq $current) { $current = [pscustomobject]@{} } else { $current = Copy-JsonValue $current }
    foreach ($field in @($patch.PSObject.Properties.Name)) {
        Set-ObjectProperty $current $field (Copy-JsonValue (Get-PropertyValue $patch $field))
    }
    Set-EntityComponent $entity $component $current $context
    return [ordered]@{ entityId = Get-EntityId $entity; component = $component; fields = @($patch.PSObject.Properties.Name) }
}

function Apply-RemoveComponent($Command, [int]$Index) {
    $context = "command[$Index] remove_component"
    $entity = Resolve-Entity (Require-Property $Command "target" $context) $context
    $component = [string](Require-Property $Command "component" $context)
    Remove-EntityComponent $entity $component $context
    return [ordered]@{ entityId = Get-EntityId $entity; component = $component }
}

function Apply-SetSceneProperty($Command, [int]$Index) {
    $context = "command[$Index] set_scene_property"
    $property = [string](Require-Property $Command "property" $context)
    if ($property -ne "game") { throw "$context 只允许修改顶层属性 'game'" }
    $value = Require-Property $Command "value" $context
    if ($value -isnot [string]) { throw "$context.value 必须是字符串" }
    Set-ObjectProperty $script:scene "game" ([string]$value)
    return [ordered]@{ property = $property; value = [string]$value }
}

function Apply-Command($Command, [int]$Index) {
    if ($null -eq $Command -or $Command -is [array]) { throw "command[$Index] 必须是 JSON 对象" }
    $op = [string](Require-Property $Command "op" "command[$Index]")
    switch ($op) {
        "create_entity" { return Apply-CreateEntity $Command $Index }
        "delete_entity" { return Apply-DeleteEntity $Command $Index }
        "rename_entity" { return Apply-RenameEntity $Command $Index }
        "set_transform" { return Apply-SetTransform $Command $Index }
        "set_parent" { return Apply-SetParent $Command $Index }
        "set_component" { return Apply-SetComponent $Command $Index }
        "patch_component" { return Apply-PatchComponent $Command $Index }
        "remove_component" { return Apply-RemoveComponent $Command $Index }
        "set_scene_property" { return Apply-SetSceneProperty $Command $Index }
        default { throw "command[$Index] 不支持操作 '$op'" }
    }
}

function Invoke-SceneValidation([string]$Path, [string]$LogPath) {
    $powershellCommand = Get-Command powershell.exe -ErrorAction SilentlyContinue
    if ($null -eq $powershellCommand) { throw "找不到 powershell.exe，无法调用场景校验器" }
    $args = @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $validateScript,
        $Path, "-Schema", $schemaPath
    )
    if ($CheckAssets) { $args += "-CheckAssets" }
    $output = (& $powershellCommand.Source @args 2>&1 | Out-String)
    $exitCode = [int]$LASTEXITCODE
    [System.IO.File]::WriteAllText($LogPath, $output, $utf8NoBom)
    return [pscustomobject]@{
        status = if ($exitCode -eq 0) { "passed" } else { "failed" }
        exitCode = $exitCode
        log = Get-RelativePath $LogPath
        output = $output.Trim()
    }
}

function Invoke-Regression([string]$Path, [string]$LogPath) {
    if ($TestLayer -eq "none") { return $null }
    if (-not (Test-Path -LiteralPath $testScript -PathType Leaf)) { throw "统一测试入口不存在: $testScript" }
    $sceneGame = [string](Get-PropertyValue $script:scene "game")
    $caseLayers = @($TestLayer)
    if ($TestLayer -eq "all") { $caseLayers = @("gameplay", "render") }
    $case = [ordered]@{
        name = "scene_command"
        scene = $Path
        game = $sceneGame
        layers = $caseLayers
        frames = 60
    }
    $manifest = [ordered]@{
        schemaVersion = 1
        defaults = [ordered]@{
            frames = 60
            fixedDeltaSeconds = 0.016666667
            timeoutSeconds = $TestTimeoutSeconds
            checkAssets = [bool]$CheckAssets
        }
        cases = @($case)
    }
    $manifestPath = Join-Path $runDir "regression_manifest.json"
    Write-JsonFile $manifestPath $manifest
    $regressionOutputRoot = Join-Path $runDir "regression_runs"
    $powershellCommand = Get-Command powershell.exe -ErrorAction SilentlyContinue
    if ($null -eq $powershellCommand) { throw "找不到 powershell.exe，无法调用统一测试入口" }
    $args = @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $testScript,
        "-Layer", $TestLayer,
        "-ManifestPath", $manifestPath,
        "-OutputRoot", $regressionOutputRoot,
        "-RunId", "scene_command"
    )
    if ($SkipTestBuild) { $args += "-SkipBuild" }
    $args += @("-TestTimeoutSeconds", [string]$TestTimeoutSeconds, "-BuildTimeoutSeconds", [string]$BuildTimeoutSeconds, "-StopOnFailure")
    $output = (& $powershellCommand.Source @args 2>&1 | Out-String)
    $exitCode = [int]$LASTEXITCODE
    [System.IO.File]::WriteAllText($LogPath, $output, $utf8NoBom)
    $regressionResultPath = Join-Path $regressionOutputRoot "scene_command\result.json"
    $regressionResult = $null
    if (Test-Path -LiteralPath $regressionResultPath -PathType Leaf) {
        try { $regressionResult = [System.IO.File]::ReadAllText($regressionResultPath, $utf8NoBom) | ConvertFrom-Json } catch {}
    }
    return [pscustomobject]@{
        status = if ($exitCode -eq 0) { "passed" } else { "failed" }
        exitCode = $exitCode
        log = Get-RelativePath $LogPath
        result = if ($null -eq $regressionResult) { $null } else { Get-RelativePath $regressionResultPath }
        output = $output.Trim()
    }
}

function Add-OperationResult([int]$Index, [string]$Op, [string]$Status, $Details, [string]$ErrorText) {
    $entry = [ordered]@{
        index = $Index
        op = $Op
        status = $Status
    }
    if ($null -ne $Details) { $entry.details = $Details }
    if ($ErrorText) { $entry.error = $ErrorText }
    [void]$script:operations.Add([pscustomobject]$entry)
}

$summary = [ordered]@{
    schemaVersion = 1
    runId = $runId
    success = $false
    startedAt = (Get-Date).ToString("o")
    sourceScene = ""
    outputScene = ""
    commands = ""
    candidateScene = Get-RelativePath $script:stagePath
    sourceBackup = $null
    destinationBackup = $null
    validation = $null
    regression = $null
    operations = @()
    aliases = @()
    counts = [ordered]@{
        total = 0
        applied = 0
        failed = 0
    }
}

try {
    if (-not (Test-Path -LiteralPath $schemaPath -PathType Leaf)) { throw "场景 schema 不存在: $schemaPath" }
    if (-not (Test-Path -LiteralPath $validateScript -PathType Leaf)) { throw "场景校验器不存在: $validateScript" }

    $script:sourcePath = Resolve-ProjectPath $ScenePath $true
    $commandsFullPath = Resolve-ProjectPath $CommandsPath $true
    if ($InPlace) {
        if ($OutputPath) { throw "-InPlace 不能同时指定 -OutputPath" }
        $script:finalPath = $script:sourcePath
    } elseif ($OutputPath) {
        $script:finalPath = Resolve-ProjectPath $OutputPath $false
        if ($script:finalPath.Equals($script:sourcePath, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "输出路径等于源场景；如需覆盖源场景必须显式使用 -InPlace"
        }
    } else {
        $script:finalPath = Join-Path $runDir "scene.generated.json"
    }

    $summary.sourceScene = Get-RelativePath $script:sourcePath
    $summary.outputScene = Get-RelativePath $script:finalPath
    $summary.commands = Get-RelativePath $commandsFullPath
    Copy-Item -LiteralPath $commandsFullPath -Destination (Join-Path $runDir "commands.applied.json")
    Write-RunMessage "RUN $runId"
    Write-RunMessage "source=$($summary.sourceScene)"
    Write-RunMessage "output=$($summary.outputScene)"

    $sourceValidation = Invoke-SceneValidation $script:sourcePath (Join-Path $runDir "source_validation.log")
    if ($sourceValidation.status -ne "passed") {
        throw "源场景校验失败，请先修复源场景；详见 $($sourceValidation.log)"
    }

    $rawScene = [System.IO.File]::ReadAllText($script:sourcePath, $utf8NoBom)
    $script:scene = $rawScene | ConvertFrom-Json
    if ($null -eq $script:scene -or $null -eq (Get-PropertyValue $script:scene "entities")) {
        throw "源场景缺少顶层 entities 数组"
    }
    foreach ($entity in @((Get-PropertyValue $script:scene "entities"))) {
        [void]$script:entities.Add($entity)
    }
    if ($script:entities.Count -eq 0) { throw "源场景 entities 不能为空" }

    $schema = [System.IO.File]::ReadAllText($schemaPath, $utf8NoBom) | ConvertFrom-Json
    foreach ($component in @($schema.components)) {
        $key = [string](Get-PropertyValue $component "serializeKey")
        if ([string]::IsNullOrWhiteSpace($key)) { continue }
        $fieldSet = @{}
        foreach ($field in @((Get-PropertyValue $component "fields"))) {
            $fieldName = [string](Get-PropertyValue $field "name")
            if ($fieldName) { $fieldSet[$fieldName] = $true }
        }
        $script:allowedComponents[$key] = $fieldSet
    }

    $commandDocument = [System.IO.File]::ReadAllText($commandsFullPath, $utf8NoBom) | ConvertFrom-Json
    $documentVersion = [int](Get-PropertyValue $commandDocument "schemaVersion")
    if ($documentVersion -ne 1) { throw "commands schemaVersion 必须为 1" }
    $commandArray = Get-PropertyValue $commandDocument "commands"
    # PowerShell ConvertFrom-Json unwraps a one-item JSON array into a
    # PSCustomObject. Normalize that case so single-command GameSpecs behave
    # the same as multi-command GameSpecs without accepting scalar values.
    $isJsonArray = $commandArray -is [array]
    $isUnwrappedSingleCommand = $commandArray -is [pscustomobject]
    if ($null -eq $commandArray -or (-not $isJsonArray -and -not $isUnwrappedSingleCommand)) {
        throw "commands 必须是 JSON 数组"
    }
    $commands = @($commandArray)
    if ($commands.Count -gt 500) { throw "单次最多允许 500 条命令" }
    $summary.counts.total = $commands.Count
    for ($index = 0; $index -lt $commands.Count; $index++) {
        $command = $commands[$index]
        $op = [string](Get-PropertyValue $command "op")
        try {
            $details = Apply-Command $command $index
            Add-OperationResult $index $op "applied" $details ""
            $summary.counts.applied = [int]$summary.counts.applied + 1
        } catch {
            $summary.counts.failed = [int]$summary.counts.failed + 1
            Add-OperationResult $index $op "failed" $null $_.Exception.Message
            throw "command[$index] $op 执行失败: $($_.Exception.Message)"
        }
    }

    $script:scene.entities = $script:entities.ToArray()
    Write-JsonFile $script:stagePath $script:scene
    $candidateValidation = Invoke-SceneValidation $script:stagePath (Join-Path $runDir "candidate_validation.log")
    $summary.validation = $candidateValidation
    if ($candidateValidation.status -ne "passed") {
        throw "候选场景校验失败，请查看 $($candidateValidation.log)"
    }

    if ($TestLayer -ne "none") {
        $summary.regression = Invoke-Regression $script:stagePath (Join-Path $runDir "regression.log")
        if ($summary.regression.status -ne "passed") {
            throw "候选场景回归失败，请查看 $($summary.regression.log)"
        }
    }

    $parent = Split-Path -Parent $script:finalPath
    if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    if (Test-Path -LiteralPath $script:finalPath -PathType Leaf) {
        $backupName = if ($script:finalPath.Equals($script:sourcePath, [System.StringComparison]::OrdinalIgnoreCase)) {
            "source.before.json"
        } else {
            "destination.before.json"
        }
        $backupPath = Join-Path $runDir $backupName
        Copy-Item -LiteralPath $script:finalPath -Destination $backupPath
        $backupEntry = [ordered]@{
            path = Get-RelativePath $backupPath
            original = Get-RelativePath $script:finalPath
            size = (Get-Item -LiteralPath $backupPath).Length
            sha256 = Get-Sha256 $backupPath
        }
        if ($script:finalPath.Equals($script:sourcePath, [System.StringComparison]::OrdinalIgnoreCase)) {
            $summary.sourceBackup = $backupEntry
        } else {
            $summary.destinationBackup = $backupEntry
        }
    }
    [System.IO.File]::Copy($script:stagePath, $script:finalPath, $true)
    if ((Get-Sha256 $script:stagePath) -ne (Get-Sha256 $script:finalPath)) {
        throw "目标场景写入后 SHA-256 校验失败，未确认交付完整性"
    }

    $summary.success = $true
    $summary.aliases = @($script:aliases.GetEnumerator() | Sort-Object Name | ForEach-Object {
        [ordered]@{ ref = [string]$_.Name; entityId = [int64]$_.Value }
    })
    Write-RunMessage "COMMITTED $($summary.outputScene)"
} catch {
    $summary.success = $false
    $summary.error = $_.Exception.Message
    Write-RunMessage "FAILED $($summary.error)"
}

$summary.operations = $script:operations.ToArray()
$summary.endedAt = (Get-Date).ToString("o")
$summary.result = Get-RelativePath $resultPath
Write-JsonFile $resultPath $summary
Write-RunMessage "RESULT $($summary.result)"
Write-Host "RESULT_PATH=$resultPath"
Write-Host "SUCCESS=$($summary.success)"

if (-not $summary.success) { exit 1 }
exit 0
