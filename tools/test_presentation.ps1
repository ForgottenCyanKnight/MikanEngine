param([string]$Gpu='4090',[string]$Output='out/bench-rt-test/presentation-opt/validation')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
if(Get-Process MikanEngine -ErrorAction SilentlyContinue){throw 'Close other engine processes before testing'}
$settings=@{
    MIKAN_GPU_PREFERENCE=$Gpu;MIKAN_VK_VALIDATION='1';MIKAN_GPU_PROFILE='0';MIKAN_CPU_PROFILE='0';MIKAN_CPU_STAGE_TRACE=''
    MIKAN_VOX_LOD='0';MIKAN_VOX_LOD_PRIMARY='0';MIKAN_VOX_LOD_ONLY='-1';MIKAN_VOX_MEMORY_STATS='0'
    MIKAN_HWRT_STATIC_VOX_CACHE='1';MIKAN_HWRT_DEVICE_LOCAL_TABLES='1';MIKAN_TRIANGLE_BLAS_CACHE='1'
    MIKAN_VOX_HW_BRICK_DDA='0';MIKAN_HWRT_VOXEL_DDA='0';MIKAN_VOX_HW_QUAD_AABB='0';MIKAN_HWRT_SOFTWARE_QUAD_BVH='0';MIKAN_VOX_QUAD_DAG='0'
    MIKAN_HWRT_HALF_GI='0';MIKAN_HWRT_HALF_GI_DENOISE='0';MIKAN_HWRT_HALF_SPECULAR='0';MIKAN_HWRT_RESTIR_PT='0'
    MIKAN_HWRT_DLSS_RR='0';MIKAN_DLSS_FG='0';MIKAN_VK_PRESENT_MODE='immediate';MIKAN_VK_IMAGE_COUNT='3'
    MIKAN_VK_ASYNC_PRESENT='0';MIKAN_VK_PRESENT_SMOKE='0'
    MIKAN_DLSS_FG_PRELOAD='0';MIKAN_VK_PRESENT_EXIT_FRAME='0'
}
$saved=@{};Push-Location $root
try{
    $out=if([IO.Path]::IsPathRooted($Output)){$Output}else{Join-Path $root $Output}
    New-Item -ItemType Directory -Force $out|Out-Null
    foreach($k in $settings.Keys){$saved[$k]=[Environment]::GetEnvironmentVariable($k,'Process');[Environment]::SetEnvironmentVariable($k,$settings[$k],'Process')}
    foreach($name in @('sync','async','smoke','default')){
        $env:MIKAN_DLSS_FG=if($name -eq 'default'){''}else{'0'}
        $env:MIKAN_VK_ASYNC_PRESENT=if($name -eq 'sync'){'0'}elseif($name -eq 'default'){''}else{'1'}
        $env:MIKAN_VK_PRESENT_SMOKE=if($name -eq 'smoke'){'1'}else{'0'}
        $env:MIKAN_VK_PRESENT_MODE=if($name -eq 'smoke'){''}else{'immediate'}
        $env:MIKAN_VK_IMAGE_COUNT=if($name -eq 'smoke'){''}else{'3'}
        $frames=if($name -eq 'smoke'){600}else{430};$capture=if($name -eq 'smoke'){540}else{420}
        $log=Join-Path $out "$name.log"
        & ./out/build/x64-release/MikanEngine.exe --project projects/menger-benchmark --headless --fixed-dt 0.016666667 --frames $frames --screenshot-frame $capture --screenshot-path (Join-Path $out "$name.png") *> $log
        if($LASTEXITCODE -ne 0){throw "$name exited $LASTEXITCODE"}
        if(!(Test-Path (Join-Path $out "$name.png"))){throw "$name screenshot missing"}
        if(Select-String -Path $log -Pattern 'VK_ERROR_DEVICE_LOST|UNASSIGNED-Threading|pSignalSemaphores-00067|pWaitSemaphores-03268|semaphore-01779'){throw "$name contains a synchronization failure"}
        if($name -in @('async','smoke') -and !(Select-String -Path $log -Pattern 'async prefetch=1')){throw "$name never exercised async presentation"}
        if($name -eq 'default' -and (Select-String -Path $log -Pattern 'active path: async native')){throw 'Default startup unexpectedly enabled experimental async presentation'}
        if($name -eq 'smoke' -and (Select-String -Path $log -Pattern '\[Presentation smoke\]').Count -ne 6){throw 'Incomplete settings/resize smoke test'}
        Write-Host "$name completed with screenshot"
    }
    $baseline=@(Select-String -Path (Join-Path $out 'sync.log') -Pattern 'VUID-[A-Za-z0-9_-]+' -AllMatches|ForEach-Object {$_.Matches.Value}|Sort-Object -Unique)
    $checks=@(foreach($name in @('async','smoke','default')){
        $vuids=@(Select-String -Path (Join-Path $out "$name.log") -Pattern 'VUID-[A-Za-z0-9_-]+' -AllMatches|ForEach-Object {$_.Matches.Value}|Sort-Object -Unique)
        [pscustomobject]@{run=$name;new_vuids=@($vuids|Where-Object {$_ -notin $baseline});existing_vuids=$baseline}
    })
    $checks|ConvertTo-Json -Depth 4|Set-Content (Join-Path $out 'validation.json')
    if(@($checks|Where-Object {$_.new_vuids.Count -gt 0}).Count){throw 'New validation VUIDs; inspect validation.json'}
}finally{
    foreach($k in $saved.Keys){[Environment]::SetEnvironmentVariable($k,$saved[$k],'Process')}
    Pop-Location
}
