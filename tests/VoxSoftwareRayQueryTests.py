"""Run the native engine Ray Query shader on CPU Vulkan; Python traces no rays."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("executable", type=Path)
    parser.add_argument("--output", type=Path, default=Path("out/software-ray-query"))
    parser.add_argument("--runtime-root", type=Path)
    parser.add_argument("--size", nargs=2, type=int, default=[128, 96])
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    runtime_root = args.runtime_root or (Path(env["MIKAN_SOFTWARE_VULKAN_ROOT"]) if env.get("MIKAN_SOFTWARE_VULKAN_ROOT") else None)
    if runtime_root:
        runtime_root = runtime_root.resolve()
        env["LD_LIBRARY_PATH"] = str(runtime_root / "usr/lib/x86_64-linux-gnu") + os.pathsep + env.get("LD_LIBRARY_PATH", "")
        env["VK_LAYER_PATH"] = str(runtime_root / "usr/share/vulkan/explicit_layer.d")
    icd_root = runtime_root / "usr/share/vulkan/icd.d" if runtime_root else Path("/usr/share/vulkan/icd.d")
    candidates = sorted(icd_root.glob("lvp_icd*.json"))
    if not candidates:
        raise RuntimeError("Lavapipe ICD missing")
    env["VK_DRIVER_FILES"] = env["VK_ICD_FILENAMES"] = str(candidates[0])
    env["LP_NUM_THREADS"] = "2"
    env["MESA_SHADER_CACHE_DIR"] = str(output / "shader-cache")
    runtime = output / "runtime"
    runtime.mkdir(mode=0o700, exist_ok=True)
    runtime.chmod(0o700)
    env["XDG_RUNTIME_DIR"] = str(runtime)
    env.pop("DISPLAY", None)  # No window system or Xvfb is needed for this path.
    models = [root / "tests/fixtures/vox-mit" / name for name in ("chr_knight.vox", "teapot.vox", "dragon.vox", "menger.vox")]
    command = [str(args.executable.resolve()), *map(str, models), "--size", *map(str, args.size), "--output", str(output)]
    with (output / "ray-query.log").open("w") as log:
        result = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=180)
    if result.returncode:
        print((output / "ray-query.log").read_text()[-12000:])
        raise RuntimeError(f"native Ray Query preview failed: {result.returncode}")
    report_path = output / "ray-query-report.json"
    report = json.loads(report_path.read_text())
    assert report["device_type"] == "CPU" and report["samples_per_pixel"] == 4
    assert report["buffer_device_address"] and report["acceleration_structure"] and report["ray_query"]
    assert report["width"] == args.size[0] and report["height"] == args.size[1]
    assert not report["rasterization"] and not report["postprocessing"]
    assert report["validation_errors"] == 0 and len(report["models"]) == 4
    for model in report["models"]:
        assert model["blas_bytes"] > 0 and model["tlas_bytes"] > 0
        assert model["triangles"] > 0 and len(model["frames"]) == 2
        for frame in model["frames"]:
            assert frame["negative_control"] == "pass"
            assert frame["primary_rays"] == args.size[0] * args.size[1] * 4
            assert frame["foreground_pixels"] > 100
    for field in ("shader", "geometry_shader"):
        report[field + "_sha256"] = hashlib.sha256((root / report[field]).read_bytes()).hexdigest()
    report["driver_manifest"] = str(candidates[0])
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    from PIL import Image, ImageDraw
    for bmp in output.glob("*.bmp"):
        with Image.open(bmp) as image:
            image.save(bmp.with_suffix(".png"))
    width, height = args.size
    overview = Image.new("RGB", (width * 2, (height + 28) * 2), (15, 20, 28))
    draw = ImageDraw.Draw(overview)
    for index, model in enumerate(report["models"]):
        x, y = (index % 2) * width, (index // 2) * (height + 28)
        draw.text((x + 6, y + 8), model["model"] + " | Ray Query | 4 spp", fill="white")
        with Image.open(output / model["frames"][0]["bmp"]) as image:
            overview.paste(image.convert("RGB"), (x, y + 28))
    overview.save(output / "overview.png")
    print(f"PASS: CPU Vulkan Ray Query, 4 models x 2 views, {width}x{height}, 4 primary samples/pixel")
    print(f"Validation errors={report['validation_errors']} warnings={report['validation_warnings']}; no rasterization or postprocess pipeline")
    for model in report["models"]:
        times = [round(frame["ray_query_dispatch_ms"], 2) for frame in model["frames"]]
        print(f"{model['model']}: triangles={model['triangles']}, dispatch_ms={times}, build_ms={model['expand_and_as_build_ms']:.2f}")
    print(f"Report: {report_path}")


if __name__ == "__main__":
    main()
