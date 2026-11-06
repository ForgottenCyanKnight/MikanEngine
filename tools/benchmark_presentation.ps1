param(
    [string]$Gpu='4090',
    [int]$Warmup=600,
    [int]$Samples=1200,
    [int]$Pairs=2,
    [string[]]$Modes=@('immediate','mailbox'),
    [int[]]$ImageCounts=@(3),
    [int[]]$Async=@(0),
    [switch]$Editor,
    [string]$Output='out/bench-rt-test/presentation-opt/modes'
)
$ErrorActionPreference='Stop'
if($Warmup -lt 300 -or $Samples -lt 1 -or $Pairs -lt 1){throw 'Invalid sample counts'}
if(Get-Process MikanEngine -ErrorAction SilentlyContinue){throw 'Close other engine processes before testing'}
$root=Split-Path $PSScriptRoot -Parent
$saved=@{}
$settings=@{
    MIKAN_GPU_PREFERENCE=$Gpu;MIKAN_GPU_PROFILE='1';MIKAN_GPU_PROFILE_WARMUP="$Warmup";MIKAN_CPU_PROFILE='1'
    MIKAN_VOX_LOD='0';MIKAN_VOX_LOD_PRIMARY='0';MIKAN_VOX_LOD_ONLY='-1';MIKAN_VOX_LOD_FORCE='-1'
    MIKAN_VOX_LOD_DIRECT_REMESH_SCALE='0';MIKAN_VOX_LOD_ONLY_CANDIDATES='0';MIKAN_VOX_MEMORY_STATS='0'
    MIKAN_HWRT_STATIC_VOX_CACHE='1';MIKAN_HWRT_DEVICE_LOCAL_TABLES='1';MIKAN_TRIANGLE_BLAS_CACHE='1'
    MIKAN_VOX_HW_BRICK_DDA='0';MIKAN_HWRT_VOXEL_DDA='0';MIKAN_VOX_HW_QUAD_AABB='0';MIKAN_HWRT_SOFTWARE_QUAD_BVH='0'
    MIKAN_HWRT_DDA_NAADF='0';MIKAN_HWRT_DDA_BITSET='0';MIKAN_HWRT_DDA_SCREEN_BOUNDS='0';MIKAN_VOX_QUAD_DAG='0'
    MIKAN_HWRT_HALF_GI='0';MIKAN_HWRT_HALF_GI_DENOISE='0';MIKAN_HWRT_HALF_SPECULAR='0'
    MIKAN_HWRT_RESTIR_PT='0';MIKAN_HWRT_DLSS_RR='0';MIKAN_DLSS_FG='0';MIKAN_VK_VALIDATION='0'
    MIKAN_CPU_STAGE_TRACE='';MIKAN_VK_PRESENT_MODE='immediate';MIKAN_VK_IMAGE_COUNT='3'
    MIKAN_VK_ASYNC_PRESENT='0'
    MIKAN_VK_PRESENT_SMOKE='0'
    MIKAN_DLSS_FG_PRELOAD='0';MIKAN_VK_PRESENT_EXIT_FRAME='0'
}
Push-Location $root
try{
    $out=if([IO.Path]::IsPathRooted($Output)){$Output}else{Join-Path $root $Output}
    New-Item -ItemType Directory -Force $out|Out-Null
    foreach($k in $settings.Keys){$saved[$k]=[Environment]::GetEnvironmentVariable($k,'Process');[Environment]::SetEnvironmentVariable($k,$settings[$k],'Process')}
    $cases=@(foreach($mode in $Modes){foreach($count in $ImageCounts){foreach($asyncMode in $Async){[pscustomobject]@{mode=$mode;images=$count;async=$asyncMode}}}})
    $results=@();$frames=$Warmup+$Samples
    for($pair=1;$pair -le $Pairs;++$pair){
        $order=@($cases);if(!($pair%2)){[array]::Reverse($order)}
        foreach($case in $order){
            $name="pair-$pair-$($case.mode)-$($case.images)-async-$($case.async)";$log=Join-Path $out "$name.log"
            $env:MIKAN_VK_ASYNC_PRESENT="$($case.async)"
            $env:MIKAN_VK_PRESENT_MODE=$case.mode;$env:MIKAN_VK_IMAGE_COUNT="$($case.images)";$env:MIKAN_CPU_STAGE_TRACE=Join-Path $out "$name.csv"
            if($Editor){
                $env:MIKAN_VK_PRESENT_EXIT_FRAME="$($frames+60)"
                & ./out/build/x64-release/MikanEngine.exe --project projects/menger-benchmark *> $log
            }else{
                & ./out/build/x64-release/MikanEngine.exe --project projects/menger-benchmark --headless --frames $frames *> $log
            }
            if($LASTEXITCODE -ne 0){throw "$name exited $LASTEXITCODE"}
            $lines=Get-Content $log
            if($lines -match 'GPU preference .* matched nothing'){throw "Requested GPU unavailable $name; do not compare fallback hardware"}
            if($lines -match 'VK_ERROR_DEVICE_LOST|VOX LOD selection'){throw "Invalid run $name"}
            if($case.async -eq 1 -and !($lines -match 'async worker ready')){throw "Async presentation unavailable $name"}
            if($case.async -eq 1 -and !($lines -match 'async prefetch=1')){throw "Async swapchain prefetch unavailable $name"}
            if($Editor -and !($lines -match 'Editor.dll loaded - editor mode')){throw "Editor not loaded $name"}
            if($Editor -and $case.async -eq 1 -and !($lines -match 'active path: async native editor')){throw "Editor async path unavailable $name"}
            $chosen=Select-String -Path $log -Pattern '\[Presentation\] mode=(\S+) requestedImages=\d+ actualImages=(\d+)'|Select-Object -Last 1
            if(!$chosen -or $chosen.Matches[0].Groups[1].Value -ne $case.mode){throw "Requested present mode unavailable: $name"}
            $actualImages=[int]$chosen.Matches[0].Groups[2].Value
            $trace=Import-Csv $env:MIKAN_CPU_STAGE_TRACE
            $iteration=@($trace|Where-Object {$_.stage -eq 'application.iteration' -and [int]$_.call -gt $Warmup})
            if($Editor){$iteration=@($iteration|Select-Object -Last $Samples)}
            if($iteration.Count -ne $Samples){throw "Incomplete CPU samples $name"}
            $wall=($iteration|Measure-Object duration_ms -Sum).Sum
            $lo=[double]$iteration[0].start_ms;$last=$iteration[-1];$hi=[double]$last.start_ms+[double]$last.duration_ms
            $scopes=@{};foreach($group in ($trace|Where-Object {[double]$_.start_ms -ge $lo -and [double]$_.start_ms -lt $hi}|Group-Object stage)){$scopes[$group.Name]=($group.Group|Measure-Object duration_ms -Sum).Sum/$Samples}
            $times=@{};foreach($line in $lines){if($line -match "frames=$frames label=(\S+) samples=$Samples avg_ms=([0-9.]+)"){$times[$Matches[1]]=[double]::Parse($Matches[2],[Globalization.CultureInfo]::InvariantCulture)}}
            if($Editor){foreach($line in $lines){if($line -match 'frames=\d+ label=(\S+) samples=\d+ avg_ms=([0-9.]+)'){$times[$Matches[1]]=[double]::Parse($Matches[2],[Globalization.CultureInfo]::InvariantCulture)}}}
            if(!$times.ContainsKey('frame_total')){throw "Missing GPU samples $name"}
            $r=[pscustomobject]@{run=$name;mode=$case.mode;images=$case.images;async=$case.async;actual_images=$actualImages;samples=$Samples;app_total_ms=$wall;fps=1000*$Samples/$wall;gpu_ms=$times['frame_total'];present_ms=$scopes['swapchain.present'];vk_present_ms=$scopes['swapchain.vk_present'];acquire_ms=$scopes['swapchain.acquire'];frame_fence_ms=$scopes['swapchain.frame_fence'];offscreen_fence_ms=$scopes['swapchain.offscreen_fence'];cpu_scopes_ms=$scopes}
            $results+=$r;$r|Select-Object run,fps,gpu_ms,present_ms,vk_present_ms,acquire_ms,offscreen_fence_ms|Format-Table
            [pscustomobject]@{warmup=$Warmup;samples=$Samples;runs=$results}|ConvertTo-Json -Depth 7|Set-Content (Join-Path $out 'runs.json')
        }
    }
    $aggregate=@($results|Group-Object mode,images,async|ForEach-Object{
        $g=$_.Group;$n=($g|Measure-Object samples -Sum).Sum;$wall=($g|Measure-Object app_total_ms -Sum).Sum
        [pscustomobject]@{mode=$g[0].mode;images=$g[0].images;async=$g[0].async;samples=$n;fps=1000*$n/$wall;gpu_ms=($g|Measure-Object gpu_ms -Average).Average;present_ms=($g|Measure-Object present_ms -Average).Average;vk_present_ms=($g|Measure-Object vk_present_ms -Average).Average;acquire_ms=($g|Measure-Object acquire_ms -Average).Average;offscreen_fence_ms=($g|Measure-Object offscreen_fence_ms -Average).Average}
    })
    [pscustomobject]@{gpu=$Gpu;warmup=$Warmup;samples=$Samples;runs=$results;aggregate=$aggregate}|ConvertTo-Json -Depth 7|Set-Content (Join-Path $out 'summary.json')
    $aggregate|Format-Table
}finally{
    foreach($k in $saved.Keys){[Environment]::SetEnvironmentVariable($k,$saved[$k],'Process')}
    Pop-Location
}
