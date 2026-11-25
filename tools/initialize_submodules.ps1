# Initialize pinned engine dependencies.
[CmdletBinding(DefaultParameterSetName='All')]
param(
    [Parameter(ParameterSetName='DLSS')][switch]$DLSSOnly,
    [Parameter(ParameterSetName='SDL3')][switch]$SDL3Only,
    [Parameter(ParameterSetName='SDLImage')][switch]$SDLImageOnly
)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot

function Initialize-SparseSubmodule([string]$name, [string]$modulePath, [string[]]$patterns, [switch]$preserveBytes) {
    $checkout = Join-Path $root $modulePath
    git -C $root submodule init -- $modulePath
    if ($LASTEXITCODE -ne 0) { throw "Submodule registration failed: $modulePath" }

    if (-not (Test-Path -LiteralPath (Join-Path $checkout '.git'))) {
        if ((Test-Path -LiteralPath $checkout) -and @(Get-ChildItem -LiteralPath $checkout -Force).Count) {
            throw "The directory contains local files: $checkout"
        }
        $cachedGitDir = git -C $root rev-parse --path-format=absolute --git-path "modules/$name"
        if ($LASTEXITCODE -ne 0) { throw "Cannot locate the Git directory: $modulePath" }
        if (Test-Path -LiteralPath $cachedGitDir -PathType Container) {
            if ($preserveBytes) {
                git --git-dir $cachedGitDir config core.autocrlf false
                if ($LASTEXITCODE -ne 0) { throw "Cannot configure checkout line endings: $modulePath" }
            }
            git -C $root submodule update --init -- $modulePath
            if ($LASTEXITCODE -ne 0) { throw "Cannot reconnect the submodule: $modulePath" }
        } else {
            git init --quiet $checkout
            if ($LASTEXITCODE -ne 0) { throw "Cannot initialize the Git directory: $modulePath" }
        }
    }

    # Configure the sparse paths before the initial checkout so Git fetches the retained blobs.
    $patterns | git -C $checkout sparse-checkout set --no-cone --stdin
    if ($LASTEXITCODE -ne 0) { throw "Sparse checkout setup failed: $modulePath" }

    git -C $checkout rev-parse --verify --quiet HEAD | Out-Null
    if ($LASTEXITCODE -ne 0) {
        if ($preserveBytes) {
            # Release license verification uses the exact upstream bytes.
            git -C $checkout config core.autocrlf false
            if ($LASTEXITCODE -ne 0) { throw "Cannot configure checkout line endings: $modulePath" }
        }
        $url = git -C $root config --get "submodule.$name.url"
        if ($LASTEXITCODE -ne 0 -or -not $url) { throw "Missing submodule URL: $modulePath" }
        git -C $checkout config remote.origin.url $url
        if ($LASTEXITCODE -ne 0) { throw "Cannot configure the remote: $modulePath" }
        git -C $checkout config remote.origin.fetch +refs/heads/*:refs/remotes/origin/*
        if ($LASTEXITCODE -ne 0) { throw "Cannot configure the fetch refspec: $modulePath" }

        $commit = git -C $root rev-parse --verify ":$modulePath"
        if ($LASTEXITCODE -ne 0) { throw "Cannot resolve the pinned commit: $modulePath" }
        git -C $checkout fetch --depth=1 --filter=blob:none origin $commit
        if ($LASTEXITCODE -ne 0) { throw "Cannot fetch the pinned commit: $modulePath" }
        git -C $checkout checkout --detach $commit
        if ($LASTEXITCODE -ne 0) { throw "Pinned checkout failed: $modulePath" }
    }

    git -C $root submodule absorbgitdirs -- $modulePath
    if ($LASTEXITCODE -ne 0) { throw "Git directory registration failed: $modulePath" }
    git -C $root submodule update --init -- $modulePath
    if ($LASTEXITCODE -ne 0) { throw "Submodule update failed: $modulePath" }
}

if (-not ($DLSSOnly -or $SDLImageOnly)) {
    Initialize-SparseSubmodule 'SDL' 'dependencies/SDL3' @(
        '/*'
        '!/.github/'
        '!/VisualC/'
        '!/VisualC-GDK/'
        '!/Xcode/'
        '!/docs/'
        '!/examples/'
        '!/test/*'
        '/test/COPYING'
        '/test/LICENSE.txt'
        '/test/*-license.txt'
        '!/src/hidapi/**/*.vcxproj'
        '!/src/hidapi/**/*.sln'
    )
}
if (-not ($DLSSOnly -or $SDL3Only)) {
    Initialize-SparseSubmodule 'SDL_image' 'dependencies/SDL3_image' @(
        '/*'
        '!/.github/'
        '!/VisualC/'
        '/VisualC/external/optional/*/LICENSE.*.txt'
        '!/Xcode/'
        '/Xcode/**/LICENSE.txt'
        '/Xcode/**/SDL_copying.h'
        '!/docs/'
        '!/examples/'
        '!/test/'
    )
}
if ($SDL3Only -or $SDLImageOnly) { return }

