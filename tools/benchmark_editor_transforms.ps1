param([int]$Pairs=2,[string]$Output='out/bench-rt-test/editor-transforms/final')
$ErrorActionPreference='Stop'
if(Get-Process MikanEngine -ErrorAction SilentlyContinue){throw 'Save and close existing engine windows first'}
$root=Split-Path $PSScriptRoot -Parent
$settings=@{
    MIKAN_RT_TRANSFORM_SMOKE='1';MIKAN_RT_QUALITY_MODE_SMOKE='0';MIKAN_GPU_PREFERENCE='4090'
    MIKAN_CPU_PROFILE='1';MIKAN_PROFILE_VIEW='scene';MIKAN_GPU_PROFILE='0';MIKAN_DLSS_FG='0';MIKAN_VK_VALIDATION='0'
    MIKAN_HWRT_NEE_TIER='balanced';MIKAN_HWRT_RESTIR_PT='0';MIKAN_VK_PRESENT_EXIT_FRAME='0';MIKAN_VK_ASYNC_PRESENT='0'
    MIKAN_HWRT_TRANSFORM_INPUT_REUSE='1';MIKAN_CPU_STAGE_TRACE=''
}
$saved=@{};Push-Location $root
try{
    $out=if([IO.Path]::IsPathRooted($Output)){$Output}else{Join-Path $root $Output}
    New-Item -ItemType Directory -Force $out|Out-Null
    foreach($key in $settings.Keys){$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process');[Environment]::SetEnvironmentVariable($key,$settings[$key],'Process')}
    $results=@()
    for($pair=1;$pair -le $Pairs;++$pair){
        $order=if($pair%2){@(0,1)}else{@(1,0)}
        foreach($reuse in $order){
            $name="pair-$pair-reuse-$reuse";$log=Join-Path $out "$name.log"
            $env:MIKAN_HWRT_TRANSFORM_INPUT_REUSE="$reuse";$env:MIKAN_CPU_STAGE_TRACE=Join-Path $out "$name.csv"
            & ./out/build/x64-release/MikanEngine.exe --project projects/menger-benchmark --screenshot-frame 600 --screenshot-path (Join-Path $out "$name.png") *> $log
            if($LASTEXITCODE -ne 0){throw "$name exited $LASTEXITCODE"}
            if(Select-String -Path $log -Pattern 'VK_ERROR_DEVICE_LOST|pure RT viewport unavailable|GPU preference .* matched nothing'){throw "$name invalid render"}
            if(@(Select-String -Path $log -Pattern '\[RT transform smoke\] frame=').Count -ne 4){throw "$name incomplete transform exercise"}
            $trace=Import-Csv $env:MIKAN_CPU_STAGE_TRACE
            $frames=@($trace|Where-Object {$_.stage -eq 'application.iteration' -and [int]$_.call -gt 180 -and [int]$_.call -le 660})
            if($frames.Count -ne 480){throw "$name missing frame samples"}
            $lo=[double]$frames[0].start_ms;$hi=[double]$frames[-1].start_ms+[double]$frames[-1].duration_ms
            $window=@($trace|Where-Object {[double]$_.start_ms -ge $lo -and [double]$_.start_ms -lt $hi})
            $scopes=@{};foreach($group in ($window|Group-Object stage)){$scopes[$group.Name]=($group.Group|Measure-Object duration_ms -Sum).Sum/480}
            $frameMs=($frames|Measure-Object duration_ms -Average).Average
            $result=[pscustomobject]@{run=$name;reuse=$reuse;samples=480;frame_ms=$frameMs;fps=1000/$frameMs;rebuilds=@($window|Where-Object stage -eq 'rt.rebuild_shared_inputs').Count;light_updates=@($window|Where-Object stage -eq 'rt.update_light_inputs').Count;scopes_ms=$scopes}
            $results+=$result;$result|Select-Object run,fps,frame_ms,rebuilds,light_updates|Format-Table
            $results|ConvertTo-Json -Depth 5|Set-Content (Join-Path $out 'summary.json')
        }
    }
}finally{
    foreach($key in $saved.Keys){[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}
    Pop-Location
}
