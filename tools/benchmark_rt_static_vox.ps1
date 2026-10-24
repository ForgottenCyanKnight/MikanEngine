param(
    [string]$Project = 'projects/menger-benchmark',
    [string]$Gpu = '4090',
    [int]$Warmup = 300,
    [int]$Samples = 600,
    [int]$Pairs = 2,
    [string]$Output = 'out/bench-rt-static-vox',
    [switch]$ReverseOrder
)
$ErrorActionPreference = 'Stop'
if ($Warmup -lt 300 -or $Samples -lt 1 -or $Pairs -lt 1) {
    throw 'Warmup must be >= 300; Samples and Pairs must be positive.'
}
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root 'out/build/x64-release/MikanEngine.exe'
if (!(Test-Path $exe)) { throw "Build MikanEngine first: $exe" }
# Windows can retain a terminated debugger process with zero handles and a tiny
# working set. It cannot submit GPU work and should not block a benchmark.
if (Get-Process MikanEngine -ErrorAction SilentlyContinue | Where-Object { $_.HandleCount -gt 0 -or $_.WorkingSet64 -gt 1MB }) {
    throw 'Close other MikanEngine processes before benchmarking.'
}
Push-Location $root
$saved = @{}
try {
    $outputPath = if ([IO.Path]::IsPathRooted($Output)) { $Output } else { Join-Path $root $Output }
    New-Item -ItemType Directory -Force $outputPath | Out-Null
    $settings = @{
        MIKAN_GPU_PREFERENCE=$Gpu; MIKAN_GPU_PROFILE='1'; MIKAN_GPU_PROFILE_WARMUP="$Warmup"
        MIKAN_CPU_PROFILE='1'; MIKAN_VOX_HW_BRICK_DDA='0'; MIKAN_HWRT_VOXEL_DDA='0'
        MIKAN_HWRT_DDA_NAADF='0'; MIKAN_HWRT_DDA_BITSET='0'; MIKAN_HWRT_DDA_SCREEN_BOUNDS='0'
        MIKAN_VOX_HW_QUAD_AABB='0'; MIKAN_HWRT_SOFTWARE_QUAD_BVH='0'
        MIKAN_HWRT_HALF_GI='0'; MIKAN_HWRT_HALF_GI_DENOISE='0'; MIKAN_HWRT_HALF_SPECULAR='0'
        MIKAN_HWRT_RESTIR_PT='0'; MIKAN_HWRT_DLSS_RR='0'; MIKAN_DLSS_FG='0'
        MIKAN_CPU_STAGE_TRACE=''; MIKAN_HWRT_STATIC_VOX_CACHE='1'
    }
    foreach ($key in $settings.Keys) {
        $saved[$key] = [Environment]::GetEnvironmentVariable($key, 'Process')
        [Environment]::SetEnvironmentVariable($key, $settings[$key], 'Process')
    }
    $results = @()
    for ($pair = 1; $pair -le $Pairs; ++$pair) {
        # Reverse each pair to reduce run-order bias (ABBA for two pairs).
        $order = if (([bool]($pair % 2)) -xor ([bool]$ReverseOrder)) { @('1','0') } else { @('0','1') }
        foreach ($cache in $order) {
            $name = "pair-$pair-cache-$cache"
            $env:MIKAN_HWRT_STATIC_VOX_CACHE = $cache
            $env:MIKAN_CPU_STAGE_TRACE = Join-Path $outputPath "$name.csv"
            & $exe --project $Project --headless --frames ($Warmup + $Samples) *> (Join-Path $outputPath "$name.log")
            if ($LASTEXITCODE -ne 0) { throw "$name exited $LASTEXITCODE" }
            $trace = Import-Csv $env:MIKAN_CPU_STAGE_TRACE
            $iteration = $trace | Where-Object { $_.stage -eq 'application.iteration' -and [int]$_.call -gt $Warmup }
            $stats = $iteration | Measure-Object duration_ms -Sum -Average
            if ($stats.Count -ne $Samples) { throw "${name}: incomplete frame trace" }
            $prepare = $trace | Where-Object { $_.stage -eq 'rt.scene_prepare' -and [int]$_.call -gt $Warmup } | Measure-Object duration_ms -Average
            $result = [pscustomobject]@{run=$name; cache=[int]$cache; frames=$stats.Count; total_ms=$stats.Sum; mean_ms=$stats.Average; fps=1000/$stats.Average; scene_prepare_ms=$prepare.Average}
            $results += $result
            $result | Format-Table
        }
    }
    $aggregate = @($results | Group-Object cache | ForEach-Object {
        $frames = ($_.Group | Measure-Object frames -Sum).Sum
        $total = ($_.Group | Measure-Object total_ms -Sum).Sum
        [pscustomobject]@{cache=[int]$_.Name; frames=$frames; mean_ms=$total/$frames; fps=1000*$frames/$total}
    })
    $gain = 100 * (($aggregate | Where-Object cache -eq 1).fps / ($aggregate | Where-Object cache -eq 0).fps - 1)
    [pscustomobject]@{runs=$results; aggregate=$aggregate; fps_gain_percent=$gain; effective=($gain -gt 10)} | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $outputPath 'summary.json')
    $aggregate | Format-Table
    Write-Host ('Average FPS gain: {0:F2}% (effective requires > 10%)' -f $gain)
} finally {
    foreach ($key in $saved.Keys) { [Environment]::SetEnvironmentVariable($key, $saved[$key], 'Process') }
    Pop-Location
}
