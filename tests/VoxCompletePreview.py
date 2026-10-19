"""Run the native production NEE/GI + Bruneton CPU still renderer."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

p = argparse.ArgumentParser()
p.add_argument('executable', type=Path)
p.add_argument('--runtime-root', type=Path, required=True)
p.add_argument('--output', type=Path, default=Path('out/knight-540p'))
p.add_argument('--size', type=int, nargs=2, default=[960, 540])
p.add_argument('--frames', type=int, default=32)
p.add_argument('--pitch', type=float, help='downward camera pitch in degrees, e.g. 2 for near eye level')
p.add_argument('--nrd', action='store_true', help='run production NRD histories and export single-frame comparisons')
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
exe, runtime, output = a.executable.resolve(), a.runtime_root.resolve(), a.output.resolve()
output.mkdir(parents=True, exist_ok=True)
env = os.environ.copy()
env['LD_LIBRARY_PATH'] = str(runtime/'usr/lib/x86_64-linux-gnu') + ':' + env.get('LD_LIBRARY_PATH','')
env['VK_LAYER_PATH'] = str(runtime/'usr/share/vulkan/explicit_layer.d')
icd = sorted((runtime/'usr/share/vulkan/icd.d').glob('lvp_icd*.json'))[0]
env['VK_DRIVER_FILES'] = env['VK_ICD_FILENAMES'] = str(icd)
env['LP_NUM_THREADS'] = '2'
env['MESA_SHADER_CACHE_DIR'] = str(runtime/'cache')
xdg = output/'runtime'; xdg.mkdir(mode=0o700, exist_ok=True); xdg.chmod(0o700)
env['XDG_RUNTIME_DIR'] = str(xdg)
env.pop('DISPLAY',None)
noise=exe.parent/'full-preview/engine/textures/stbn/stbn_vec2_128x128x64.rgba8.bin'
assert noise.is_file() and hashlib.sha256(noise.read_bytes()).hexdigest()=='e2c8e1156baba84e7d7c47c9fab5c70d6628c328a3dcad48d0394758b0729841', 'STBN missing or wrong; see docs/KNIGHT_540P.md'
command = [str(exe), str(root/'tests/fixtures/vox-mit/chr_knight.vox'), str(output), *map(str,a.size), str(a.frames)]
if a.pitch is not None or a.nrd:
    command.append(str(a.pitch if a.pitch is not None else 2))
if a.nrd:
    command.append('nrd')
with (output/'render.log').open('w') as log:
    result = subprocess.run(command,cwd=exe.parent/'full-preview',env=env,stdout=log,stderr=subprocess.STDOUT,timeout=7200)
if result.returncode:
    print((output/'render.log').read_text()[-12000:])
    raise RuntimeError(f'native renderer exited {result.returncode}')
report = json.loads((output/'complete-report.json').read_text())
assert report['width'] == a.size[0] and report['height'] == a.size[1]
assert report['device_type'] == 'CPU' and report['atmosphere_orders'] == 4
if a.pitch is not None:
    assert abs(report['camera_pitch_degrees']-a.pitch)<1e-4
if a.nrd:
    assert report['denoiser'] and report['nrd_frames_processed']==a.frames
    assert not report['nrd_external_frame_average']
assert report['hdr_finite'] and report['validation_errors'] == 0
assert report['physical_sun'] and report['stbn'] and report['gi_probe']
assert report['gi_energy_sum'] > 0 and report['geometry_pixels_union'] > 100
report['shader_sha256'] = {str(path.relative_to(root)):hashlib.sha256(path.read_bytes()).hexdigest() for path in sorted((root/'engine/shaders/glsl').glob('*')) if path.is_file() and (path.suffix == '.glsl' or path.name.startswith(('atmo_','vox_rt_')) or path.name in ('tonemap.frag','hardware_rt_present.frag'))}
report['driver_manifest'] = str(icd)
report['primary_samples_per_pixel'] = report['accumulated_frames']
report['gi_sample_budget_per_surface_pixel'] = report['accumulated_frames']*report['gi_samples_per_frame']
report['full_resolution_accumulation_ms'] = sum(report['frame_ms'])
(output/'complete-report.json').write_text(json.dumps(report,indent=2)+'\n')
from PIL import Image
for bmp in output.glob('knight-*.bmp'):
    with Image.open(bmp) as image:
        assert image.size == tuple(a.size)
        image.convert('RGB').save(bmp.with_suffix('.png'))
if a.nrd:
    from PIL import ImageChops
    with Image.open(output/'knight-raw-last.png') as raw, Image.open(output/'knight-nrd-last.png') as filtered:
        difference=ImageChops.difference(raw,filtered)
        assert difference.getbbox(), 'NRD output is identical to raw input'
        report['nrd_changed_pixel_bbox']=list(difference.getbbox())
        report['nrd_output_is_last_frame']=True
        report['nrd_backend']='NRD 4.18.0 REBLUR_DIFFUSE + REBLUR_SPECULAR'
        if a.size==[960,540] and a.pitch==2:
            roi=(0,0,200,540)
            assert ImageChops.difference(raw.crop(roi),filtered.crop(roi)).getbbox() is None
            report['sky_control_region']=list(roi)
            report['sky_control_region_unchanged']=True
    (output/'complete-report.json').write_text(json.dumps(report,indent=2)+'\n')
print(f'PASS {a.size}, frames={a.frames}; atmosphere_ms={report["atmosphere_lut_ms"]:.2f}; total_ms={report["total_ms"]:.2f}; validation={report["validation_errors"]}/{report["validation_warnings"]}')
