# Editor .blend importer. Source files are retained; runtime scenes use the GLB.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$InputPath,
    [Parameter(Mandatory=$true)][string]$ResultPath,
    [string]$BlenderPath = ""
)
$ErrorActionPreference = 'Stop'
function RemoveImportStaging {
    if ($staging -and (Test-Path -LiteralPath $staging)) {
        $resolved = [IO.Path]::GetFullPath($staging)
        if ([IO.Path]::GetDirectoryName($resolved) -ne [IO.Path]::GetFullPath($cache) -or
            [IO.Path]::GetFileName($resolved) -ne '.staging') { throw 'Unsafe import staging path' }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
# Serialize imports: concurrent runs would race on import.log and generation
# state (observed as editor retry storms colliding with CLI imports).
$importMutex = New-Object System.Threading.Mutex($false, 'MikanBlendImport')
if (-not $importMutex.WaitOne(0)) { throw 'Another .blend import is already running; retry after it finishes' }
trap {
    if ($cache) { [IO.File]::AppendAllText((Join-Path $cache 'import.log'), "`r`n$($_.Exception.Message)`r`n", (New-Object Text.UTF8Encoding($true))) }
    RemoveImportStaging
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
$source = (Get-Item -LiteralPath $InputPath).FullName
if ([IO.Path]::GetExtension($source).ToLowerInvariant() -ne '.blend') { throw 'Expected a .blend file' }
$cache = Join-Path (Split-Path $source) (Join-Path '.meta' ([IO.Path]::GetFileName($source)))
New-Item -ItemType Directory -Force -Path $cache | Out-Null
$exporter = Join-Path $PSScriptRoot 'blender/export_model.py'
$indexPath = Join-Path $cache 'import.json'
$fixedOutput = Join-Path $cache 'model.glb'
function FileHash([string]$path) {
    if (Test-Path -LiteralPath $path -PathType Leaf) { return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }
    return 'missing'
}
$signature = (FileHash $source) + ':' + (FileHash $exporter) + ':' + (FileHash $PSCommandPath) + ':' + (FileHash ($source + '.import.json'))
if (Test-Path -LiteralPath $indexPath) {
    try {
        $index = Get-Content -LiteralPath $indexPath -Raw -Encoding UTF8 | ConvertFrom-Json
        $valid = $index.output -eq $fixedOutput -and $index.signature -eq $signature -and (FileHash $index.output) -eq $index.outputHash
        foreach ($dependency in $index.dependencies) {
            if ((FileHash $dependency.path) -ne $dependency.hash) { $valid = $false }
        }
        if ($valid) {
            [IO.File]::WriteAllText($ResultPath, $index.output, (New-Object Text.UTF8Encoding($false)))
            exit 0
        }
    } catch { Write-Warning "Rebuilding invalid Blender import cache: $_" }
}
$candidates = @($BlenderPath, $env:BLENDER_EXECUTABLE)
$command = Get-Command blender -ErrorAction SilentlyContinue
if ($command) { $candidates += $command.Source }
foreach ($root in @("$env:ProgramFiles\Blender Foundation", 'D:\Program Files\Blender Foundation')) {
    if (Test-Path -LiteralPath $root) {
        $candidates += @(Get-ChildItem -LiteralPath $root -Filter blender.exe -Recurse -File | Sort-Object FullName -Descending | ForEach-Object FullName)
    }
}
$blender = $candidates | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } | Select-Object -First 1
if (-not $blender) { throw 'Blender not found. Set BLENDER_EXECUTABLE to blender.exe and restart the editor.' }
# Keep one published model; failed exports must not overwrite the last good one.
$staging = Join-Path $cache '.staging'
RemoveImportStaging
New-Item -ItemType Directory -Path $staging | Out-Null
$output = Join-Path $staging 'model.glb'
$logPath = Join-Path $cache 'import.log'
# Hold the log handle in the per-run generation directory: import.log is read
# by tooling and concurrent writers on it caused sharing violations.
$blenderLog = Join-Path $staging 'blender.log'
$previousErrorPreference = $ErrorActionPreference
$ErrorActionPreference = 'Continue' # Blender's stderr must be captured, not abort the pipe early in Windows PowerShell.
& $blender --background --factory-startup --disable-autoexec $source --python-exit-code 1 --python $exporter -- $output 2>&1 | Out-File -LiteralPath $blenderLog -Encoding utf8
$blenderExitCode = $LASTEXITCODE
$ErrorActionPreference = $previousErrorPreference
if ($blenderExitCode -ne 0) {
    [IO.File]::AppendAllText($logPath, "`r`n--- blender exit $blenderExitCode ---`r`n" + [IO.File]::ReadAllText($blenderLog), (New-Object Text.UTF8Encoding($true)))
    throw "Blender export failed. See $logPath"
}
[IO.File]::AppendAllText($logPath, "`r`n--- blender exit 0 ---`r`n" + [IO.File]::ReadAllText($blenderLog), (New-Object Text.UTF8Encoding($true)))
$bytes = [IO.File]::ReadAllBytes($output)
if ($bytes.Length -lt 20 -or [Text.Encoding]::ASCII.GetString($bytes,0,4) -ne 'glTF') { throw 'Invalid Blender GLB output' }
$dependencyPaths = Get-Content -LiteralPath ([IO.Path]::ChangeExtension($output, '.dependencies.json')) -Raw -Encoding UTF8 | ConvertFrom-Json
$dependencies = @($dependencyPaths | ForEach-Object { @{path=$_; hash=(FileHash $_)} })
foreach ($file in Get-ChildItem -LiteralPath $staging -File) {
    Move-Item -LiteralPath $file.FullName -Destination (Join-Path $cache $file.Name) -Force
}
RemoveImportStaging
$output = $fixedOutput
$index = @{signature=$signature; output=$output; outputHash=(FileHash $output); dependencies=$dependencies}
$temporary = "$indexPath.tmp"
[IO.File]::WriteAllText($temporary, ($index | ConvertTo-Json -Depth 5), (New-Object Text.UTF8Encoding($false)))
Move-Item -LiteralPath $temporary -Destination $indexPath -Force
[IO.File]::WriteAllText($ResultPath, $output, (New-Object Text.UTF8Encoding($false)))
