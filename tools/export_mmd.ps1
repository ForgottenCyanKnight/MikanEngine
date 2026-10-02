[CmdletBinding()]
param(
    [string]$ProjectPath = (Join-Path (Split-Path $PSScriptRoot -Parent) 'projects/mmd-dance-prototype'),
    [string]$OutputPath = '',
    [ValidateSet(60,120)][int]$Fps = 60,
    [ValidateRange(0,600)][int]$WarmupFrames = 60,
    [ValidateRange(0,36000)][double]$DurationSeconds = 0,
    [ValidateRange(0,1000000)][int]$FrameLimit = 0,
    [ValidateSet('sync','async')][string]$Readback = 'async',
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$ProjectPath = (Resolve-Path -LiteralPath $ProjectPath).Path
if (!$OutputPath) { $OutputPath = Join-Path $root ('out/videos/mmd-' + (Get-Date -Format yyyyMMdd_HHmmss) + '-'+$Fps+'fps.mp4') }
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
if ([IO.Path]::GetExtension($OutputPath) -ne '.mp4') { throw 'Output must be an MP4 file' }
if (Test-Path -LiteralPath $OutputPath) { throw 'Output exists; choose a new filename' }
$runDir = $OutputPath + '.export'
New-Item -ItemType Directory -Path $runDir -Force | Out-Null
$manifest = Get-Content -LiteralPath (Join-Path $ProjectPath 'project.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$resourceRoot = [IO.Path]::GetFullPath((Join-Path $ProjectPath $manifest.resourceRoot))
$scenePath = Join-Path $resourceRoot $manifest.scene
$scene = Get-Content -LiteralPath $scenePath -Raw -Encoding UTF8 | ConvertFrom-Json
$models = @($scene.entities | Where-Object {$_.mesh -and $_.vmd -and $_.vmd.enabled -and $_.vmd.motionPath})
if (!$models.Count) { throw 'No enabled animated MMD model in scene' }
$player = $models[0].vmd
$motion = Join-Path $resourceRoot $player.motionPath
# VMD 0002: header 30 + model name 20; each bone record is 111 bytes.
$reader = [IO.BinaryReader]::new([IO.File]::OpenRead($motion))
try {
    $header = [Text.Encoding]::ASCII.GetString($reader.ReadBytes(30)).Trim([char]0)
    if ($header -notlike 'Vocaloid Motion Data 0002*') { throw 'Expected VMD 0002 motion' }
    [void]$reader.ReadBytes(20)
    $count = $reader.ReadUInt32()
    if ($count -gt 10000000 -or (54L + 111L * $count) -gt $reader.BaseStream.Length) { throw 'Invalid VMD bone records' }
    [uint32]$lastFrame = 0
    for ($i=0; $i -lt $count; $i++) {
        [void]$reader.ReadBytes(15)
        $lastFrame = [Math]::Max($lastFrame, $reader.ReadUInt32())
        [void]$reader.BaseStream.Seek(92, [IO.SeekOrigin]::Current)
    }
} finally { $reader.Dispose() }
if (!$lastFrame) { throw 'Motion has no animation duration' }
$duration = $lastFrame / 30.0
if ($DurationSeconds -gt 0) { $duration = [Math]::Min($duration, $DurationSeconds) }
# Include t=0 and the authored endpoint when it lies on the output grid.
$frames = [int][Math]::Floor($duration * $Fps + 1e-7) + 1
if ($FrameLimit -gt 0) { $frames = [Math]::Min($frames, $FrameLimit); $duration = ($frames - 1) / [double]$Fps }
foreach ($entity in $scene.entities) {
    if ($entity.vmd) {
        $entity.vmd.loop = $false
        $entity.vmd.playing = $true
        $entity.vmd.speed = 1
        $entity.vmd.startFrame = 0
        $entity.vmd | Add-Member -NotePropertyName currentFrame -NotePropertyValue 0 -Force
    }
    if ($entity.camera) { $entity.camera.showFrustumWireframe = $false }
}
$candidate = Join-Path $runDir 'export-scene.json'
$scene | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $candidate -Encoding UTF8
if (!$SkipBuild) {
    & (Join-Path $PSScriptRoot 'build.ps1') -Target MikanEngine -KillEngine
    if ($LASTEXITCODE -ne 0) { throw 'Engine build failed' }
}
& (Join-Path $PSScriptRoot 'setup_ffmpeg.ps1') | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'FFmpeg installation failed' }
$encoder = Join-Path $root 'out/tools/ffmpeg/bin/ffmpeg.exe'
$probe = Join-Path $root 'out/tools/ffmpeg/bin/ffprobe.exe'
$engine = Join-Path $root 'out/build/x64-Release/MikanEngine.exe'
$arguments = @('--headless','--no-voxel-world','--project',$ProjectPath,'--scene',$candidate,
    '--frames',($frames+$WarmupFrames),'--video-output',$OutputPath,'--video-encoder',$encoder,
    '--video-fps',$Fps,'--video-warmup',$WarmupFrames,'--video-readback',$Readback,'--dump-state',(Join-Path $runDir 'final-state.json'),
    '--crash-log',(Join-Path $runDir 'crash.log'))
Write-Host "Exporting 1920x1080 $Fps FPS, $frames frames ($duration seconds) -> $OutputPath"
Push-Location (Split-Path $engine)
$exportClock = [Diagnostics.Stopwatch]::StartNew()
try { & $engine @arguments *> (Join-Path $runDir 'engine.log'); $engineExit = $LASTEXITCODE } finally { $exportClock.Stop(); Pop-Location }
if ($engineExit -ne 0) { throw "Export failed ($engineExit); see $runDir/engine.log" }
$probeJson = & $probe -v error -count_frames -select_streams v:0 -show_entries stream=codec_name,width,height,r_frame_rate,nb_read_frames,duration -of json $OutputPath
if ($LASTEXITCODE -ne 0) { throw 'Video verification failed' }
$stream = ($probeJson | ConvertFrom-Json).streams[0]
if ($stream.width -ne 1920 -or $stream.height -ne 1080 -or $stream.r_frame_rate -ne "$Fps/1" -or [int]$stream.nb_read_frames -ne $frames) {
    throw 'Encoded resolution, frame rate or frame count mismatch'
}
@{success=$true;output=$OutputPath;fps=$Fps;frames=$frames;animationSeconds=$duration;video=$stream;
    readback=$Readback;exportSeconds=$exportClock.Elapsed.TotalSeconds;exportFramesPerSecond=$frames/$exportClock.Elapsed.TotalSeconds} |
    ConvertTo-Json -Depth 8 | Set-Content (Join-Path $runDir 'result.json') -Encoding UTF8
Write-Host "Complete: $OutputPath ($frames verified frames)"
