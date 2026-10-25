param(
    [string]$Gpu='780M',
    [int]$Warmup=360,
    [int]$Samples=360,
    [int]$Pairs=2,
    [string]$Output='out/bench-rt-test/menger-isolated',
    [switch]$CaptureOnly
)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$exe=Join-Path $root 'out/build/x64-release/MikanEngine.exe'
if(Get-Process MikanEngine -ErrorAction SilentlyContinue){throw 'Close the engine before benchmarking.'}
if($Warmup -lt 300 -or $Samples -lt 1 -or $Pairs -lt 1){throw 'Invalid sample settings.'}
$settings=@{
    MIKAN_GPU_PREFERENCE=$Gpu; MIKAN_GPU_PROFILE='1'; MIKAN_GPU_PROFILE_WARMUP="$Warmup"
    MIKAN_CPU_PROFILE='1'; MIKAN_VOX_LOD='1'; MIKAN_VOX_LOD_PRIMARY='1'; MIKAN_VOX_LOD_FORCE='-1'
    MIKAN_VOX_LOD_ONLY='0'; MIKAN_VOX_LOD_MIN_TRIANGLES='12'; MIKAN_VOX_LOD_MAX_MERGE='1000000000'
    MIKAN_VOX_HW_BRICK_DDA='0'; MIKAN_HWRT_VOXEL_DDA='0'; MIKAN_VOX_HW_QUAD_AABB='0'
    MIKAN_HWRT_SOFTWARE_QUAD_BVH='0'; MIKAN_VOX_QUAD_DAG='0'; MIKAN_HWRT_STATIC_VOX_CACHE='1'
    MIKAN_HWRT_HALF_GI='2'; MIKAN_HWRT_HALF_GI_DENOISE='1'; MIKAN_HWRT_HALF_SPECULAR='1'
    MIKAN_HWRT_RESTIR_PT='0'; MIKAN_HWRT_DLSS_RR='0'; MIKAN_DLSS_FG='0'
    MIKAN_HWRT_DEVICE_LOCAL_TABLES='1'; MIKAN_VOX_MEMORY_STATS='1'; MIKAN_CPU_STAGE_TRACE=''
}
$saved=@{}
Push-Location $root
try{
    $out=if([IO.Path]::IsPathRooted($Output)){$Output}else{Join-Path $root $Output}
    New-Item -ItemType Directory -Force $out | Out-Null
    foreach($k in $settings.Keys){$saved[$k]=[Environment]::GetEnvironmentVariable($k,'Process');[Environment]::SetEnvironmentVariable($k,$settings[$k],'Process')}
    $results=@();$frames=$Warmup+$Samples
    if(!$CaptureOnly){for($pair=1;$pair -le $Pairs;++$pair){
        $order=if($pair%2){@(0,1,2,3)}else{@(3,2,1,0)}
        foreach($level in $order){
            $env:MIKAN_VOX_LOD_ONLY="$level"
            $name="pair-$pair-lod-$level";$log=Join-Path $out "$name.log"
            $env:MIKAN_CPU_STAGE_TRACE=Join-Path $out "$name.csv"
            & $exe --project projects/menger-benchmark --headless --fixed-dt 0.016666667 --frames $frames *> $log
            if($LASTEXITCODE -ne 0){throw "$name failed: $LASTEXITCODE"}
            $lines=Get-Content $log
            if($lines -match 'requested level .* unavailable|vox BLAS build failed|pure RT viewport unavailable'){throw "$name has invalid geometry"}
            if($level -gt 0){
                if(!($lines -match "VOX LOD only.*level=$level")){throw "$name did not load isolated LOD"}
                if($lines -match 'VOX BLAS memory'){throw "$name loaded original BLAS"}
            }elseif($lines -match 'VOX LOD BLAS|VOX LOD cache'){throw "$name loaded proxy geometry"}
            $times=@{};foreach($line in $lines){if($line -match "frames=$frames label=(\S+) samples=$Samples avg_ms=([0-9.]+)"){$times[$Matches[1]]=[double]::Parse($Matches[2],[Globalization.CultureInfo]::InvariantCulture)}}
            foreach($scope in @('frame_total','rt.view1.primary_hits','rt.view1.lighting_di_gi_specular')){if(!$times.ContainsKey($scope)){throw "Missing $scope in $name"}}
            $trace=Import-Csv $env:MIKAN_CPU_STAGE_TRACE | Where-Object {$_.stage -eq 'application.iteration' -and [int]$_.call -gt $Warmup}
            $cpu=$trace|Measure-Object duration_ms -Sum
            if($cpu.Count -ne $Samples){throw "Incomplete samples in $name"}
            $r=[pscustomobject]@{run=$name;level=$level;samples=$Samples;gpu_ms=$times['frame_total'];primary_ms=$times['rt.view1.primary_hits'];lighting_ms=$times['rt.view1.lighting_di_gi_specular'];app_total_ms=$cpu.Sum;app_fps=1000*$Samples/$cpu.Sum}
            $results+=$r;$r|Format-Table
        }
    }
    $aggregate=@($results|Group-Object level|ForEach-Object{
        $g=$_.Group;$n=($g|Measure-Object samples -Sum).Sum;$wall=($g|Measure-Object app_total_ms -Sum).Sum
        [pscustomobject]@{level=[int]$_.Name;gpu_ms=($g|Measure-Object gpu_ms -Average).Average;primary_ms=($g|Measure-Object primary_ms -Average).Average;lighting_ms=($g|Measure-Object lighting_ms -Average).Average;app_fps=1000*$n/$wall;samples=$n}
    }|Sort-Object level)
    [pscustomobject]@{gpu=$Gpu;half_lighting=$true;warmup=$Warmup;runs=$results;aggregate=$aggregate}|ConvertTo-Json -Depth 6|Set-Content (Join-Path $out 'summary.json')
    $aggregate|Format-Table
    }
    foreach($level in 0..3){
        $env:MIKAN_VOX_LOD_ONLY="$level";$env:MIKAN_CPU_STAGE_TRACE=''
        & $exe --project projects/menger-benchmark --headless --fixed-dt 0.016666667 --frames 430 --screenshot-frame 420 --screenshot-path (Join-Path $out "lod-$level.png") *> (Join-Path $out "capture-lod-$level.log")
        if($LASTEXITCODE -ne 0){throw "Screenshot LOD $level failed"}
    }
}finally{
    foreach($k in $saved.Keys){[Environment]::SetEnvironmentVariable($k,$saved[$k],'Process')}
    Pop-Location
}
