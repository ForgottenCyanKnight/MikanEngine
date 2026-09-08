# agent_discovery.ps1 - MikanEngine AI Native 项目/场景/资产发现层
# ------------------------------------------------------------------
# 这是 Agent 的只读上下文入口。它不启动引擎、不修改项目文件，只把
# 项目能力、场景结构和资源索引整理成可审计的 JSON。
#
# 输出：out\agent_discovery\<run-id>\request.json / result.json / manifest.json
# 退出码：0=发现成功，3=输入或环境错误。
# ------------------------------------------------------------------
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("project", "context", "scene", "assets", "device")]
    [string]$Mode,

    [string]$ProjectPath = "",

    [string]$ScenePath = "",

    [string]$Query = "",

    [ValidateSet("all", "scenes", "scripts", "shaders", "models", "textures", "audio")]
    [string]$AssetType = "all",

    [int]$MaxResults = 200,

    [int]$MaxEntities = 500,

    [int]$MaxAssetReferences = 200,

    [string]$OutputRoot = "out\agent_discovery",

    [string]$RunId = "",

    [string]$RenderDocPath = "",

    [string]$NsightPath = "",

    [string]$VulkanInfoPath = ""
)

$ErrorActionPreference = "Stop"
$root = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot)).TrimEnd('\', '/')
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$script:runDir = $null
$script:resultPath = $null
$script:sourceArtifacts = New-Object 'System.Collections.Generic.List[object]'
$script:selectedProjectRoot = $null
$script:selectedResourceRoot = $null
$script:selectedProjectManifestPath = $null
$script:excludedDirectoryNames = @(
    ".git", ".vs", "out", "backup", "dependencies", "third_party", "lib", "dll",
    "android", "HarmonyOS"
)
$script:knownAssetExtensions = @(
    ".obj", ".fbx", ".gltf", ".glb", ".dae", ".ply", ".vox",
    ".png", ".jpg", ".jpeg", ".tga", ".dds", ".ktx", ".ktx2", ".hdr", ".exr",
    ".wav", ".ogg", ".mp3", ".flac", ".tmx", ".json"
)

function Get-PropertyValue($Object, [string]$Name, $Default = $null) {
    if ($null -eq $Object) { return $Default }
    if ($Object -is [System.Collections.IDictionary] -and $Object.Contains($Name)) {
        return $Object[$Name]
    }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $Default }
    return $property.Value
}

function Write-JsonFile([string]$Path, $Value) {
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    $json = $Value | ConvertTo-Json -Depth 60
    [System.IO.File]::WriteAllText($Path, $json, $utf8NoBom)
}

function Get-Sha256([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return "" }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $stream = $null
    try {
        $stream = [System.IO.File]::OpenRead($Path)
        return ([System.BitConverter]::ToString($sha.ComputeHash($stream)).Replace("-", "")).ToLowerInvariant()
    } finally {
        if ($null -ne $stream) { $stream.Dispose() }
        $sha.Dispose()
    }
}

