param(
    [string]$Project = 'projects/menger-benchmark',
    [string]$Gpu = '4090',
    [int]$Warmup = 300,
    [int]$Samples = 600,
    [int]$Pairs = 2,
    [string]$Output = 'out/bench-rt-gpu-tables',
    [ValidateSet('Triangles','Brick')][string]$Traversal = 'Triangles',
    [switch]$HalfLighting,
    [switch]$ForceDdaStaging,
    [switch]$ComparePrimaryLod,
    [switch]$CompareVoxLod,
    [switch]$Capture
)
$ErrorActionPreference = 'Stop'
if ($Warmup -lt 300 -or $Samples -lt 1 -or $Pairs -lt 1) { throw 'Warmup >= 300, Samples > 0 and Pairs > 0 required.' }
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root 'out/build/x64-release/MikanEngine.exe'
if (!(Test-Path $exe)) { throw 'Build MikanEngine first.' }
if (Get-Process MikanEngine -ErrorAction SilentlyContinue | Where-Object { $_.HandleCount -gt 0 -or $_.WorkingSet64 -gt 1MB }) { throw 'Close other MikanEngine processes first.' }
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
        MIKAN_HWRT_STATIC_VOX_CACHE='1'; MIKAN_HWRT_DEVICE_LOCAL_TABLES='1'; MIKAN_CPU_STAGE_TRACE=''
        MIKAN_HWRT_DEVICE_LOCAL_DDA='1'; MIKAN_HWRT_DDA_FORCE_STAGING='0'; MIKAN_VOX_QUAD_DAG='0'
        MIKAN_VOX_LOD='0'; MIKAN_VOX_LOD_PRIMARY='0'
        MIKAN_VOX_LOD_ONLY='-1'; MIKAN_VOX_LOD_DIRECT_REMESH_SCALE='0'; MIKAN_VOX_LOD_ONLY_CANDIDATES='0'
        MIKAN_VOX_LOD_FORCE='-1'
    }
    if($Traversal -eq 'Brick'){$settings.MIKAN_VOX_HW_BRICK_DDA='1'}
    if($HalfLighting){$settings.MIKAN_HWRT_HALF_GI='2';$settings.MIKAN_HWRT_HALF_GI_DENOISE='1';$settings.MIKAN_HWRT_HALF_SPECULAR='1'}
    if($ForceDdaStaging){$settings.MIKAN_HWRT_DDA_FORCE_STAGING='1'}
    $memorySwitch=if($Traversal -eq 'Brick'){'MIKAN_HWRT_DEVICE_LOCAL_DDA'}else{'MIKAN_HWRT_DEVICE_LOCAL_TABLES'}
    if($ComparePrimaryLod){
        if($Traversal -ne 'Triangles'){throw 'Primary mesh LOD requires Triangles traversal.'}
        $settings.MIKAN_VOX_LOD='1'
        $memorySwitch='MIKAN_VOX_LOD_PRIMARY'
    }
    if($CompareVoxLod){
        if($ComparePrimaryLod -or $Traversal -ne 'Triangles'){throw 'CompareVoxLod requires Triangles and cannot combine with ComparePrimaryLod.'}
        $settings.MIKAN_VOX_LOD_PRIMARY='1'
        $memorySwitch='MIKAN_VOX_LOD'
    }
    foreach ($key in $settings.Keys) {
        $saved[$key] = [Environment]::GetEnvironmentVariable($key, 'Process')
        [Environment]::SetEnvironmentVariable($key, $settings[$key], 'Process')
    }
    $results = @()
    $frames = $Warmup + $Samples
    for ($pair = 1; $pair -le $Pairs; ++$pair) {
        $order = if ($pair % 2) { @('1','0') } else { @('0','1') }
        foreach ($enabled in $order) {
            $name = "pair-$pair-local-$enabled"
            [Environment]::SetEnvironmentVariable($memorySwitch,$enabled,'Process')
            $env:MIKAN_CPU_STAGE_TRACE = Join-Path $outputPath "$name.csv"
            $log = Join-Path $outputPath "$name.log"
            & $exe --project $Project --headless --frames $frames *> $log
            if ($LASTEXITCODE -ne 0) { throw "$name exited $LASTEXITCODE" }
            if($CompareVoxLod -and $enabled -eq '1'){
                $selectionLines=Select-String -Path $log -Pattern 'VOX LOD selection.*L1=([0-9]+) L2=([0-9]+) L3=([0-9]+)'
                $selected=$false
                foreach($line in $selectionLines){$groups=$line.Matches[0].Groups;if(([int]$groups[1].Value+[int]$groups[2].Value+[int]$groups[3].Value) -gt 0){$selected=$true;break}}
                if(!$selected){throw "$name selected no nonzero LOD; refusing ineffective comparison"}
            }
            $trace = Import-Csv $env:MIKAN_CPU_STAGE_TRACE | Where-Object { $_.stage -eq 'application.iteration' -and [int]$_.call -gt $Warmup }
            $cpu = $trace | Measure-Object duration_ms -Sum
            if ($cpu.Count -ne $Samples) { throw "${name}: incomplete CPU trace" }
            $gpuTimes = @{}
            foreach ($line in Get-Content $log) {
                if ($line -match "frames=$frames label=(\S+) samples=$Samples avg_ms=([0-9.]+)") {
                    $gpuTimes[$Matches[1]] = [double]::Parse($Matches[2], [Globalization.CultureInfo]::InvariantCulture)
                }
            }
            foreach ($label in @('frame_total','rt.view1.primary_hits','rt.view1.lighting_di_gi_specular','rt.view1.nrd_diffuse','rt.view1.nrd_specular','rt.view1.taa_or_upscale')) {
                if (!$gpuTimes.ContainsKey($label)) { throw "${name}: missing GPU scope $label" }
            }
            $result = [pscustomobject]@{run=$name; local=[int]$enabled; samples=$Samples; gpu_ms=$gpuTimes['frame_total']; primary_ms=$gpuTimes['rt.view1.primary_hits']; lighting_ms=$gpuTimes['rt.view1.lighting_di_gi_specular']; nrd_diffuse_ms=$gpuTimes['rt.view1.nrd_diffuse']; nrd_specular_ms=$gpuTimes['rt.view1.nrd_specular']; upscale_ms=$gpuTimes['rt.view1.taa_or_upscale']; cpu_total_ms=$cpu.Sum; app_fps=1000*$Samples/$cpu.Sum}
            $results += $result
            $result | Format-Table
        }
    }
    $aggregate = @($results | Group-Object local | ForEach-Object {
        $count = ($_.Group | Measure-Object samples -Sum).Sum
        $wall = ($_.Group | Measure-Object cpu_total_ms -Sum).Sum
        [pscustomobject]@{local=[int]$_.Name; samples=$count; gpu_ms=($_.Group | Measure-Object gpu_ms -Average).Average; app_fps=1000*$count/$wall}
    })
    $before = $aggregate | Where-Object local -eq 0
    $after = $aggregate | Where-Object local -eq 1
    $gain = 100*($before.gpu_ms/$after.gpu_ms-1)
    [pscustomobject]@{gpu=$Gpu; traversal=$Traversal; half_lighting=[bool]$HalfLighting; force_dda_staging=[bool]$ForceDdaStaging; memory_switch=$memorySwitch; runs=$results; aggregate=$aggregate; gpu_throughput_gain_percent=$gain; gpu_time_reduction_percent=100*(1-$after.gpu_ms/$before.gpu_ms); effective=($gain -gt 10)} | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $outputPath 'summary.json')
    $aggregate | Format-Table
    Write-Host ('GPU throughput gain: {0:F2}% (effective requires > 10%)' -f $gain)
    if ($Capture) {
        foreach ($enabled in @('0','1')) {
            [Environment]::SetEnvironmentVariable($memorySwitch,$enabled,'Process')
            $env:MIKAN_CPU_STAGE_TRACE=''
            & $exe --project $Project --headless --frames 430 --fixed-dt 0.016666667 --screenshot-frame 420 --screenshot-path (Join-Path $outputPath "local-$enabled.png") *> (Join-Path $outputPath "capture-local-$enabled.log")
            if ($LASTEXITCODE -ne 0) { throw "Capture local=$enabled failed" }
        }
    }
} finally {
    foreach ($key in $saved.Keys) { [Environment]::SetEnvironmentVariable($key, $saved[$key], 'Process') }
    Pop-Location
}
