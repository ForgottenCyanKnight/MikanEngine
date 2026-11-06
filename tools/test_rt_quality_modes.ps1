param(
    [string]$Gpu='4090',
    [int]$Runs=3,
    [switch]$Validation,
    [switch]$NativeOnly,
    [string]$Output='out/bench-rt-test/quality-mode-crash/regression'
)
$ErrorActionPreference='Stop'
if($Runs -lt 1){throw 'Runs must be positive'}
if(Get-Process MikanEngine -ErrorAction SilentlyContinue){throw 'Save and close existing engine windows before testing'}
$root=Split-Path $PSScriptRoot -Parent
$settings=@{
    MIKAN_RT_QUALITY_MODE_SMOKE='1';MIKAN_GPU_PREFERENCE=$Gpu
    MIKAN_VK_VALIDATION=$(if($Validation){'1'}else{'0'})
    MIKAN_CPU_PROFILE='1';MIKAN_PROFILE_VIEW='scene';MIKAN_GPU_PROFILE='0'
    MIKAN_DLSS_FG=$(if($NativeOnly){'0'}else{''});MIKAN_VK_ASYNC_PRESENT='0'
    MIKAN_HWRT_NEE_TIER='balanced';MIKAN_HWRT_RESTIR_PT='0'
    MIKAN_HWRT_DLSS_RR='0';MIKAN_HWRT_DLSS_SR='0';MIKAN_HWRT_FSR2='1'
    MIKAN_HWRT_RESTIR_TEMPORAL='';MIKAN_HWRT_RESTIR_PT_SPATIAL=''
    MIKAN_VK_PRESENT_EXIT_FRAME='0';MIKAN_VK_PRESENT_SMOKE='0'
    MIKAN_PIPELINE_CAPTURE_FRAME='0';MIKAN_CPU_STAGE_TRACE=''
}
$saved=@{};Push-Location $root
try{
    $out=if([IO.Path]::IsPathRooted($Output)){$Output}else{Join-Path $root $Output}
    New-Item -ItemType Directory -Force $out|Out-Null
    foreach($key in $settings.Keys){
        $saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')
        [Environment]::SetEnvironmentVariable($key,$settings[$key],'Process')
    }
    $results=@()
    for($run=1;$run -le $Runs;++$run){
        $log=Join-Path $out "run-$run.log"
        & ./out/build/x64-release/MikanEngine.exe --project projects/menger-benchmark *> $log
        if($LASTEXITCODE -ne 0){throw "Run $run exited $LASTEXITCODE"}
        if(Select-String -Path $log -Pattern 'VK_ERROR_DEVICE_LOST|pure RT viewport unavailable|GPU preference .* matched nothing'){throw "Run $run rendering failure; inspect $log"}
        $steps=@(Select-String -Path $log -SimpleMatch '[RT quality mode smoke]')
        if($steps.Count -ne 14 -or !(Select-String -Path $log -SimpleMatch 'Exit successfully')){throw "Run $run incomplete regression sequence"}
        $results+=[pscustomobject]@{run=$run;steps=$steps.Count;clean_exit=$true;log=$log}
        $results|ConvertTo-Json -Depth 3|Set-Content (Join-Path $out 'results.json')
        Write-Output "Run $run passed: 14 editor/quality/game steps"
    }
}finally{
    foreach($key in $saved.Keys){[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}
    Pop-Location
}