function Get-RelativePath([string]$Path) {
    $full = [System.IO.Path]::GetFullPath($Path)
    $prefix = $root.TrimEnd('\', '/') + '\'
    if ($full.Equals($root, [System.StringComparison]::OrdinalIgnoreCase)) { return "" }
    if ($full.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $full.Substring($prefix.Length).Replace('\', '/')
    }
    return $full.Replace('\', '/')
}

function Resolve-ProjectPath([string]$Path, [bool]$MustExist = $false) {
    if ([string]::IsNullOrWhiteSpace($Path)) { throw "路径不能为空" }
    $candidate = if ([System.IO.Path]::IsPathRooted($Path)) { $Path } else { Join-Path $root $Path }
    $full = [System.IO.Path]::GetFullPath($candidate)
    $prefix = $root.TrimEnd('\', '/') + '\'
    if (-not $full.Equals($root, [System.StringComparison]::OrdinalIgnoreCase) -and
        -not $full.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "路径必须位于项目目录内: $Path"
    }
    if ($MustExist -and -not (Test-Path -LiteralPath $full -PathType Leaf)) {
        throw "文件不存在: $full"
    }
    return $full
}

function Resolve-ProjectDirectory([string]$Path, [bool]$MustExist = $false) {
    $full = Resolve-ProjectPath $Path $false
    if ($MustExist -and -not (Test-Path -LiteralPath $full -PathType Container)) {
        throw "目录不存在: $full"
    }
    return $full
}

function Test-PathWithin([string]$Path, [string]$BasePath) {
    $full = [System.IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
    $base = [System.IO.Path]::GetFullPath($BasePath).TrimEnd('\', '/')
    return $full.Equals($base, [System.StringComparison]::OrdinalIgnoreCase) -or
        $full.StartsWith($base + '\', [System.StringComparison]::OrdinalIgnoreCase)
}

function Get-PathRelativeTo([string]$Path, [string]$BasePath) {
    $full = [System.IO.Path]::GetFullPath($Path)
    $base = [System.IO.Path]::GetFullPath($BasePath).TrimEnd('\', '/')
    if ($full.Equals($base, [System.StringComparison]::OrdinalIgnoreCase)) { return '.' }
    $prefix = $base + '\'
    if ($full.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $full.Substring($prefix.Length).Replace('\', '/')
    }
    $baseUri = [System.Uri]::new($prefix)
    $fullUri = [System.Uri]::new($full)
    return [System.Uri]::UnescapeDataString($baseUri.MakeRelativeUri($fullUri).ToString()).Replace('\', '/')
}

function Read-JsonIfExists([string]$RelativePath) {
    $full = Resolve-ProjectPath $RelativePath $false
    if (-not (Test-Path -LiteralPath $full -PathType Leaf)) { return $null }
    try {
        return [System.IO.File]::ReadAllText($full, $utf8NoBom) | ConvertFrom-Json
    } catch {
        return [pscustomobject]@{ __parseError = $_.Exception.Message }
    }
}

function Add-SourceArtifact([string]$Path, [string]$Kind) {
    try {
        $full = if ([System.IO.Path]::IsPathRooted($Path)) { Resolve-ProjectPath $Path $true } else { Resolve-ProjectPath $Path $true }
        $relative = Get-RelativePath $full
        foreach ($existing in $script:sourceArtifacts.ToArray()) {
            if ([string]$existing.path -eq $relative) { return $existing }
        }
        $entry = [ordered]@{
            path = $relative
            kind = $Kind
            size = [int64](Get-Item -LiteralPath $full).Length
            sha256 = Get-Sha256 $full
        }
        [void]$script:sourceArtifacts.Add([pscustomobject]$entry)
        return [pscustomobject]$entry
    } catch {
        return $null
    }
}

function Get-PathSegments([string]$FullPath) {
    $relative = Get-RelativePath $FullPath
    return @($relative -split '/')
}

function Test-ExcludedFile([System.IO.FileInfo]$File) {
    $segments = Get-PathSegments $File.FullName
    if ($segments.Count -gt 0 -and $script:excludedDirectoryNames -contains $segments[0]) { return $true }
    return $false
}

function Get-FilesUnderRoots([string[]]$RelativeRoots) {
    $filesByPath = @{}
    foreach ($relativeRoot in @($RelativeRoots)) {
        $fullRoot = Join-Path $root $relativeRoot
        if (-not (Test-Path -LiteralPath $fullRoot -PathType Container)) { continue }
        foreach ($file in @(Get-ChildItem -LiteralPath $fullRoot -File -Recurse -Force -ErrorAction SilentlyContinue)) {
            if (Test-ExcludedFile $file) { continue }
            $key = $file.FullName.ToLowerInvariant()
            if (-not $filesByPath.ContainsKey($key)) { $filesByPath[$key] = $file }
        }
    }
    return @($filesByPath.Values | Sort-Object FullName)
}

function Get-AssetTypeForFile([System.IO.FileInfo]$File) {
    $relative = (Get-RelativePath $File.FullName).ToLowerInvariant()
    $extension = $File.Extension.ToLowerInvariant()
    if ($extension -eq ".json" -and ($relative -match "^assets/" -or $relative -match "^projects/.+/scenes/[^/]+\.json$" -or $relative -match "(^|/)scene[^/]*\.json$")) { return "scenes" }
    if ($extension -in @(".cpp", ".cc", ".cxx", ".c", ".h", ".hh", ".hpp", ".hxx", ".cs", ".lua", ".js", ".ts")) { return "scripts" }
    if ($extension -in @(".glsl", ".vert", ".frag", ".geom", ".tesc", ".tese", ".comp", ".hlsl", ".shader", ".spv")) { return "shaders" }
    if ($extension -in @(".obj", ".fbx", ".gltf", ".glb", ".dae", ".ply", ".vox")) { return "models" }
    if ($extension -in @(".png", ".jpg", ".jpeg", ".tga", ".dds", ".ktx", ".ktx2", ".hdr", ".exr")) { return "textures" }
    if ($extension -in @(".wav", ".ogg", ".mp3", ".flac")) { return "audio" }
    return "other"
}

function Get-FileSummary([System.IO.FileInfo]$File, [string]$Type, [bool]$IncludeHash) {
    $summary = [ordered]@{
        path = Get-RelativePath $File.FullName
        type = $Type
        extension = $File.Extension.ToLowerInvariant()
        size = [int64]$File.Length
        modifiedAt = $File.LastWriteTimeUtc.ToString("o")
    }
    if ($IncludeHash) { $summary.sha256 = Get-Sha256 $File.FullName }
    return [pscustomobject]$summary
}

function Get-InventoryCategory([string]$Category, [string[]]$Roots, [int]$Limit) {
    $all = @(Get-FilesUnderRoots $Roots | Where-Object { (Get-AssetTypeForFile $_) -eq $Category })
    $selected = @($all | Select-Object -First $Limit | ForEach-Object { Get-FileSummary $_ $Category $false })
    return [ordered]@{
        category = $Category
        roots = @($Roots)
        total = $all.Count
        returned = $selected.Count
        truncated = ($all.Count -gt $selected.Count)
        items = @($selected)
    }
}

function Get-ProjectInventory([int]$Limit) {
    $categories = [ordered]@{}
    $categories.scenes = Get-InventoryCategory "scenes" @("assets", "projects") $Limit
    $categories.scripts = Get-InventoryCategory "scripts" @("projects", "src", "include") $Limit
    $categories.shaders = Get-InventoryCategory "shaders" @("engine\shaders", "src\shaders") $Limit
    $categories.models = Get-InventoryCategory "models" @("models", "assets", "resources") $Limit
    $categories.textures = Get-InventoryCategory "textures" @("assets", "models", "resources") $Limit
    $categories.audio = Get-InventoryCategory "audio" @("assets", "resources") $Limit

    $projectsRoot = Join-Path $root "projects"
    $plugins = @()
    if (Test-Path -LiteralPath $projectsRoot -PathType Container) {
        foreach ($projectDir in @(Get-ChildItem -LiteralPath $projectsRoot -Directory -Force -ErrorAction SilentlyContinue | Sort-Object FullName)) {
            $manifestPath = Join-Path $projectDir.FullName "project.json"
            $codeRoot = "games"
            if (Test-Path -LiteralPath $manifestPath -PathType Leaf) {
                try {
                    $manifest = [System.IO.File]::ReadAllText($manifestPath, $utf8NoBom) | ConvertFrom-Json
                    $manifestCodeRoot = [string](Get-PropertyValue $manifest "codeRoot" "games")
                    if ($manifestCodeRoot) { $codeRoot = $manifestCodeRoot.Replace('/', '\').Trim() }
                } catch { continue }
            }
            $codeDir = Join-Path $projectDir.FullName $codeRoot
            if (-not (Test-Path -LiteralPath $codeDir -PathType Container)) { continue }
            foreach ($pluginDir in @(Get-ChildItem -LiteralPath $codeDir -Directory -Force -ErrorAction SilentlyContinue | Sort-Object FullName)) {
                $plugins += [ordered]@{
                    path = Get-RelativePath $pluginDir.FullName
                    type = "plugins"
                    name = $pluginDir.Name
                    projectPath = Get-RelativePath $projectDir.FullName
                }
                if ($plugins.Count -ge $Limit) { break }
            }
            if ($plugins.Count -ge $Limit) { break }
        }
    }
    $categories.plugins = [ordered]@{
        category = "plugins"
        roots = @("projects/*/games")
        total = $plugins.Count
        returned = $plugins.Count
        truncated = $false
        items = @($plugins)
    }
    return $categories
}

function Get-ProjectSummary([int]$Limit) {
    $projects = Read-JsonIfExists "projects.json"
    $presets = Read-JsonIfExists "CMakePresets.json"
    $declaredProjects = @()
    foreach ($entry in @((Get-PropertyValue $projects "projects" @()))) {
        $path = [string](Get-PropertyValue $entry "path" "")
        $exists = $false
        if ($path) { $exists = Test-Path -LiteralPath (Join-Path $root $path) -PathType Container }
        $declaredProjects += [ordered]@{
            name = [string](Get-PropertyValue $entry "name" "")
            path = $path
            exists = $exists
        }
    }
    $configureNames = @((Get-PropertyValue $presets "configurePresets" @()) | ForEach-Object { [string](Get-PropertyValue $_ "name" "") } | Where-Object { $_ })
    $buildNames = @((Get-PropertyValue $presets "buildPresets" @()) | ForEach-Object { [string](Get-PropertyValue $_ "name" "") } | Where-Object { $_ })
    return [ordered]@{
        root = "."
        metadata = [ordered]@{
            projectsFile = if ($projects) { "projects.json" } else { "" }
            cmakePresetsFile = if ($presets) { "CMakePresets.json" } else { "" }
            projectCount = $declaredProjects.Count
        }
        declaredProjects = @($declaredProjects)
        build = [ordered]@{
            configurePresets = @($configureNames)
            buildPresets = @($buildNames)
            defaultBuildDirectory = "out/build/x64-Release"
            executable = "out/build/x64-Release/MikanEngine.exe"
            gameplayTestExecutable = "out/build/x64-Release/MikanTestRunner.exe"
        }
        inventory = Get-ProjectInventory $Limit
    }
}

function Get-ProjectManifestState([string]$InputProjectPath) {
    $projectFull = Resolve-ProjectDirectory $InputProjectPath $true
    $manifestFull = Join-Path $projectFull "project.json"
    if (-not (Test-Path -LiteralPath $manifestFull -PathType Leaf)) {
        throw "项目目录缺少 project.json: $projectFull"
    }
    try {
        $manifest = [System.IO.File]::ReadAllText($manifestFull, $utf8NoBom) | ConvertFrom-Json
    } catch {
        throw "项目 manifest JSON 解析失败: $manifestFull；$($_.Exception.Message)"
    }

    $resourceRootValue = [string](Get-PropertyValue $manifest "resourceRoot" ".")
    if ([string]::IsNullOrWhiteSpace($resourceRootValue)) { $resourceRootValue = "." }
    if ([System.IO.Path]::IsPathRooted($resourceRootValue)) {
        throw "project.json.resourceRoot 必须是项目内相对路径: $resourceRootValue"
    }
    $resourceFull = [System.IO.Path]::GetFullPath((Join-Path $projectFull $resourceRootValue))
    if (-not (Test-PathWithin $resourceFull $projectFull)) {
        throw "project.json.resourceRoot 不能逃逸项目目录: $resourceRootValue"
    }

    $assetRoots = New-Object 'System.Collections.Generic.List[object]'
    foreach ($entryValue in @((Get-PropertyValue $manifest "assets" @()))) {
        $entry = [string]$entryValue
        if ([string]::IsNullOrWhiteSpace($entry)) { continue }
        if ([System.IO.Path]::IsPathRooted($entry)) {
            throw "project.json.assets 只能使用项目内相对路径: $entry"
        }
        $entryFull = [System.IO.Path]::GetFullPath((Join-Path $projectFull $entry))
        if (-not (Test-PathWithin $entryFull $projectFull)) {
            throw "project.json.assets 不能逃逸项目目录: $entry"
        }
        $normalized = Get-PathRelativeTo $entryFull $projectFull
        if ($normalized -eq ".") { $normalized = "" }
        $assetRoots.Add([pscustomobject][ordered]@{
            path = $normalized
            fullPath = $entryFull
            exists = (Test-Path -LiteralPath $entryFull)
            isDirectory = (Test-Path -LiteralPath $entryFull -PathType Container)
        })
    }

    [void](Add-SourceArtifact (Get-RelativePath $manifestFull) "project_manifest")
    return [pscustomobject][ordered]@{
        projectRoot = $projectFull
        projectRelativePath = Get-RelativePath $projectFull
        manifestPath = $manifestFull
        manifest = $manifest
        name = [string](Get-PropertyValue $manifest "name" "")
        scene = [string](Get-PropertyValue $manifest "scene" "")
        game = [string](Get-PropertyValue $manifest "game" "")
        resourceRoot = $resourceRootValue
        resourceRootFull = $resourceFull
        resourceRootRelativePath = Get-PathRelativeTo $resourceFull $projectFull
        assetRoots = @($assetRoots.ToArray())
    }
}

function Resolve-ProjectContextReferenceFull([string]$Value, $State) {
    if ([string]::IsNullOrWhiteSpace($Value)) { return "" }
    $clean = ([string]$Value).Replace('\', '/').Split('?')[0].TrimStart('/')
    while ($clean.StartsWith('../', [System.StringComparison]::Ordinal)) { $clean = $clean.Substring(3) }
    if ([string]::IsNullOrWhiteSpace($clean)) { return "" }

    if ($clean.StartsWith('engine/', [System.StringComparison]::OrdinalIgnoreCase)) {
        $candidate = [System.IO.Path]::GetFullPath((Join-Path $root $clean))
        if (Test-PathWithin $candidate $root -and (Test-Path -LiteralPath $candidate -PathType Leaf)) { return $candidate }
        return ""
    }

    $base = if ($clean.StartsWith('assets/', [System.StringComparison]::OrdinalIgnoreCase)) {
        [string]$State.projectRoot
    } else {
        [string]$State.resourceRootFull
    }
    $candidate = [System.IO.Path]::GetFullPath((Join-Path $base $clean))
    if (-not (Test-PathWithin $candidate $State.projectRoot)) { return "" }
    if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
    return ""
}

function Get-ProjectAssetType([string]$RelativePath) {
    $extension = [System.IO.Path]::GetExtension($RelativePath).ToLowerInvariant()
    $normalized = $RelativePath.Replace('\', '/').ToLowerInvariant()
    if ($extension -eq '.json' -and ($normalized -match '(^|/)scenes?/' -or $normalized -match '(^|/)scene[^/]*\.json$' -or $normalized -match '(^|/)main\.json$')) { return 'scenes' }
    if ($extension -in @('.cpp', '.cc', '.cxx', '.c', '.h', '.hh', '.hpp', '.hxx', '.cs', '.lua', '.js', '.ts')) { return 'scripts' }
    if ($extension -in @('.glsl', '.vert', '.frag', '.geom', '.tesc', '.tese', '.comp', '.hlsl', '.shader', '.spv')) { return 'shaders' }
    if ($extension -in @('.obj', '.fbx', '.gltf', '.glb', '.dae', '.ply', '.vox')) { return 'models' }
    if ($extension -in @('.png', '.jpg', '.jpeg', '.tga', '.dds', '.ktx', '.ktx2', '.hdr', '.exr')) { return 'textures' }
    if ($extension -in @('.wav', '.ogg', '.mp3', '.flac')) { return 'audio' }
    return 'other'
}

function Get-ProjectSemanticRole([string]$Type, [string]$RelativePath) {
    $normalized = $RelativePath.Replace('\', '/').ToLowerInvariant()
    $name = [System.IO.Path]::GetFileNameWithoutExtension($normalized)
    switch ($Type) {
        'scenes' { return 'scene' }
        'scripts' { if ($normalized -match '(^|/)games/') { return 'gameplay_script' }; return 'script' }
        'shaders' { return 'shader' }
        'models' { if ($normalized -match '(^|/)(animation|animations|anim)(/|_|-)') { return 'animation_model' }; return 'model' }
        'textures' {
            if ($name -match '(albedo|base.?color|diffuse)') { return 'material_base_color' }
            if ($name -match '(normal|nrm)') { return 'material_normal' }
            if ($name -match '(roughness|metallic|orm|rma)') { return 'material_pbr' }
            if ($name -match '(emissive|emission)') { return 'material_emissive' }
            if ($name -match '(hdr|environment|sky)') { return 'environment' }
            if ($normalized -match '(^|/)(ui|icon|icons|font)(/|_)') { return 'ui_texture' }
            return 'texture'
        }
        'audio' { return 'audio' }
        default { return 'asset' }
    }
}

function Get-ProjectSemanticTags([string]$RelativePath) {
    $tokens = @($RelativePath.Replace('\', '/') -split '[/_\- .]+' | Where-Object { $_ -and $_.Length -ge 2 } | Select-Object -Unique -First 16)
    return @($tokens)
}

function Test-ProjectManifestRegistration([string]$RelativePath, $State) {
    $normalized = $RelativePath.Replace('\', '/').TrimStart('./')
    foreach ($entry in @($State.assetRoots)) {
        $rootPath = ([string]$entry.path).Replace('\', '/').TrimStart('./')
        if ([string]::IsNullOrWhiteSpace($rootPath) -or
            $normalized.Equals($rootPath, [System.StringComparison]::OrdinalIgnoreCase) -or
            $normalized.StartsWith($rootPath + '/', [System.StringComparison]::OrdinalIgnoreCase)) { return $true }
    }
    return $false
}

function Add-ProjectAssetCandidate($Candidates, [System.IO.FileInfo]$File, [string]$RelativePath, [string]$Source, [bool]$Registered, [string]$ReferenceField = '') {
    $key = if ($null -ne $File) { $File.FullName.ToLowerInvariant() } else { 'missing:' + $RelativePath.ToLowerInvariant() }
    if (-not $Candidates.ContainsKey($key)) {
        $Candidates[$key] = [pscustomobject]@{
            file = $File
            relativePath = $RelativePath.Replace('\', '/')
            registered = $Registered
            sources = New-Object 'System.Collections.Generic.List[string]'
            references = New-Object 'System.Collections.Generic.List[object]'
            missingReason = if ($null -eq $File) { 'referenced_or_registered_path_not_found' } else { '' }
        }
    }
    $candidate = $Candidates[$key]
    if ($Registered) { $candidate.registered = $true }
    if ($Source -and -not $candidate.sources.Contains($Source)) { [void]$candidate.sources.Add($Source) }
    if ($ReferenceField) {
        [void]$candidate.references.Add([ordered]@{ field = $ReferenceField; source = $Source })
    }
}

function Add-ProjectSceneStringReferences($Value, [string]$FieldPath, [int]$Depth, [System.Collections.Generic.List[object]]$Results, $State) {
    if ($Results.Count -ge $MaxAssetReferences -or $Depth -gt 20 -or $null -eq $Value) { return }
    if ($Value -is [string]) {
        $text = [string]$Value
        $pathKey = $FieldPath -match '(?i)(path|file|clip|texture|model|vox|heightmap|albedo|normal|roughness|metallic|ao|emissive|layer\d|tmx|tilemap)'
        $knownExtension = $text -match '(?i)\.(obj|fbx|gltf|glb|dae|ply|vox|png|jpg|jpeg|tga|dds|ktx|ktx2|hdr|exr|wav|ogg|mp3|flac|tmx|json)(\?.*)?$'
        if (($pathKey -or $knownExtension) -and -not [string]::IsNullOrWhiteSpace($text)) {
            $resolvedFull = Resolve-ProjectContextReferenceFull $text $State
            $isEngine = $text.Replace('\', '/').TrimStart('/') -match '(?i)^engine/'
            $scope = if ($isEngine) { 'engine' } else { 'project' }
            $relative = if ($resolvedFull) {
                if ($isEngine) { Get-RelativePath $resolvedFull } else { Get-PathRelativeTo $resolvedFull $State.projectRoot }
            } else { $text.Replace('\', '/').TrimStart('/') }
            [void]$Results.Add([ordered]@{
                field = $FieldPath
                value = $text
                path = $relative
                type = if ($isEngine) { Get-AssetTypeForReference $text } else { Get-ProjectAssetType $relative }
                scope = $scope
                resolvedPath = if ($resolvedFull) { Get-RelativePath $resolvedFull } else { '' }
                exists = [bool]$resolvedFull
                registered = if ($resolvedFull -and -not $isEngine) { Test-ProjectManifestRegistration (Get-PathRelativeTo $resolvedFull $State.projectRoot) $State } else { $false }
            })
        }
        return
    }
    if ($Value -is [System.Collections.IEnumerable] -and -not ($Value -is [System.Collections.IDictionary])) {
        $index = 0
        foreach ($item in $Value) {
            Add-ProjectSceneStringReferences $item ("{0}[{1}]" -f $FieldPath, $index) ($Depth + 1) $Results $State
            $index++
            if ($Results.Count -ge $MaxAssetReferences) { return }
        }
        return
    }
    foreach ($property in @($Value.PSObject.Properties)) {
        Add-ProjectSceneStringReferences $property.Value ("{0}.{1}" -f $FieldPath, $property.Name) ($Depth + 1) $Results $State
        if ($Results.Count -ge $MaxAssetReferences) { return }
    }
}

function Get-ProjectSceneReferenceInfo($State) {
    $sceneFull = Resolve-ProjectContextReferenceFull ([string]$State.scene) $State
    $references = New-Object 'System.Collections.Generic.List[object]'
    $parseError = ''
    if ($sceneFull) {
        [void](Add-SourceArtifact (Get-RelativePath $sceneFull) 'project_scene')
        try {
            $scene = [System.IO.File]::ReadAllText($sceneFull, $utf8NoBom) | ConvertFrom-Json
            Add-ProjectSceneStringReferences $scene 'scene' 0 $references $State
        } catch { $parseError = $_.Exception.Message }
    }
    return [ordered]@{
        path = if ($sceneFull) { Get-PathRelativeTo $sceneFull $State.projectRoot } else { [string]$State.scene }
        exists = [bool]$sceneFull
        parseable = [bool]($sceneFull -and -not $parseError)
        parseError = $parseError
        references = @($references.ToArray())
    }
}

function New-ProjectAssetRecord($Candidate, $State, [bool]$IncludeHash) {
    $relative = [string]$Candidate.relativePath
    $type = if ($Candidate.file) { Get-ProjectAssetType $relative } else { Get-ProjectAssetType $relative }
    $record = [ordered]@{
        path = $relative
        enginePath = if ($Candidate.file) { Get-RelativePath $Candidate.file.FullName } else { '' }
        name = [System.IO.Path]::GetFileNameWithoutExtension($relative)
        type = $type
        semanticRole = Get-ProjectSemanticRole $type $relative
        tags = Get-ProjectSemanticTags $relative
        extension = [System.IO.Path]::GetExtension($relative).ToLowerInvariant()
        exists = [bool]$Candidate.file
        registered = [bool]$Candidate.registered
        sources = @($Candidate.sources.ToArray())
        references = @($Candidate.references.ToArray())
        missingReason = [string]$Candidate.missingReason
    }
    if ($Candidate.file) {
        $record.size = [int64]$Candidate.file.Length
        $record.modifiedAt = $Candidate.file.LastWriteTimeUtc.ToString('o')
        if ($IncludeHash) { $record.sha256 = Get-Sha256 $Candidate.file.FullName }
    } else {
        $record.size = $null
        $record.modifiedAt = ''
        if ($IncludeHash) { $record.sha256 = '' }
    }
    return [pscustomobject]$record
}

function Get-ProjectAssetIndex($State, [string]$SearchQuery, [string]$RequestedType, [int]$Limit, [bool]$IncludeHash = $false) {
    $candidates = @{}
    foreach ($entry in @($State.assetRoots)) {
        $entryPath = [string]$entry.path
        if (-not $entry.exists) {
            Add-ProjectAssetCandidate $candidates $null $entryPath 'manifest' $true
            continue
        }
        if ($entry.isDirectory) {
            foreach ($file in @(Get-ChildItem -LiteralPath $entry.fullPath -File -Recurse -Force -ErrorAction SilentlyContinue)) {
                if (Test-ExcludedFile $file) { continue }
                Add-ProjectAssetCandidate $candidates $file (Get-PathRelativeTo $file.FullName $State.projectRoot) 'manifest' $true
            }
        } else {
            $file = Get-Item -LiteralPath $entry.fullPath
            Add-ProjectAssetCandidate $candidates $file $entryPath 'manifest' $true
        }
    }

    $sceneInfo = Get-ProjectSceneReferenceInfo $State
    $sceneRelative = [string]$sceneInfo.path
    $sceneFull = Resolve-ProjectContextReferenceFull ([string]$State.scene) $State
    if ($sceneFull) {
        $sceneFile = Get-Item -LiteralPath $sceneFull
        Add-ProjectAssetCandidate $candidates $sceneFile $sceneRelative 'manifest_scene' (Test-ProjectManifestRegistration $sceneRelative $State)
    } elseif ($sceneRelative) {
        Add-ProjectAssetCandidate $candidates $null $sceneRelative 'manifest_scene' (Test-ProjectManifestRegistration $sceneRelative $State)
    }
    foreach ($reference in @($sceneInfo.references)) {
        if ([string]$reference.scope -ne 'project') { continue }
        $referencePath = [string]$reference.path
        $resolvedFull = Resolve-ProjectContextReferenceFull ([string]$reference.value) $State
        if ($resolvedFull) {
            Add-ProjectAssetCandidate $candidates (Get-Item -LiteralPath $resolvedFull) $referencePath 'scene_reference' ([bool]$reference.registered) ([string]$reference.field)
        } elseif ($referencePath) {
            Add-ProjectAssetCandidate $candidates $null $referencePath 'scene_reference' ([bool]$reference.registered) ([string]$reference.field)
        }
    }

    $needle = if ($SearchQuery) { $SearchQuery.ToLowerInvariant() } else { '' }
    $tokens = @($needle -split '[^a-z0-9\u4e00-\u9fff]+' | Where-Object { $_ })
    $ranked = New-Object 'System.Collections.Generic.List[object]'
    foreach ($candidate in @($candidates.Values)) {
        $record = New-ProjectAssetRecord $candidate $State $IncludeHash
        if ($RequestedType -ne 'all' -and [string]$record.type -ne $RequestedType) { continue }
        $score = 0
        if ($needle) {
            $blob = (([string]$record.path) + ' ' + ([string]$record.name) + ' ' + ([string]$record.semanticRole) + ' ' + ((@($record.tags) -join ' ')) + ' ' + ((@($record.sources) -join ' '))).ToLowerInvariant()
            foreach ($token in $tokens) {
                if ($blob.IndexOf($token, [System.StringComparison]::Ordinal) -ge 0) { $score += 1 }
            }
            if ($score -eq 0) { continue }
            if (([string]$record.path).ToLowerInvariant().IndexOf($needle, [System.StringComparison]::Ordinal) -ge 0) { $score += 4 }
            if (([string]$record.semanticRole).ToLowerInvariant().IndexOf($needle, [System.StringComparison]::Ordinal) -ge 0) { $score += 3 }
            $record | Add-Member -MemberType NoteProperty -Name matchScore -Value $score
        }
        [void]$ranked.Add([pscustomobject]@{ score = $score; path = [string]$record.path; record = $record })
    }
    $ordered = if ($needle) { @(($ranked | Sort-Object @{ Expression = 'score'; Descending = $true }, @{ Expression = 'path'; Descending = $false })) } else { @(($ranked | Sort-Object @{ Expression = 'path'; Descending = $false })) }
    $totalMatches = @($ordered).Count
    $selected = @($ordered | Select-Object -First $Limit | ForEach-Object { $_.record })
    return [ordered]@{
        projectPath = [string]$State.projectRelativePath
        resourceRoot = [string]$State.resourceRootRelativePath
        query = $SearchQuery
        assetType = $RequestedType
        totalMatches = $totalMatches
        returned = $selected.Count
        truncated = ($totalMatches -gt $selected.Count)
        items = @($selected)
    }
}

function Get-ProjectContext([string]$InputProjectPath, [string]$SearchQuery, [string]$RequestedType, [int]$Limit, [bool]$IncludeHash = $false) {
    $state = Get-ProjectManifestState $InputProjectPath
    $script:selectedProjectRoot = $state.projectRoot
    $script:selectedResourceRoot = $state.resourceRootFull
    $script:selectedProjectManifestPath = $state.manifestPath
    $sceneInfo = Get-ProjectSceneReferenceInfo $state
    $assetIndex = Get-ProjectAssetIndex $state $SearchQuery $RequestedType $Limit $IncludeHash
    $assetRootSummary = @($state.assetRoots | ForEach-Object {
        [ordered]@{
            path = [string]$_.path
            exists = [bool]$_.exists
            type = if ($_.exists -and -not $_.isDirectory) { Get-ProjectAssetType ([string]$_.path) } else { 'directory' }
            fileCount = if ($_.exists -and $_.isDirectory) { @((Get-ChildItem -LiteralPath $_.fullPath -File -Recurse -Force -ErrorAction SilentlyContinue)).Count } elseif ($_.exists) { 1 } else { 0 }
        }
    })
    return [ordered]@{
        projectPath = [string]$state.projectRelativePath
        root = [string]$state.projectRelativePath
        manifest = [ordered]@{
            path = Get-RelativePath $state.manifestPath
            name = [string]$state.name
            scene = [string]$state.scene
            game = [string]$state.game
            resourceRoot = [string]$state.resourceRootRelativePath
            assets = @($state.assetRoots | ForEach-Object { [string]$_.path })
        }
        resource = [ordered]@{
            root = [string]$state.resourceRootRelativePath
            absolutePath = Get-RelativePath $state.resourceRootFull
            exists = (Test-Path -LiteralPath $state.resourceRootFull -PathType Container)
        }
        defaultScene = $sceneInfo
        assetRoots = $assetRootSummary
        assetIndex = $assetIndex
        queryContract = [ordered]@{
            scenePath = '项目相对路径，例如 scenes/main.json'
            assetPath = '项目相对路径；写入场景时保持引擎的相对资源路径语义'
            enginePath = '引擎资产使用 engine/ 前缀，项目资产不使用 engine/ 前缀'
            indexScope = '仅包含 project.json.assets[]、默认场景及其项目资源引用，不扫描 out/backup/第三方依赖'
        }
    }
}

function Get-SceneSchemaSummary {
    $schemaPath = Join-Path $root "tools\scene_schema.json"
    $schema = Read-JsonIfExists "tools/scene_schema.json"
    $components = @((Get-PropertyValue $schema "components" @()))
    $keys = @($components | ForEach-Object { [string](Get-PropertyValue $_ "serializeKey" "") } | Where-Object { $_ })
    $categories = @($components | ForEach-Object { [string](Get-PropertyValue $_ "category" "") } | Where-Object { $_ } | Sort-Object -Unique)
    $fieldCount = 0
    foreach ($component in $components) { $fieldCount += @((Get-PropertyValue $component "fields" @())).Count }
    return [ordered]@{
        path = if (Test-Path -LiteralPath $schemaPath -PathType Leaf) { "tools/scene_schema.json" } else { "" }
        exists = (Test-Path -LiteralPath $schemaPath -PathType Leaf)
        sha256 = if (Test-Path -LiteralPath $schemaPath -PathType Leaf) { Get-Sha256 $schemaPath } else { "" }
        componentCount = $components.Count
        serializableComponentCount = $keys.Count
        fieldCount = $fieldCount
        categories = @($categories)
        componentKeys = @($keys)
    }
}

function Get-ExternalToolPath([string]$RequestedPath, [string[]]$CommandNames = @(), [string[]]$CandidatePaths = @()) {
    if (-not [string]::IsNullOrWhiteSpace($RequestedPath)) {
        try {
            $requestedFull = [System.IO.Path]::GetFullPath($RequestedPath)
            if (Test-Path -LiteralPath $requestedFull -PathType Leaf) { return $requestedFull }
        } catch {}
    }
    foreach ($name in @($CommandNames)) {
        if ([string]::IsNullOrWhiteSpace([string]$name)) { continue }
        $command = Get-Command $name -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -ne $command) {
            $path = [string](Get-PropertyValue $command "Source" (Get-PropertyValue $command "Path" ""))
            if ($path -and (Test-Path -LiteralPath $path -PathType Leaf)) { return [System.IO.Path]::GetFullPath($path) }
        }
    }
    foreach ($candidate in @($CandidatePaths)) {
        if ([string]::IsNullOrWhiteSpace([string]$candidate)) { continue }
        try {
            $candidateFull = [System.IO.Path]::GetFullPath([string]$candidate)
            if (Test-Path -LiteralPath $candidateFull -PathType Leaf) { return $candidateFull }
        } catch {}
    }
    return ""
}

function Get-ToolVersion([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) { return "" }
    try { return [string]([System.Diagnostics.FileVersionInfo]::GetVersionInfo($Path).FileVersion) } catch { return "" }
}

function Invoke-ReadOnlyCommand([string]$Path, [string[]]$Arguments, [int]$TimeoutSeconds = 15) {
    $result = [ordered]@{
        path = $Path
        arguments = @($Arguments)
        status = "not_run"
        exitCode = $null
        timedOut = $false
        output = ""
        error = ""
    }
    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        $result.status = "unavailable"
        return $result
    }
    $process = $null
    try {
        $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
        $startInfo.FileName = $Path
        $startInfo.UseShellExecute = $false
        $startInfo.CreateNoWindow = $true
        $startInfo.RedirectStandardOutput = $true
        $startInfo.RedirectStandardError = $true
        $startInfo.Arguments = [string]::Join(" ", @($Arguments | ForEach-Object {
            $argument = [string]$_
            if ($argument -match '[\s"]') { '"' + $argument.Replace('"', '\"') + '"' } else { $argument }
        }))
        $process = [System.Diagnostics.Process]::new()
        $process.StartInfo = $startInfo
        [void]$process.Start()
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $result.status = "timeout"
            $result.timedOut = $true
            try { $process.Kill() } catch {}
            [void]$process.WaitForExit(2000)
        } else {
            $result.status = "completed"
        }
        try { $result.output = [string]$stdoutTask.Result } catch {}
        try { $result.error = [string]$stderrTask.Result } catch {}
        if ($process.HasExited) { $result.exitCode = [int]$process.ExitCode }
        if ($result.status -eq "completed" -and $result.exitCode -ne 0) { $result.status = "failed" }
    } catch {
        $result.status = "error"
        $result.error = $_.Exception.Message
    } finally {
        if ($null -ne $process) { $process.Dispose() }
    }
    return $result
}

function Get-VulkanDeviceRecords([string]$Text) {
    $devices = New-Object 'System.Collections.Generic.List[object]'
    $current = $null
    foreach ($line in @($Text -split "\r?\n")) {
        if ($line -match '^\s*GPU(\d+):\s*$') {
            if ($null -ne $current) { [void]$devices.Add($current) }
            $current = [ordered]@{ index = [int]$Matches[1] }
            continue
        }
        if ($null -eq $current) { continue }
        if ($line -match '^\s*(apiVersion|driverVersion|vendorID|deviceID|deviceType|deviceName|driverID|driverName|driverInfo|conformanceVersion)\s*=\s*(.*?)\s*$') {
            $current[[string]$Matches[1]] = [string]$Matches[2]
        }
    }
    if ($null -ne $current) { [void]$devices.Add($current) }
    return @($devices.ToArray())
}

function Get-DeviceVendor([string]$Name, [string]$VendorId, [string]$DriverName) {
    $value = "$Name $VendorId $DriverName".ToLowerInvariant()
    if ($value -match '0x10de|nvidia') { return "nvidia" }
    if ($value -match '0x1002|amd|radeon') { return "amd" }
    if ($value -match '0x8086|intel') { return "intel" }
    return "unknown"
}

function Get-DeviceCapabilities {
    if (-not [string]::IsNullOrWhiteSpace($VulkanInfoPath)) {
        $vulkanLeaf = [System.IO.Path]::GetFileName($VulkanInfoPath)
        if ($vulkanLeaf -notin @("vulkaninfo.exe", "vulkaninfo")) { throw "VulkanInfoPath 只能指向 vulkaninfo.exe" }
    }
    $vulkanCandidates = New-Object 'System.Collections.Generic.List[string]'
    if ($env:VULKAN_SDK) { [void]$vulkanCandidates.Add((Join-Path $env:VULKAN_SDK "Bin\vulkaninfo.exe")) }
    if ($env:WINDIR) { [void]$vulkanCandidates.Add((Join-Path $env:WINDIR "System32\vulkaninfo.exe")) }
    $vulkanInfo = Get-ExternalToolPath $VulkanInfoPath @("vulkaninfo.exe", "vulkaninfo") @($vulkanCandidates.ToArray())
    $vulkanProbe = Invoke-ReadOnlyCommand $vulkanInfo @("--summary") 20
    $vulkanDevices = @(Get-VulkanDeviceRecords ([string]$vulkanProbe.output))
    $vendors = @($vulkanDevices | ForEach-Object { Get-DeviceVendor ([string]$_.deviceName) ([string]$_.vendorID) ([string]$_.driverName) } | Sort-Object -Unique)
    $hasNvidia = $vendors -contains "nvidia"
    $instanceVersion = ""
    if ([string]$vulkanProbe.output -match '(?m)^Vulkan Instance Version:\s*(.+?)\s*$') { $instanceVersion = [string]$Matches[1] }

    $renderDocRequested = $RenderDocPath
    if ($renderDocRequested -and (Test-Path -LiteralPath $renderDocRequested -PathType Container)) {
        $renderDocRequested = Join-Path $renderDocRequested "qrenderdoc.exe"
    }
    $renderDocUi = Get-ExternalToolPath $renderDocRequested @("qrenderdoc.exe")
   $renderDocCmdRequested = if ($renderDocUi) { Join-Path (Split-Path -Parent $renderDocUi) "renderdoccmd.exe" } else { "" }
   $renderDocCmd = Get-ExternalToolPath $renderDocCmdRequested @("renderdoccmd.exe")

   $nsightCandidates = New-Object 'System.Collections.Generic.List[string]'
    $programRoots = @($env:ProgramFiles, $env:ProgramW6432, ${env:ProgramFiles(x86)}) + @(Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Root)
    foreach ($programRoot in @($programRoots | Where-Object { $_ } | Sort-Object -Unique)) {
        $pattern = Join-Path ([string]$programRoot) "Program Files\NVIDIA Corporation\Nsight Graphics*\host\windows-desktop-nomad-x64\ngfx-ui.exe"
        if ([string]$programRoot -match '(?i)\\Program Files\\?$') {
            $pattern = Join-Path ([string]$programRoot) "NVIDIA Corporation\Nsight Graphics*\host\windows-desktop-nomad-x64\ngfx-ui.exe"
        }
        foreach ($candidate in @(Get-ChildItem -Path $pattern -File -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName)) {
            [void]$nsightCandidates.Add([string]$candidate)
        }
    }
    $nsightRequested = $NsightPath
    if ($nsightRequested -and (Test-Path -LiteralPath $nsightRequested -PathType Container)) {
        $nsightRequested = Join-Path $nsightRequested "ngfx-ui.exe"
    }
    $nsightUi = Get-ExternalToolPath $nsightRequested @("ngfx-ui.exe") @($nsightCandidates.ToArray())
    $nsightDir = if ($nsightUi) { Split-Path -Parent $nsightUi } else { "" }
    $nsightTraceRequested = if ($nsightDir) { Join-Path $nsightDir "ngfx.exe" } else { "" }
    $nsightCaptureRequested = if ($nsightDir) { Join-Path $nsightDir "ngfx-capture.exe" } else { "" }
    $nsightReplayRequested = if ($nsightDir) { Join-Path $nsightDir "ngfx-replay.exe" } else { "" }
    $nsightTrace = Get-ExternalToolPath $nsightTraceRequested @("ngfx.exe")
    $nsightCapture = Get-ExternalToolPath $nsightCaptureRequested @("ngfx-capture.exe")
    $nsightReplay = Get-ExternalToolPath $nsightReplayRequested @("ngfx-replay.exe")

    $engineArtifactPaths = @("out/build/x64-Release/MikanEngine.exe", "out/build/x64-Release/MikanTestRunner.exe", "out/build/x64-Release/Game.dll")
    $engineArtifacts = @($engineArtifactPaths | ForEach-Object {
        $full = Join-Path $root ($_ -replace '/', '\')
        [ordered]@{ path = $_; exists = (Test-Path -LiteralPath $full -PathType Leaf); size = if (Test-Path -LiteralPath $full -PathType Leaf) { [int64](Get-Item -LiteralPath $full).Length } else { 0 }; sha256 = if (Test-Path -LiteralPath $full -PathType Leaf) { Get-Sha256 $full } else { "" } }
    })
    $adapterRecords = @()
    try {
        $adapterRecords = @(Get-CimInstance Win32_VideoController -ErrorAction Stop | ForEach-Object {
            [ordered]@{ name = [string]$_.Name; driverVersion = [string]$_.DriverVersion; pnpDeviceId = [string]$_.PNPDeviceID }
        })
    } catch {}
    $processArchitecture = [string]$env:PROCESSOR_ARCHITECTURE
    try {
        $runtimeArchitecture = [string][System.Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture
        if ($runtimeArchitecture) { $processArchitecture = $runtimeArchitecture }
    } catch {}
    $vulkanStatus = if ($vulkanProbe.status -eq "completed" -and $vulkanProbe.exitCode -eq 0 -and $vulkanDevices.Count -gt 0) { "available" } elseif ($vulkanProbe.status -eq "unavailable") { "unavailable" } else { "error" }
    $renderDocStatus = if ($renderDocCmd) { "available" } elseif ($renderDocUi) { "installed_but_capture_command_missing" } else { "unavailable" }
    $nsightStatus = if (-not $nsightUi -and -not $nsightTrace -and -not $nsightCapture) { "unavailable" } elseif ($vendors.Count -gt 0 -and -not $hasNvidia) { "unsupported_for_detected_gpu" } else { "available_not_profiled" }
    $recommendations = New-Object 'System.Collections.Generic.List[string]'
    if ($vulkanStatus -ne "available") { [void]$recommendations.Add("Vulkan 设备枚举不可用；在执行渲染测试前确认驱动、loader 和 vulkaninfo。") }
    if ($renderDocStatus -eq "installed_but_capture_command_missing") { [void]$recommendations.Add("RenderDoc UI 已发现，但缺少 renderdoccmd.exe；不能把普通截图当作 RenderDoc 视觉证据。") }
    if ($renderDocStatus -eq "unavailable") { [void]$recommendations.Add("未发现 RenderDoc 命令行捕获器；如需视觉证据，提供 RenderDocPath 或安装桌面端 RenderDoc。") }
    if ($nsightStatus -eq "unsupported_for_detected_gpu") { [void]$recommendations.Add("Nsight 工具已发现，但当前 Vulkan GPU 列表没有 NVIDIA 设备；不要把 Nsight GPU Trace 作为本机性能证据。") }
    elseif ($nsightStatus -eq "available_not_profiled") { [void]$recommendations.Add("Nsight 工具已发现，但性能权限尚未探测；执行 capture_performance 后再判断 permission_denied。") }
    return [ordered]@{
        schemaVersion = 1
        status = if ($vulkanStatus -eq "available") { "ready_for_desktop_probe" } else { "partial" }
        host = [ordered]@{
            os = [System.Environment]::OSVersion.VersionString
            platform = "windows"
            processArchitecture = $processArchitecture
            is64BitOperatingSystem = [System.Environment]::Is64BitOperatingSystem
            is64BitProcess = [System.Environment]::Is64BitProcess
            processorCount = [System.Environment]::ProcessorCount
            powershell = $PSVersionTable.PSVersion.ToString()
        }
        vulkan = [ordered]@{
            loader = [ordered]@{ exists = if ($env:WINDIR) { Test-Path -LiteralPath (Join-Path $env:WINDIR "System32\vulkan-1.dll") } else { $false } }
            vulkanInfo = [ordered]@{ path = $vulkanInfo; version = Get-ToolVersion $vulkanInfo; probeStatus = $vulkanProbe.status; exitCode = $vulkanProbe.exitCode; timedOut = $vulkanProbe.timedOut }
            instanceVersion = $instanceVersion
            devices = @($vulkanDevices)
            vendors = @($vendors)
            status = $vulkanStatus
            warning = ([string]$vulkanProbe.error).Trim()
        }
        adapters = @($adapterRecords)
        renderDoc = [ordered]@{
            status = $renderDocStatus
            uiPath = $renderDocUi
            commandPath = $renderDocCmd
            uiVersion = Get-ToolVersion $renderDocUi
            commandVersion = Get-ToolVersion $renderDocCmd
            captureReady = [bool]$renderDocCmd
        }
        nsight = [ordered]@{
            status = $nsightStatus
            uiPath = $nsightUi
            tracePath = $nsightTrace
            capturePath = $nsightCapture
            replayPath = $nsightReplay
            uiVersion = Get-ToolVersion $nsightUi
            gpuTraceReady = [bool]($nsightTrace -and ($hasNvidia -or $vendors.Count -eq 0))
            graphicsCaptureReady = [bool]($nsightCapture -and ($hasNvidia -or $vendors.Count -eq 0))
            permission = "not_tested"
        }
        engine = [ordered]@{
            artifacts = @($engineArtifacts)
            projectRoot = Get-RelativePath $root
        }
        mobile = [ordered]@{
            status = "deferred"
            supportedTargets = @("android-arm64")
            note = "本阶段只查询 Windows 桌面能力；Android install/run/logcat 仍未接入。"
        }
        recommendations = @($recommendations.ToArray())
    }
}

function Get-Capabilities([string]$SourceMode) {
    return [ordered]@{
        context = [ordered]@{
            inspectProject = $true
            getProjectContext = $true
            inspectScene = $true
            queryAssets = $true
            projectRootAware = $true
            semanticAssetIndex = $true
            deviceCapabilities = $true
            sourceMode = $SourceMode
        }
        workflowActions = @(
            "inspect_project", "get_project_context", "inspect_scene", "query_assets", "create_script", "build",
            "compile_games", "validate_scene", "apply_scene_commands", "run_gameplay_test",
            "run_render_test", "capture_frame", "capture_performance", "read_dump", "assert_state", "stop_engine"
        )
        mcpTools = @(
            "inspect_project", "get_project_context", "inspect_scene", "query_assets", "create_script", "build",
            "run_agent_workflow", "run_agent_task", "run_agent_plan", "collect_agent_evidence", "evaluate_agent_result",
            "capture_frame", "capture_performance", "validate_scene", "apply_scene_commands",
            "run_gameplay_test", "run_render_test", "read_dump", "assert_state", "engine_status", "stop_engine",
            "get_device_capabilities"
        )
        aiNativeLoop = @(
            "discover_context", "discover_device_capabilities", "generate_or_patch", "build", "validate", "run_deterministic_test",
            "capture_visual_or_performance_evidence", "summarize_result"
        )
        platform = [ordered]@{
            host = "windows"
            mobile = [ordered]@{ status = "deferred"; reason = "本阶段暂不把 Android 执行链路作为前置条件" }
        }
    }
}

function Get-EntityName($Entity) {
    $value = Get-PropertyValue $Entity "name" ""
    if ($value -is [string]) { return [string]$value }
    return [string](Get-PropertyValue $value "name" "")
}

function Get-ComponentKeys($SchemaSummary) {
    $keys = @{}
    foreach ($key in @((Get-PropertyValue $SchemaSummary "componentKeys" @()))) { $keys[[string]$key] = $true }
    return $keys
}

function Get-AssetTypeForReference([string]$Value) {
    $extension = [System.IO.Path]::GetExtension($Value).ToLowerInvariant()
    if ($extension -in @(".obj", ".fbx", ".gltf", ".glb", ".dae", ".ply", ".vox")) { return "models" }
    if ($extension -in @(".png", ".jpg", ".jpeg", ".tga", ".dds", ".ktx", ".ktx2", ".hdr", ".exr")) { return "textures" }
    if ($extension -in @(".wav", ".ogg", ".mp3", ".flac")) { return "audio" }
    if ($extension -eq ".json") { return "scenes" }
    return "reference"
}

function Resolve-Reference([string]$Value) {
    if ([string]::IsNullOrWhiteSpace($Value)) { return $null }
    if ($script:selectedProjectRoot -and $script:selectedResourceRoot) {
        $state = [pscustomobject]@{
            projectRoot = $script:selectedProjectRoot
            resourceRootFull = $script:selectedResourceRoot
        }
        $selectedFull = Resolve-ProjectContextReferenceFull $Value $state
        if ($selectedFull) { return Get-RelativePath $selectedFull }
    }
    $normalized = $Value.Replace('/', '\').TrimStart('\')
    $candidates = New-Object 'System.Collections.Generic.List[string]'
    if ([System.IO.Path]::IsPathRooted($Value)) {
        try { [void]$candidates.Add((Resolve-ProjectPath $Value $false)) } catch {}
    } else {
        foreach ($relative in @($normalized, (Join-Path "models" $normalized), (Join-Path "resources" $normalized))) {
            try { [void]$candidates.Add((Resolve-ProjectPath $relative $false)) } catch {}
        }
    }
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return Get-RelativePath $candidate }
    }
    return ""
}

function Resolve-SelectedScenePath([string]$InputScenePath) {
    if ($script:selectedProjectRoot -and $script:selectedResourceRoot -and -not [System.IO.Path]::IsPathRooted($InputScenePath)) {
        $clean = $InputScenePath.Replace('\', '/').TrimStart('/')
        $base = if ($clean.StartsWith('assets/', [System.StringComparison]::OrdinalIgnoreCase)) {
            $script:selectedProjectRoot
        } else {
            $script:selectedResourceRoot
        }
        $candidate = [System.IO.Path]::GetFullPath((Join-Path $base $clean))
        $candidateWithinProject = Test-PathWithin $candidate $script:selectedProjectRoot
        $candidateExists = Test-Path -LiteralPath $candidate -PathType Leaf
        if ($candidateWithinProject -and $candidateExists) { return $candidate }
    }
    return Resolve-ProjectPath $InputScenePath $true
}

function Add-SceneStringReferences($Value, [string]$FieldPath, [int]$Depth, [System.Collections.Generic.List[object]]$Results) {
    if ($Results.Count -ge $MaxAssetReferences -or $Depth -gt 20 -or $null -eq $Value) { return }
    if ($Value -is [string]) {
        $text = [string]$Value
        $pathKey = $FieldPath -match '(?i)(path|file|clip|texture|model|vox|heightmap|albedo|normal|roughness|metallic|ao|emissive|layer\d|tmx|tilemap)'
        $knownExtension = $text -match '(?i)\.(obj|fbx|gltf|glb|dae|ply|vox|png|jpg|jpeg|tga|dds|ktx|ktx2|hdr|exr|wav|ogg|mp3|flac|tmx|json)(\?.*)?$'
        if (($pathKey -or $knownExtension) -and -not [string]::IsNullOrWhiteSpace($text)) {
            $resolved = Resolve-Reference $text
            [void]$Results.Add([ordered]@{
                field = $FieldPath
                value = $text
                type = Get-AssetTypeForReference $text
                resolvedPath = $resolved
                exists = [bool]$resolved
            })
        }
        return
    }
    if ($Value -is [System.Collections.IEnumerable] -and -not ($Value -is [System.Collections.IDictionary])) {
        $index = 0
        foreach ($item in $Value) {
            Add-SceneStringReferences $item ("{0}[{1}]" -f $FieldPath, $index) ($Depth + 1) $Results
            $index++
            if ($Results.Count -ge $MaxAssetReferences) { return }
        }
        return
    }
    foreach ($property in @($Value.PSObject.Properties)) {
        Add-SceneStringReferences $property.Value ("{0}.{1}" -f $FieldPath, $property.Name) ($Depth + 1) $Results
        if ($Results.Count -ge $MaxAssetReferences) { return }
    }
}

function Get-ParentDepth([int]$Id, $ParentById) {
    $depth = 0
    $visited = @{}
    $current = $Id
    while ($ParentById.ContainsKey($current) -and $depth -lt 100) {
        if ($visited.ContainsKey($current)) { return $depth }
        $visited[$current] = $true
        $parent = [int64]$ParentById[$current]
        if ($parent -eq 4294967295 -or $parent -eq -1) { break }
        $current = [int]$parent
        $depth++
    }
    return $depth
}

function Get-SceneSummary([string]$InputScenePath, [int]$EntityLimit, [int]$ReferenceLimit) {
    $sceneFull = Resolve-SelectedScenePath $InputScenePath
    Add-SourceArtifact $sceneFull "scene_input" | Out-Null
    $schemaSummary = Get-SceneSchemaSummary
    Add-SourceArtifact "tools/scene_schema.json" "scene_schema" | Out-Null
    $scene = [System.IO.File]::ReadAllText($sceneFull, $utf8NoBom) | ConvertFrom-Json
    $entities = @((Get-PropertyValue $scene "entities" @()))
    $knownComponents = Get-ComponentKeys $schemaSummary
    $componentCounts = @{}
    $unknownComponents = @{}
    $parentById = @{}
    foreach ($entity in $entities) {
        $id = [int](Get-PropertyValue $entity "id" -1)
        $hierarchy = Get-PropertyValue $entity "hierarchy" $null
        if ($id -ge 0 -and $null -ne $hierarchy) { $parentById[$id] = [int64](Get-PropertyValue $hierarchy "parent" 4294967295) }
        foreach ($property in @($entity.PSObject.Properties)) {
            if ($property.Name -eq "id") { continue }
            $componentName = [string]$property.Name
            if (-not $componentCounts.ContainsKey($componentName)) { $componentCounts[$componentName] = 0 }
            $componentCounts[$componentName]++
            if (-not $knownComponents.ContainsKey($componentName)) { $unknownComponents[$componentName] = $true }
        }
    }
    $entityItems = New-Object 'System.Collections.Generic.List[object]'
    $scriptNames = @{}
    foreach ($entity in @($entities | Select-Object -First $EntityLimit)) {
        $id = [int](Get-PropertyValue $entity "id" -1)
        $hierarchy = Get-PropertyValue $entity "hierarchy" $null
        $components = @($entity.PSObject.Properties | Where-Object { $_.Name -ne "id" } | ForEach-Object { $_.Name } | Sort-Object)
        $scriptComponent = Get-PropertyValue $entity "script" $null
        $scriptName = [string](Get-PropertyValue $scriptComponent "scriptName" "")
        if ($scriptName) { $scriptNames[$scriptName] = $true }
        [void]$entityItems.Add([ordered]@{
            id = $id
            name = Get-EntityName $entity
            parentId = [int64](Get-PropertyValue $hierarchy "parent" 4294967295)
            childrenCount = @((Get-PropertyValue $hierarchy "children" @())).Count
            depth = Get-ParentDepth $id $parentById
            components = @($components)
            script = if ($scriptName) { $scriptName } else { "" }
        })
    }
    $references = New-Object 'System.Collections.Generic.List[object]'
    Add-SceneStringReferences $scene "scene" 0 $references
    $componentArray = @($componentCounts.GetEnumerator() | Sort-Object Name | ForEach-Object {
        [ordered]@{ name = $_.Name; count = [int]$_.Value }
    })
    $unknownArray = @($unknownComponents.Keys | Sort-Object)
    return [ordered]@{
        source = [ordered]@{
            path = Get-RelativePath $sceneFull
            size = [int64](Get-Item -LiteralPath $sceneFull).Length
            sha256 = Get-Sha256 $sceneFull
        }
        game = [string](Get-PropertyValue $scene "game" "")
        entityCount = $entities.Count
        returnedEntityCount = $entityItems.Count
        entitiesTruncated = ($entities.Count -gt $entityItems.Count)
        maxHierarchyDepth = if ($parentById.Count -gt 0) { @($parentById.Keys | ForEach-Object { Get-ParentDepth ([int]$_) $parentById } | Measure-Object -Maximum).Maximum } else { 0 }
        componentCounts = @($componentArray)
        scriptNames = @($scriptNames.Keys | Sort-Object)
        assetReferenceCount = $references.Count
        assetReferencesTruncated = ($references.Count -ge $ReferenceLimit)
        assetReferences = @($references.ToArray())
        entities = @($entityItems.ToArray())
        schema = $schemaSummary
        validation = [ordered]@{
            status = if ($unknownArray.Count -eq 0) { "parseable" } else { "parseable_with_unknown_components" }
            mode = "structural_summary_only"
            schemaAvailable = [bool](Get-PropertyValue $schemaSummary "exists" $false)
            unknownComponentKeys = @($unknownArray)
            nextAction = "如需严格字段/资源校验，继续调用 validate_scene"
        }
    }
}

function Get-AssetSearch([string]$SearchQuery, [string]$RequestedType, [int]$Limit, [string]$InputProjectPath = '', [bool]$IncludeHash = $false) {
    if ($InputProjectPath) {
        $state = Get-ProjectManifestState $InputProjectPath
        $script:selectedProjectRoot = $state.projectRoot
        $script:selectedResourceRoot = $state.resourceRootFull
        $script:selectedProjectManifestPath = $state.manifestPath
        return Get-ProjectAssetIndex $state $SearchQuery $RequestedType $Limit $IncludeHash
    }
    $roots = @("models", "resources", "projects", "engine\shaders\glsl", "engine\shaders\spv")
    $needle = if ($SearchQuery) { $SearchQuery.ToLowerInvariant() } else { "" }
    if ($RequestedType -eq "scenes") { $roots = @("projects") }
    elseif ($RequestedType -eq "scripts") { $roots = @("projects", "src", "include") }
    elseif ($RequestedType -eq "shaders") { $roots = @("engine\shaders", "src\shaders") }
    elseif ($RequestedType -eq "models") { $roots = @("models", "resources") }
    elseif ($RequestedType -eq "textures") { $roots = @("models", "resources") }
    elseif ($RequestedType -eq "audio") { $roots = @("resources") }
    $all = @(
        Get-FilesUnderRoots $roots | ForEach-Object {
            $type = Get-AssetTypeForFile $_
            if ($RequestedType -ne "all" -and $type -ne $RequestedType) { return }
            $relative = Get-RelativePath $_.FullName
            if ($needle -and $relative.ToLowerInvariant().IndexOf($needle, [System.StringComparison]::Ordinal) -lt 0) { return }
            Get-FileSummary $_ $type $true
        }
    )
    $selected = @($all | Select-Object -First $Limit)
    return [ordered]@{
        query = $SearchQuery
        assetType = $RequestedType
        roots = @($roots)
        totalMatches = $all.Count
        returned = $selected.Count
        truncated = ($all.Count -gt $selected.Count)
        items = @($selected)
    }
}

function Get-ManifestEntry([string]$Path, [string]$Kind) {
    $full = Resolve-ProjectPath $Path $true
    return [ordered]@{
        path = Get-RelativePath $full
        kind = $Kind
        size = [int64](Get-Item -LiteralPath $full).Length
        sha256 = Get-Sha256 $full
    }
}

function Write-DiscoveryManifest([string]$RequestPath, [string]$ManifestPath) {
    $files = New-Object 'System.Collections.Generic.List[object]'
    [void]$files.Add((Get-ManifestEntry (Get-RelativePath $RequestPath) "discovery_request"))
    foreach ($artifact in $script:sourceArtifacts.ToArray()) {
        [void]$files.Add([ordered]@{
            path = [string]$artifact.path
            kind = [string]$artifact.kind
            size = [int64]$artifact.size
            sha256 = [string]$artifact.sha256
        })
    }
    $manifest = [ordered]@{
        schemaVersion = 1
        tool = "agent_discovery"
        apiVersion = 1
        generatedAt = (Get-Date).ToString("o")
        runDir = Get-RelativePath $script:runDir
        files = @($files.ToArray())
        note = "manifest 不包含 result.json 和自身，避免哈希循环；result.json 由运行目录独立保存。"
    }
    Write-JsonFile $ManifestPath $manifest
    return $manifest
}

try {
    if ($MaxResults -lt 1 -or $MaxResults -gt 2000) { throw "maxResults 必须在 1..2000" }
    if ($MaxEntities -lt 1 -or $MaxEntities -gt 5000) { throw "maxEntities 必须在 1..5000" }
    if ($MaxAssetReferences -lt 1 -or $MaxAssetReferences -gt 5000) { throw "maxAssetReferences 必须在 1..5000" }
    if ($Mode -eq "scene" -and [string]::IsNullOrWhiteSpace($ScenePath)) { throw "scene 模式需要 scenePath" }
    if ($Mode -eq "context" -and [string]::IsNullOrWhiteSpace($ProjectPath)) { throw "context 模式需要 projectPath" }

    if ($ProjectPath) {
        $selectedState = Get-ProjectManifestState $ProjectPath
        $script:selectedProjectRoot = $selectedState.projectRoot
        $script:selectedResourceRoot = $selectedState.resourceRootFull
        $script:selectedProjectManifestPath = $selectedState.manifestPath
    }

    $outputRootFull = Resolve-ProjectPath $OutputRoot $false
    if (-not $RunId) { $RunId = (Get-Date -Format "yyyyMMdd-HHmmssfff") + "-" + ([Guid]::NewGuid().ToString("N").Substring(0, 8)) }
    if ($RunId -notmatch '^[A-Za-z0-9_.-]{1,80}$') { throw "RunId 只能包含字母、数字、下划线、点和短横线" }
    $script:runDir = Join-Path $outputRootFull $RunId
    if (Test-Path -LiteralPath $script:runDir) { throw "discovery run 目录已存在，为避免覆盖请换 RunId: $RunId" }
    New-Item -ItemType Directory -Path $script:runDir -Force | Out-Null
    $script:resultPath = Join-Path $script:runDir "result.json"
    $requestPath = Join-Path $script:runDir "request.json"
    $manifestPath = Join-Path $script:runDir "manifest.json"
    $request = [ordered]@{
        schemaVersion = 1
        tool = "agent_discovery"
        apiVersion = 1
        mode = $Mode
        scenePath = $ScenePath
        query = $Query
        assetType = $AssetType
        maxResults = $MaxResults
        maxEntities = $MaxEntities
        maxAssetReferences = $MaxAssetReferences
        projectPath = $ProjectPath
        renderDocPath = $RenderDocPath
        nsightPath = $NsightPath
        vulkanInfoPath = $VulkanInfoPath
        outputRoot = Get-RelativePath $outputRootFull
        runId = $RunId
    }
    Write-JsonFile $requestPath $request

    $projectSummary = $null
    $projectContext = $null
    $sceneSummary = $null
    $assetSearch = $null
    $deviceCapabilities = $null
    $nextAction = "继续根据本次 context 选择受控生成、构建、校验或测试工具"
    switch ($Mode) {
        "project" {
            Add-SourceArtifact "projects.json" "project_metadata" | Out-Null
            Add-SourceArtifact "CMakePresets.json" "build_metadata" | Out-Null
            Add-SourceArtifact "tools/scene_schema.json" "scene_schema" | Out-Null
            $projectSummary = Get-ProjectSummary $MaxResults
            if ($ProjectPath) { $projectContext = Get-ProjectContext $ProjectPath $Query $AssetType $MaxResults $false }
            $nextAction = if ($ProjectPath) { "先读取 projectContext 的 assetIndex 和 defaultScene，再选择 validate_scene 或 run_agent_test" } else { "先读取 capabilities、inventory 和 build，再选择 inspect_scene 或 query_assets 缩小上下文" }
        }
        "context" {
            $projectContext = Get-ProjectContext $ProjectPath $Query $AssetType $MaxResults $false
            $nextAction = "根据 projectContext.defaultScene、assetIndex 和 queryContract 生成后续测试或受控修改计划"
        }
        "scene" {
            $sceneSummary = Get-SceneSummary $ScenePath $MaxEntities $MaxAssetReferences
            $nextAction = "根据 scene.validation、componentCounts 和 assetReferences 决定 validate_scene、apply_scene_commands 或 run_gameplay_test"
        }
        "assets" {
            $assetSearch = Get-AssetSearch $Query $AssetType $MaxResults $ProjectPath $false
            Add-SourceArtifact "tools/scene_schema.json" "scene_schema" | Out-Null
            $nextAction = if ($ProjectPath) { "从 project assetIndex 返回的项目相对路径和 semanticRole 中选择后续素材" } else { "从 query_assets 返回的路径中选择场景、模型、纹理或脚本，再交给后续受控工具" }
        }
        "device" {
            $deviceCapabilities = Get-DeviceCapabilities
            $nextAction = "根据 deviceCapabilities.vulkan、renderDoc、nsight 和 mobile 选择可执行的桌面证据链路；未测试权限不能视为性能通过"
        }
    }

    $result = [ordered]@{
        schemaVersion = 1
        tool = "agent_discovery"
        apiVersion = 1
        success = $true
        generatedAt = (Get-Date).ToString("o")
        mode = $Mode
        runId = $RunId
        runDir = Get-RelativePath $script:runDir
        requestPath = Get-RelativePath $requestPath
        project = $projectSummary
        projectContext = $projectContext
        scene = $sceneSummary
        assets = $assetSearch
        deviceCapabilities = $deviceCapabilities
        schema = Get-SceneSchemaSummary
        capabilities = Get-Capabilities $Mode
        artifacts = @($script:sourceArtifacts.ToArray()) + @([pscustomobject](Get-ManifestEntry (Get-RelativePath $requestPath) "discovery_request"))
        manifestPath = ""
        nextAction = $nextAction
    }
    Write-JsonFile $script:resultPath $result
    Write-DiscoveryManifest $requestPath $manifestPath | Out-Null
    $result.manifestPath = Get-RelativePath $manifestPath
    Write-JsonFile $script:resultPath $result
    Write-Output "DISCOVERY_RESULT_PATH=$script:resultPath"
    Write-Output "SUCCESS=True"
    exit 0
} catch {
    $fatalError = $_.Exception.Message
    if ($null -ne $script:runDir) {
        if ($null -eq $script:resultPath) { $script:resultPath = Join-Path $script:runDir "result.json" }
        $errorResult = [ordered]@{
            schemaVersion = 1
            tool = "agent_discovery"
            apiVersion = 1
            success = $false
            generatedAt = (Get-Date).ToString("o")
            mode = $Mode
            runId = $RunId
            runDir = Get-RelativePath $script:runDir
            error = $fatalError
            nextAction = "修复 discovery 输入或项目结构后重试"
        }
        Write-JsonFile $script:resultPath $errorResult
        Write-Output "DISCOVERY_RESULT_PATH=$script:resultPath"
    }
    Write-Output "ERROR=$fatalError"
    Write-Output "SUCCESS=False"
    exit 3
}
