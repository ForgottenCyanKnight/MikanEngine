$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$glslangValidator = Join-Path $scriptDir "glslang\bin\glslangValidator.exe"
$glslDir = Join-Path $scriptDir "..\engine\shaders\glsl"
$spvDir = Join-Path $scriptDir "..\engine\shaders\spv"

if (-not (Test-Path $spvDir)) {
    New-Item -ItemType Directory -Path $spvDir -Force | Out-Null
}

Write-Host "Compiling GLSL shaders to SPIR-V..." -ForegroundColor Cyan
Write-Host ""

$shaders = Get-ChildItem -Path $glslDir -Include "*.vert","*.frag","*.comp" -Recurse

foreach ($shader in $shaders) {
    $extension = $shader.Extension
    $name = $shader.BaseName
    $outputName = "$name$extension.spv"
    $outputPath = Join-Path $spvDir $outputName
    
    Write-Host "Compiling: $($shader.Name)" -ForegroundColor Yellow
    & $glslangValidator -V $shader.FullName -o $outputPath
    
    if ($LASTEXITCODE -eq 0) {
        Write-Host "  -> $outputName" -ForegroundColor Green
    } else {
        Write-Host "  -> Failed!" -ForegroundColor Red
    }
}

Write-Host ""
Write-Host "Done! SPIR-V files are in: $spvDir" -ForegroundColor Cyan
