$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$glslangValidator = Join-Path $scriptDir "glslang\bin\glslangValidator.exe"
$glslDir = Join-Path $scriptDir "..\engine\shaders\glsl"
$spvDir = Join-Path $scriptDir "..\engine\shaders\spv"

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

$shaders = @(Get-ChildItem -LiteralPath $glslDir -Include "*.vert", "*.frag", "*.comp" -Recurse -File)
if ($shaders.Count -eq 0) {
    throw "No GLSL shader sources found: $glslDir"
}

$token = "{0}-{1}" -f (Get-Date -Format "yyyyMMdd-HHmmssfff"), ([Guid]::NewGuid().ToString("N").Substring(0, 8))
$transactionDir = Join-Path $spvDir (".compile-shaders-$token")
$rollbackDir = Join-Path $transactionDir ".old"
$entries = @()
$commitStarted = $false

try {
    New-Item -ItemType Directory -Path $transactionDir -Force | Out-Null

    foreach ($shader in $shaders) {
        $extension = $shader.Extension
        $name = $shader.BaseName
        $outputName = "$name$extension.spv"
        $outputPath = Join-Path $spvDir $outputName
        $stagedPath = Join-Path $transactionDir $outputName
        $entries += [pscustomobject]@{
            source = $shader.FullName
            output = $outputPath
            staged = $stagedPath
            hadOriginal = Test-Path -LiteralPath $outputPath -PathType Leaf
            backup = Join-Path $rollbackDir $outputName
        }

        Write-Host "Compiling: $($shader.Name)" -ForegroundColor Yellow
        & $glslangValidator -V $shader.FullName -o $stagedPath
        $compileExit = $LASTEXITCODE
        $stagedValid = ($compileExit -eq 0 -and (Test-Path -LiteralPath $stagedPath -PathType Leaf) -and ((Get-Item -LiteralPath $stagedPath).Length -gt 0))
        if (-not $stagedValid) {
            throw "shader compile failed: $($shader.FullName) (exit=$compileExit)"
        }
        Write-Host "  -> $outputName" -ForegroundColor Green
    }

    New-Item -ItemType Directory -Path $rollbackDir -Force | Out-Null
    foreach ($entry in $entries) {
        if ($entry.hadOriginal) {
            Copy-Item -LiteralPath $entry.output -Destination $entry.backup -Force
        }
    }

    $commitStarted = $true
    foreach ($entry in $entries) {
        Copy-Item -LiteralPath $entry.staged -Destination $entry.output -Force
    }

    Write-Host ""
    Write-Host "Committed $($entries.Count) shader(s). SPIR-V files are in: $spvDir" -ForegroundColor Cyan
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
                Write-Host "Rollback warning for $($entry.output): $($_.Exception.Message)" -ForegroundColor Red
            }
        }
        Write-Host "Previous SPIR-V outputs were restored where possible." -ForegroundColor Yellow
    } else {
        Write-Host "Existing SPIR-V outputs were left untouched." -ForegroundColor Yellow
    }
    exit 1
}
finally {
    if (Test-Path -LiteralPath $transactionDir -PathType Container) {
        Remove-Item -LiteralPath $transactionDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}