if (-not $DLSSOnly) {
    $sparsePaths = @('dependencies/SDL3', 'dependencies/SDL3_image')
    $sourcePatterns = [ordered]@{
        'FidelityFX-FSR2' = 'fsr2.txt'
        'meshoptimizer' = 'meshoptimizer.txt'
        'zlib' = 'zlib.txt'
    }
    foreach ($name in $sourcePatterns.Keys) {
        $modulePath = "dependencies/$name"
        $patternFile = Join-Path (Join-Path $PSScriptRoot 'submodule-paths') $sourcePatterns[$name]
        $patterns = @(Get-Content -LiteralPath $patternFile)
        Initialize-SparseSubmodule $name $modulePath $patterns -preserveBytes
        $sparsePaths += $modulePath
    }

    $paths = @(git -C $root config -f .gitmodules --get-regexp '^submodule\..*\.path$' |
        ForEach-Object { ($_ -split '\s+', 2)[1] } |
        Where-Object { $_ -notin $sparsePaths })
    if ($LASTEXITCODE -ne 0) { throw 'Cannot read submodule paths' }
    if ($paths.Count) {
        git -C $root submodule update --init -- $paths
        if ($LASTEXITCODE -ne 0) { throw 'Engine submodule initialization failed' }
    }
}

$manifest = Get-Content -LiteralPath (Join-Path $root 'dependencies/DLSS-sdk-manifest.json') -Encoding UTF8 -Raw | ConvertFrom-Json
if ($manifest.commit -notmatch '^[0-9a-f]{40}$' -or -not @($manifest.files).Count) {
    throw 'Invalid DLSS SDK manifest'
}
$sdk = Join-Path $root 'dependencies/DLSS'
if (Test-Path -LiteralPath (Join-Path $sdk '.git')) {
    throw "Move the legacy DLSS Git checkout out of $sdk before installing the Windows SDK."
}

function Test-SdkFile([string]$file, $entry) {
    return (Test-Path -LiteralPath $file -PathType Leaf) -and
        ((Get-Item -LiteralPath $file).Length -eq $entry.size) -and
        ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -eq $entry.sha256)
}

$downloadRoot = "https://raw.githubusercontent.com/NVIDIA/DLSS/$($manifest.commit)"
$ProgressPreference = 'SilentlyContinue'
foreach ($entry in $manifest.files) {
    $relative = [string]$entry.path
    if ($relative -notmatch '^[a-zA-Z0-9_.-]+(/[a-zA-Z0-9_.-]+)*$' -or
        ($relative -split '/') -contains '..' -or ($relative -split '/') -contains '.') {
        throw "Invalid DLSS SDK path: $relative"
    }
    $file = Join-Path $sdk $relative
    if (Test-SdkFile $file $entry) { continue }

    New-Item -ItemType Directory -Path (Split-Path -Parent $file) -Force | Out-Null
    $staged = "$file.$([Guid]::NewGuid()).partial"
    try {
        Write-Host "[DLSS] fetching $relative"
        Invoke-WebRequest -Uri "$downloadRoot/$relative" -UseBasicParsing -TimeoutSec 180 -OutFile $staged
        if (-not (Test-SdkFile $staged $entry)) { throw "DLSS SDK download failed validation: $relative" }
        Move-Item -LiteralPath $staged -Destination $file -Force
    } finally {
        if (Test-Path -LiteralPath $staged -PathType Leaf) {
            Remove-Item -LiteralPath $staged -Force
        }
    }
}
& (Join-Path $PSScriptRoot 'validate_dlss_sdk.ps1')
