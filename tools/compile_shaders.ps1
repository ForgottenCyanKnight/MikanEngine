$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$glslangValidator = Join-Path $scriptDir "glslang\bin\glslangValidator.exe"
$root = Split-Path -Parent $scriptDir
$glslDir = Join-Path $root "engine\shaders\glsl"
$spvDir = Join-Path $root "engine\shaders\spv"
$recipe = Join-Path $root "cmake\CompileShaders.cmake"

if (-not (Test-Path -LiteralPath $glslangValidator -PathType Leaf)) {
    throw "glslangValidator not found: $glslangValidator"
}
if (-not (Test-Path -LiteralPath $glslDir -PathType Container)) {
    throw "GLSL directory not found: $glslDir"
}
if (-not (Test-Path -LiteralPath $spvDir -PathType Container)) {
    New-Item -ItemType Directory -Path $spvDir -Force | Out-Null
}

Write-Host "Compiling GLSL shaders to SPIR-V (transactional)..." -ForegroundColor Cyan
Write-Host ""

$token = "{0}-{1}" -f (Get-Date -Format "yyyyMMdd-HHmmssfff"), ([Guid]::NewGuid().ToString("N").Substring(0, 8))
$transactionDir = Join-Path $spvDir (".compile-shaders-$token")
$stagingDir = Join-Path $transactionDir "new"
$rollbackDir = Join-Path $transactionDir ".old"
$entries = @()
$commitStarted = $false
$rollbackFailed = $false

try {
    New-Item -ItemType Directory -Path $stagingDir -Force | Out-Null
    & cmake "-DGLSL_DIR=$glslDir" "-DSPV_DIR=$stagingDir" "-DGLSLANG_VALIDATOR=$glslangValidator" -P $recipe
    if ($LASTEXITCODE -ne 0) {
        throw "Shader compilation failed (exit=$LASTEXITCODE)."
    }

    $outputs = @(Get-ChildItem -LiteralPath $stagingDir -Recurse -File -Filter '*.spv')
    $outputs += Get-Item -LiteralPath (Join-Path $stagingDir "shader-build-plan.txt")
    foreach ($file in $outputs) {
        if ($file.Length -eq 0) { throw "Empty shader build output: $($file.FullName)" }
        $outputName = $file.FullName.Substring($stagingDir.Length + 1)
        $outputPath = Join-Path $spvDir $outputName
        $entries += [pscustomobject]@{
            output = $outputPath
            staged = $file.FullName
            hadOriginal = Test-Path -LiteralPath $outputPath -PathType Leaf
            backup = Join-Path $rollbackDir $outputName
        }
    }

    foreach ($entry in $entries) {
        New-Item -ItemType Directory -Path (Split-Path -Parent $entry.output) -Force | Out-Null
        if ($entry.hadOriginal) {
            New-Item -ItemType Directory -Path (Split-Path -Parent $entry.backup) -Force | Out-Null
            Copy-Item -LiteralPath $entry.output -Destination $entry.backup -Force
        }
    }

    $commitStarted = $true
    foreach ($entry in $entries) {
        Copy-Item -LiteralPath $entry.staged -Destination $entry.output -Force
    }

    Write-Host ""
    Write-Host "Committed $($outputs.Count - 1) shader(s). SPIR-V files are in: $spvDir" -ForegroundColor Cyan
    exit 0
}
catch {
    Write-Host ""
    Write-Host "Shader transaction aborted: $($_.Exception.Message)" -ForegroundColor Red
    if ($commitStarted) {
        foreach ($entry in $entries) {
            try {
                if ($entry.hadOriginal -and (Test-Path -LiteralPath $entry.backup -PathType Leaf)) {
                    Copy-Item -LiteralPath $entry.backup -Destination $entry.output -Force
                } elseif (-not $entry.hadOriginal -and (Test-Path -LiteralPath $entry.output -PathType Leaf)) {
                    Remove-Item -LiteralPath $entry.output -Force
                }
            } catch {
                $rollbackFailed = $true
                Write-Host "Rollback warning for $($entry.output): $($_.Exception.Message)" -ForegroundColor Red
            }
        }
        if ($rollbackFailed) {
            Write-Host "Recovery files kept in: $rollbackDir" -ForegroundColor Yellow
        } else {
            Write-Host "Previous SPIR-V outputs were restored." -ForegroundColor Yellow
        }
    } else {
        Write-Host "Existing SPIR-V outputs were left untouched." -ForegroundColor Yellow
    }
    exit 1
}
finally {
    if (!$rollbackFailed -and (Test-Path -LiteralPath $transactionDir -PathType Container)) {
        $resolvedTransaction = (Resolve-Path -LiteralPath $transactionDir).ProviderPath
        if ($resolvedTransaction -eq [System.IO.Path]::GetFullPath($transactionDir) -and
                (Split-Path -Parent $resolvedTransaction) -eq (Resolve-Path -LiteralPath $spvDir).ProviderPath) {
            Remove-Item -LiteralPath $resolvedTransaction -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}
