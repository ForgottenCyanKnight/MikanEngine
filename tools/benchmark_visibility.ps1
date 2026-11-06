param([string]$Gpu='780M',[ValidateSet('native','raster')][string[]]$Flavors=@('native','raster'),[int]$Warmup=300,[int]$Samples=300,[string]$Tag='first')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
Set-Location $root
$out=Join-Path $root "out\rt-shader-variants\bench-$Gpu-$Tag"
New-Item $out -ItemType Directory -Force | Out-Null
$settings=@{
 MIKAN_VISIBILITY_ONLY='1';MIKAN_VOX_BRICK_DEDUP='0';MIKAN_GPU_PREFERENCE=$Gpu;MIKAN_GPU_PROFILE='1';MIKAN_GPU_PROFILE_WARMUP="$Warmup";MIKAN_CPU_PROFILE='1'
 MIKAN_DLSS_FG='0';MIKAN_HWRT_DLSS_RR='0';MIKAN_HWRT_DLSS_SR='0';MIKAN_HWRT_FSR2='1'
 MIKAN_VOX_LOD='0';MIKAN_VOX_LOD_ONLY='-1';MIKAN_VOX_LOD_FORCE='-1';MIKAN_VOX_LOD_PRIMARY='0';MIKAN_TRIANGLE_DEBUG='0'
 MIKAN_HWRT_VOXEL_DDA='0';MIKAN_VOX_HW_BRICK_DDA='0';MIKAN_HWRT_DDA_NAADF='0';MIKAN_HWRT_DDA_BITSET='0';MIKAN_HWRT_DDA_SCREEN_BOUNDS='0'
 MIKAN_HWRT_SOFTWARE_QUAD_BVH='0';MIKAN_VOX_HW_QUAD_AABB='0';MIKAN_VOX_QUAD_DAG='0';MIKAN_VOX_QUAD_STATS='0'
 MIKAN_HWRT_STATIC_VOX_CACHE='1';MIKAN_HWRT_DEVICE_LOCAL_TABLES='1';MIKAN_RT_STATS='0';MIKAN_TRIANGLE_BLAS_CACHE='1'
 MIKAN_HWRT_NEE_TIER='balanced';MIKAN_HWRT_NEE_DI_SPP='1';MIKAN_HWRT_FRESH_DIFFUSE_SPP='1'
 MIKAN_HWRT_HALF_GI='0';MIKAN_HWRT_HALF_GI_DENOISE='0';MIKAN_HWRT_HALF_SPECULAR='0';MIKAN_HWRT_NRD_JOINT='0'
 MIKAN_HWRT_RESTIR_PT='0';MIKAN_HWRT_IRRADIANCE_CACHE='0';MIKAN_HWRT_IRRADIANCE_STATS='0'
 MIKAN_HWRT_HYBRID='0';MIKAN_HWRT_HYBRID_SUN='0';MIKAN_HWRT_PRIMARY_SUN_ONLY='0';MIKAN_HWRT_RAW_OUTPUT='0'
}
foreach($k in $settings.Keys){[Environment]::SetEnvironmentVariable($k,$settings[$k],'Process')}
$settings | ConvertTo-Json | Set-Content (Join-Path $out 'settings.json')
$results=@();$frames=$Warmup+$Samples
foreach($flavor in $Flavors){
 $env:MIKAN_VISIBILITY=if($flavor -eq 'raster'){'raster'}else{'raytrace'}
 $env:MIKAN_HWRT_SHADER_VARIANTS='1'
 $env:MIKAN_VOX_HW_QUAD_AABB=if($flavor -eq 'aabb'){'1'}else{'0'}
 $env:MIKAN_VOX_HW_BRICK_DDA=if($flavor -in @('brick','baseline')){'1'}else{'0'}
 $env:MIKAN_VOX_BRICK_SIZE='4'
 $env:MIKAN_HWRT_VOXEL_DDA=if($flavor -eq 'naadf'){'1'}else{'0'}
 $env:MIKAN_HWRT_DDA_NAADF=if($flavor -eq 'naadf'){'1'}else{'0'}
 $env:MIKAN_HWRT_SHADER_OVERRIDE=if($flavor -eq 'baseline'){Join-Path $root 'out\brick-opt\baseline.spv'}elseif($flavor -eq 'brick'){Join-Path $root 'engine\shaders\spv\vox_rt_realtime.comp.brick.spv'}else{''}
 $env:MIKAN_CPU_STAGE_TRACE=Join-Path $out "$flavor.csv"
 $log=Join-Path $out "$flavor.log"
 Write-Output "Starting $Gpu $flavor"
 & .\out\build\x64-release\MikanEngine.exe --project projects/menger-benchmark --headless --frames $frames > $log 2> (Join-Path $out "$flavor-stderr.log")
 if($LASTEXITCODE -ne 0){throw "$Gpu $flavor exit=$LASTEXITCODE"}
 $lines=Get-Content $log
 if($lines -match 'VOX LOD BLAS|VOX LOD selection|vox BLAS build failed|pure RT viewport unavailable'){throw 'Invalid no-LOD benchmark'}
 if(!($lines -match "Selected physical device:.*$([regex]::Escape($Gpu))")){throw "Requested GPU $Gpu was not selected; refusing mislabeled results"}
 $times=@{};foreach($line in $lines){if($line -match "frames=$frames label=(\S+) samples=$Samples avg_ms=([0-9.]+)"){$times[$Matches[1]]=[double]::Parse($Matches[2],[Globalization.CultureInfo]::InvariantCulture)}}
 if(!$times.ContainsKey('frame_total')){throw 'No GPU profile summary'}
 $trace=@(Import-Csv $env:MIKAN_CPU_STAGE_TRACE | Where-Object {$_.stage -eq 'application.iteration' -and [int]$_.call -gt $Warmup})
 $cpu=$trace|Measure-Object duration_ms -Sum
 if($cpu.Count -ne $Samples){throw "Wrong CPU sample count $($cpu.Count)"}
 $scopes=@{};foreach($scope in @('rt.scene_prepare','frame.render','swapchain.present')){
  $t=@(Import-Csv $env:MIKAN_CPU_STAGE_TRACE | Where-Object {$_.stage -eq $scope -and [int]$_.call -gt $Warmup});$m=$t|Measure-Object duration_ms -Average;$scopes[$scope]=$m.Average
 }
 $r=[pscustomobject]@{gpu=$Gpu;flavor=$flavor;samples=$Samples;visibility_ms=if($flavor -eq 'raster'){$times['rt.view1.visibility_raster']}else{$times['rt.view1.visibility_raytrace']};raster_draw_ms=$times['visibility.raster_draw'];raster_copy_ms=$times['visibility.raster_copy'];raster_pack_ms=$times['visibility.raster_pack'];hzb_cull_ms=$times['visibility.hzb_cull'];hzb_build_ms=$times['visibility.hzb_build']}
 $results+=$r;$r|ConvertTo-Json -Depth 5
 $results|ConvertTo-Json -Depth 8 | Set-Content (Join-Path $out 'summary.json')
}
