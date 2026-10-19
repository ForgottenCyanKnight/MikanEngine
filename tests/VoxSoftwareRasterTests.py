"""Exercise real Vulkan draws/presentation under Xvfb with a CPU-only ICD.

Pillow only serializes the captured BMP pixels as PNG and assembles an overview;
all model rasterization and shader execution happen in Vulkan, not Python.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import select
import shutil
import subprocess


def write_overviews(output):
    from PIL import Image, ImageDraw
    for mode in ("albedo", "normals"):
        overview = Image.new("RGB", (1280, 1040), (14, 18, 26))
        draw = ImageDraw.Draw(overview)
        for index, name in enumerate(("chr_knight", "teapot", "dragon", "menger")):
            x, y = (index % 2) * 640, (index // 2) * 520
            label = "original VOX albedo" if mode == "albedo" else "geometry normal buffer"
            draw.text((x + 16, y + 12), f"{name} | {label} | CPU Vulkan", fill="white")
            with Image.open(output / f"{name}_view0_backfill_{mode}.png") as image:
                overview.paste(image.convert("RGB"), (x, y + 40))
        overview.save(output / ("overview.png" if mode == "albedo" else "normals-overview.png"))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("executable", type=Path)
    parser.add_argument("--output", type=Path, default=Path("out/software-vulkan"))
    parser.add_argument("--runtime-root", type=Path)
    parser.add_argument("--strict-comparison", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    runtime_root = args.runtime_root or (Path(env["MIKAN_SOFTWARE_VULKAN_ROOT"]) if env.get("MIKAN_SOFTWARE_VULKAN_ROOT") else None)
    if runtime_root:
        runtime_root = runtime_root.resolve()
        env["PATH"] = str(runtime_root / "usr/bin") + os.pathsep + env["PATH"]
        env["LD_LIBRARY_PATH"] = str(runtime_root / "usr/lib/x86_64-linux-gnu") + os.pathsep + env.get("LD_LIBRARY_PATH", "")
        env["VK_LAYER_PATH"] = str(runtime_root / "usr/share/vulkan/explicit_layer.d")
    icd_root = (runtime_root / "usr/share/vulkan/icd.d") if runtime_root else Path("/usr/share/vulkan/icd.d")
    candidates = sorted(icd_root.glob("lvp_icd*.json"))
    if not candidates:
        raise RuntimeError("Lavapipe ICD missing; install mesa-vulkan-drivers")
    env["VK_DRIVER_FILES"] = str(candidates[0])
    env["VK_ICD_FILENAMES"] = str(candidates[0])
    env["SDL_VIDEO_DRIVER"] = "x11"
    env["LP_NUM_THREADS"] = "2"
    env["MESA_SHADER_CACHE_DIR"] = str(output / "shader-cache")
    runtime = output / "runtime"
    runtime.mkdir(mode=0o700, exist_ok=True)
    runtime.chmod(0o700)
    env["XDG_RUNTIME_DIR"] = str(runtime)
    xvfb = shutil.which("Xvfb", path=env["PATH"])
    if not xvfb:
        raise RuntimeError("Xvfb missing")
    read_fd, write_fd = os.pipe()
    process = None
    try:
        with (output / "xvfb.log").open("w") as log:
            process = subprocess.Popen([xvfb, "-displayfd", str(write_fd), "-screen", "0", "1280x1024x24", "-nolisten", "tcp", "-ac"],
                                       pass_fds=(write_fd,), stdout=log, stderr=log, env=env)
            os.close(write_fd)
            write_fd = -1
            if not select.select([read_fd], [], [], 10)[0]:
                raise RuntimeError("Xvfb did not start within 10 seconds; see xvfb.log (local socket access is required)")
            display = os.read(read_fd, 64).decode().strip()
            if not display.isdigit():
                raise RuntimeError("Xvfb failed to report a display number; see xvfb.log")
            env["DISPLAY"] = ":" + display
            models = [root / "tests/fixtures/vox-mit" / name for name in ("chr_knight.vox", "teapot.vox", "dragon.vox", "menger.vox")]
            command = [str(args.executable.resolve()), *map(str, models), "--output", str(output)]
            if args.strict_comparison:
                command.append("--strict-comparison")
            with (output / "render.log").open("w") as render_log:
                result = subprocess.run(command,
                                        stdout=render_log, stderr=subprocess.STDOUT, env=env, timeout=180)
            if result.returncode:
                print((output / "render.log").read_text()[-12000:])
                raise RuntimeError(f"Vulkan raster validation failed with exit code {result.returncode}")
    finally:
        os.close(read_fd)
        if write_fd >= 0:
            os.close(write_fd)
        if process:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()

    report_path = output / "render-report.json"
    report = json.loads(report_path.read_text())
    assert report["device_type"] == "CPU" and report["sdl_video_driver"] == "x11"
    assert report["presented_frames"] == 16 and len(report["frames"]) == 8
    assert report["validation_errors"] == 0
    assert not report["postprocessing"] and not report["ray_tracing"] and not report["lighting_pass"]
    # This is a rendering/presentation smoke test. Pixel equivalence is a
    # separate diagnostic and is NEVER reported as passing when it differs.
    for frame in report["frames"]:
        assert frame["foreground_pixels"] > 100
    for field in ("vertex_shader", "fragment_shader"):
        report[field + "_sha256"] = hashlib.sha256((root / report[field]).read_bytes()).hexdigest()
    report["driver_manifest"] = str(candidates[0])
    report_path.write_text(json.dumps(report, indent=2) + "\n")

    from PIL import Image
    for bmp in output.glob("*.bmp"):
        with Image.open(bmp) as image:
            image.save(bmp.with_suffix(".png"))
    write_overviews(output)
    print(f"PASS: CPU Vulkan / SDL X11 / {report['presented_frames']} presented frames / zero validation errors")
    print(f"COMPARISON (separate from smoke test): albedo pixel-identical={report['albedo_pixel_identical']}; strict comparison={report['strict_comparison_pass']}")
    print(f"Color differences={sum(f['different_albedo_pixels'] for f in report['frames'])} pixels; coverage differences={sum(f['coverage_mismatch_pixels'] for f in report['frames'])} pixels")
    print(f"Report: {report_path}")
    print(f"Images: {output / 'overview.png'}")


if __name__ == "__main__":
    main()
