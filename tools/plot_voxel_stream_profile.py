"""Align CPU traces, Vulkan timestamps, and replay phases by render frame.

Usage: python tools/plot_voxel_stream_profile.py CAPTURE_DIR --title "Capture configuration"
Requires matplotlib. Reads cpu.csv, gpu.csv, replay.csv and optionally raster.csv.
"""

import argparse
from bisect import bisect_right
from collections import Counter, defaultdict
import csv
import json
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np


CPU_STAGES = {
    "world.update": "World update",
    "world.stream_lock": "Streaming lock wait",
    "world.stream_window": "Update stream window",
    "world.stream_build_window": "Build stream window",
    "world.stream_unload": "Unload chunks",
    "world.stream_load": "Load chunks",
    "world.stream_resident_scan": "Scan resident chunk keys",
    "world.stream_load_missing": "Load missing chunks",
    "world.stream_activate_chunk": "Insert active chunks",
    "world.stream_load_neighbors": "Queue loaded chunk and neighbors",
    "world.stream_unload_cache": "Move unloaded chunks into cache",
    "world.stream_unload_neighbors": "Queue unloaded chunk neighbors",
    "world.generate_chunk": "Generate terrain blocks",
    "world.restore_cached_chunk": "Restore cached chunks",
    "world.lod_lock": "LOD lock wait",
    "world.lod_scan": "Scan chunk LOD",
    "world.mesh_queue": "Queue mesh jobs",
    "raster.voxel_sync": "Sync resident meshes",
    "world.raster_snapshot_lock": "Mesh snapshot lock wait",
    "raster.voxel_cull": "Collect and cull draws",
    "swapchain.acquire": "Acquire swapchain image",
    "swapchain.offscreen_fence": "Wait for offscreen fence",
    "swapchain.present": "Present swapchain image",
    "render.shader_hot_reload": "Shader hot-reload scan",
}
GPU_STAGES = {
    "raster.gbuffer_geometry": "G-buffer geometry",
    "raster.voxel": "Voxel raster draw",
    "raster.lighting": "Lighting and composite",
    "raster.post_process": "Post-processing",
    "game_hiz": "Hi-Z construction",
}
PHASE_COLORS = ["#eef4ff", "#f2effb", "#f0f8f4", "#fff0e4", "#f0f8f4", "#fff0e4"]
CPU_PLOT_STAGES = (
    "world.generate_chunk", "world.restore_cached_chunk", "world.stream_unload", "world.stream_lock",
    "world.stream_load", "world.stream_resident_scan", "world.lod_scan", "raster.voxel_sync",
    "render.shader_hot_reload",
)


def read_rows(path):
    with path.open(newline="", encoding="utf-8-sig") as source:
        return list(csv.DictReader(source))


def read_cpu_frames(path):
    rows = read_rows(path)
    iterations = sorted(
        (float(row["start_ms"]), float(row["duration_ms"]))
        for row in rows if row["stage"] == "application.iteration"
    )
    starts = [start for start, _ in iterations]
    serials = {}
    for row in rows:
        if row["stage"] == "frame.serial":
            iteration = bisect_right(starts, float(row["start_ms"])) - 1
            if iteration < 0:
                continue
            if iteration in serials:
                raise ValueError(f"More than one render frame in application iteration {iteration}")
            serials[iteration] = int(row["call"])

    frames = defaultdict(Counter)
    for iteration, serial in serials.items():
        frames[serial]["application.iteration"] = iterations[iteration][1]
    for row in rows:
        stage = row["stage"]
        if stage in ("application.iteration", "frame.serial"):
            continue
        start = float(row["start_ms"])
        iteration = bisect_right(starts, start) - 1
        if iteration not in serials:
            continue
        frame_start, frame_duration = iterations[iteration]
        if start >= frame_start + frame_duration:
            continue
        frame = frames[serials[iteration]]
        frame[stage] += float(row["duration_ms"])
        frame[f"{stage}.count"] += 1
    for frame in frames.values():
        if frame["world.stream_load_missing.count"]:
            # Remaining time includes the resident-key lookup loop and CSV writes.
            frame["world.stream_resident_scan"] = max(0.0, frame["world.stream_load"] - frame["world.stream_load_missing"])
    return frames


def read_gpu_frames(path):
    frames = defaultdict(Counter)
    for row in read_rows(path):
        frames[int(row["frame"])][row["scope"]] += float(row["gpu_ms"])
    return frames


def get_series(frames, source, key, missing=0.0):
    return [source.get(frame, {}).get(key, missing) for frame in frames]


def write_summary(path, groups, cpu, gpu, raster):
    summary = {}
    captured_cpu_stages = {stage for frame in cpu.values() for stage in frame}
    for phase, frames in groups.items():
        measures = {
            "cpu_frame_ms": get_series(frames, cpu, "application.iteration"),
            "gpu_frame_ms": get_series(frames, gpu, "frame_total", float("nan")),
            **{name: get_series(frames, cpu, name) for name in CPU_STAGES if name in captured_cpu_stages},
            **{name: get_series(frames, gpu, name, float("nan")) for name in GPU_STAGES},
            "generated_chunks": get_series(frames, cpu, "world.generate_chunk.count"),
            "restored_chunks": get_series(frames, cpu, "world.restore_cached_chunk.count"),
            "uploaded_bytes": get_series(frames, raster, "geometry_upload_bytes"),
        }
        summary[phase] = {
            "frames": len(frames),
            "cpu_frames_over_16_67_ms": sum(value > 1000 / 60 for value in measures["cpu_frame_ms"]),
            "slowest_frame": max(frames, key=lambda frame: cpu[frame]["application.iteration"]),
        }
        for name, values in measures.items():
            values = np.asarray(values)
            values = values[np.isfinite(values)]
            if len(values):
                summary[phase][name] = {
                    "median": round(float(np.median(values)), 3),
                    "p95": round(float(np.percentile(values, 95)), 3),
                    "max": round(float(np.max(values)), 3),
                    "total": round(float(np.sum(values)), 3),
                }
    path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")


