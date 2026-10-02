# MMD offline MP4 export (Windows)

Run from the engine repository in PowerShell:

```powershell
./tools/export_mmd.ps1 -OutputPath 'D:\Engine project\vulkan engine\out\videos\dance.mp4'
```

Defaults: local `projects/mmd-dance-prototype`, 1920x1080, 60 FPS,
H.264 CRF 18 / yuv420p, no audio. VMD timing remains 30 authored frames per
second; the output interpolates poses, it does not change playback speed.
The MMD runtime retains its 120 Hz physics step.

The exporter builds MikanEngine, downloads FFmpeg if absent, verifies the
publisher's archive SHA256, and renders through a hidden SDL/Vulkan window.
It needs a functioning Vulkan driver but no visible editor or real-time frame rate.
FFmpeg is a separate executable; no codec library is linked into Game.dll.
Download provider: https://www.gyan.dev/ffmpeg/builds/ (listed on
https://ffmpeg.org/download.html). Package and license files remain under
`out/tools/ffmpeg/package`; downloaded executables are local build tools.

```powershell
# Reuse an existing build, export a short diagnostic clip.
./tools/export_mmd.ps1 -SkipBuild -DurationSeconds 2 -OutputPath 'D:\clips\test.mp4'

# Alternative project or output frame rate.
./tools/export_mmd.ps1 -ProjectPath 'D:\my-project' -Fps 120 -OutputPath 'D:\clips\dance120.mp4'
```

The tool creates an export-only scene copy alongside the output, disables VMD
looping, resets tracks to frame zero, and preserves the source scene and assets.
The end time is the last bone key of the first enabled animated model's VMD.
Sixty temporal warmup frames hold the first pose at t=0; warmup is not recorded.
Frame zero is included, followed by exactly one fixed timestep per output frame.
An authored endpoint on the output grid is included, so the MP4 duration is one
output-frame interval longer than the time of its final sample.
Cloud wind and caustics use the export clock instead of wall time.

The default asynchronous path uses three persistently mapped staging buffers,
one completion fence per buffer, and an ordered encoding worker. GPU rendering
can overlap pixel consumption and encoding; buffers are reused only after their
pixels have been consumed. Slow rendering/encoding stalls the exporter instead
of dropping frames. The engine retains its existing synchronization for shared
offscreen targets; this change does not allow those targets to be overwritten
while a prior frame is using them.
No image sequence or uncompressed video file is stored. Existing output files
are refused. On capture or encoder failure the process returns failure; a partial
MP4 must not be treated as a finished export.

`<output>.export/engine.log` contains progress every 60 encoded frames.
`final-state.json` records the final runtime state. On successful completion,
ffprobe decodes/counts the video and verifies 1920x1080, output FPS, and exact
frame count before writing `result.json` with `success: true`.
No source changes or videos are pushed automatically.

For repeatable 1000-frame speed comparisons:

```powershell
./tools/export_mmd.ps1 -SkipBuild -FrameLimit 1000 -Readback sync -OutputPath 'D:\clips\sync.mp4'
./tools/export_mmd.ps1 -SkipBuild -FrameLimit 1000 -Readback async -OutputPath 'D:\clips\async.mp4'
```

Both modes keep identical rendering, physics, frame timestamps and x264 settings.
`exportSeconds` in result.json measures the engine process, including startup,
temporal warmup and encoder finalization, but excludes ffprobe validation and
building. `exportFramesPerSecond` is actual export throughput, independent of
the video's declared 60/120 FPS playback rate.