def write_plot(path, title, groups, cpu, gpu, raster):
    frames = sorted(frame for phase in groups.values() for frame in phase)
    fig, axes = plt.subplots(4, 1, figsize=(14, 10), sharex=True, layout="constrained")
    fig.suptitle(title, fontsize=15, weight="bold")
    axes[0].plot(frames, get_series(frames, cpu, "application.iteration"), label="CPU frame wall time",
                 color="#bd402d", linewidth=1.4)
    axes[0].plot(frames, get_series(frames, gpu, "frame_total", float("nan")), label="Vulkan GPU frame",
                 color="#1763a6", linewidth=1.3)
    crossings = [frame for frame in frames if cpu[frame]["world.stream_window.count"]]
    axes[0].scatter(crossings, get_series(crossings, cpu, "application.iteration"), s=14,
                    color="#6f3015", zorder=3, label="Stream window updated")
    axes[0].axhline(1000 / 60, color="#666666", linewidth=.8, linestyle="--", label="60 FPS budget")
    axes[0].set_title("Frame duration", loc="left")

    for key in CPU_PLOT_STAGES:
        values = get_series(frames, cpu, key)
        if any(values):
            axes[1].plot(frames, values, label=CPU_STAGES[key], linewidth=1.05)
    axes[1].set_title("CPU time on the main thread", loc="left")

    for key, label in GPU_STAGES.items():
        values = get_series(frames, gpu, key, float("nan"))
        if np.isfinite(values).any():
            axes[2].plot(frames, values, label=label, linewidth=1.1)
    axes[2].set_title("Vulkan GPU timestamp scopes (G-buffer includes voxel draw)", loc="left")

    axes[3].plot(frames, get_series(frames, cpu, "world.generate_chunk.count"),
                 color="#bd402d", linewidth=1.1, label="Generated chunks")
    axes[3].plot(frames, get_series(frames, cpu, "world.restore_cached_chunk.count"),
                 color="#167562", linewidth=1.05, label="Restored chunks")
    if raster:
        upload_ax = axes[3].twinx()
        upload_ax.plot(frames, np.asarray(get_series(frames, raster, "geometry_upload_bytes")) / (1024**2),
                       color="#1763a6", linewidth=1, alpha=.7, label="Geometry upload")
        upload_ax.set_ylabel("MiB")
        upload_ax.legend(loc="upper right", frameon=False, fontsize=8)
    axes[3].set_title("Streaming work and geometry uploads", loc="left")
    axes[3].set_ylabel("Chunks")
    axes[3].set_xlabel("Render frame (fixed 1/60 s simulation step)")

    for ax in axes[:3]:
        ax.set_ylabel("ms")
    for ax in axes:
        ax.set_xlim(frames[0] - .5, frames[-1] + .5)
        ax.set_ylim(bottom=0)
        ax.grid(axis="y", linewidth=.5, alpha=.35)
        for index, (phase, phase_frames) in enumerate(groups.items()):
            first, last = phase_frames[0] - .5, phase_frames[-1] + .5
            ax.axvspan(first, last, color=PHASE_COLORS[index % len(PHASE_COLORS)], zorder=0)
            if ax is axes[0]:
                ax.text((first + last) / 2, .98, phase.replace("_", " "), ha="center", va="top",
                        transform=ax.get_xaxis_transform(), fontsize=8, color="#283147")
        handles, labels = ax.get_legend_handles_labels()
        if handles:
            ax.legend(handles, labels, loc="upper left", bbox_to_anchor=(0, .87),
                      ncol=3 if ax is axes[1] else 2, frameon=False, fontsize=8)
    fig.savefig(path, dpi=150)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--title", default="Voxel world streaming on PC")
    args = parser.parse_args()
    capture = args.capture
    cpu = read_cpu_frames(capture / "cpu.csv")
    gpu = read_gpu_frames(capture / "gpu.csv")
    raster = defaultdict(Counter)
    raster_path = capture / "raster.csv"
    if raster_path.exists():
        for row in read_rows(raster_path):
            # Upload bytes are consumed by the first view and reset for subsequent views.
            raster[int(row["frame"])]["geometry_upload_bytes"] += int(row["geometry_upload_bytes"])

    groups = defaultdict(list)
    for row in read_rows(capture / "replay.csv"):
        frame = int(row["frame"])
        phase = row.get("world_phase", row["phase"])
        if phase.lower() != "warmup" and frame in cpu:
            groups[phase].append(frame)
    if not groups or not gpu:
        parser.error("capture has no render frames shared by the CPU trace and replay, or no GPU timestamps")

    crossing_groups = {}
    for phase, frames in groups.items():
        crossings = [frame for frame in frames if cpu[frame]["world.stream_window.count"]]
        if crossings:
            crossing_groups[f"{phase} (stream updates only)"] = crossings
    write_summary(capture / "summary.json", {**groups, **crossing_groups}, cpu, gpu, raster)
    write_plot(capture / "timeline.png", args.title, groups, cpu, gpu, raster)


if __name__ == "__main__":
    main()
