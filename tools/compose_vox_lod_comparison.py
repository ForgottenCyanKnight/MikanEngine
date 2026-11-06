"""Compose a benchmark figure from unchanged engine screenshots and measured data."""
import argparse
import json
import pathlib
import re
from PIL import Image, ImageDraw, ImageFont

parser = argparse.ArgumentParser()
parser.add_argument('directory', type=pathlib.Path)
parser.add_argument('--budgets', nargs='+', type=float, help='LOD triangle budget percentages, including LOD0')
parser.add_argument('--title', default='Menger')
parser.add_argument('--candidate-filtering', action='store_true')
args = parser.parse_args()
root = args.directory
summary = json.loads((root / 'summary.json').read_text(encoding='utf-8-sig'))
bake = json.loads((root / 'bake.json').read_text(encoding='utf-8-sig'))
component = bake['components'][0]
counts = [bake.get('candidate_instance_triangles', component['original_triangles']), *bake.get('candidate_lod_instance_triangles', component['lod_triangles'])]
levels = sorted(row['level'] for row in summary['aggregate'])
if args.candidate_filtering:
    counts = []
    for level in levels:
        text = (root / f'pair-1-lod-{level}.log').read_text(encoding='utf-8', errors='replace')
        matches = re.findall(r'VOX LOD selection.*?secondaryTriangles=(\d+)', text)
        assert matches, f'No scene triangle count for LOD {level}'
        counts.append(int(matches[-1]))
rows = summary['aggregate']
font = 'C:/Windows/Fonts/msyh.ttc'
title_font = ImageFont.truetype(font, 40)
label_font = ImageFont.truetype(font, 32)
small_font = ImageFont.truetype(font, 26)
images = [Image.open(root / f'lod-{level}.png').convert('RGB') for level in levels]
w, h = images[0].size
assert all(im.size == (w, h) for im in images), 'Screenshot sizes differ'
header, band, footer = 100, 124, 74
canvas = Image.new('RGB', (w * 2, header + (h + band) * ((len(levels)+1)//2) + footer), '#171c25')
draw = ImageDraw.Draw(canvas)
draw.text((28, 16), f'{args.title} · 固定 {len(levels)} 级 LOD · 同一游戏相机', font=title_font, fill='white')
first_log = (root / 'pair-1-lod-0.log').read_text(encoding='utf-8', errors='replace')
resolution = re.search(r'native geometry/DI/GI=(\d+x\d+)', first_log)
native = resolution[1] if resolution else 'unknown'
draw.text((w + 12, 26), f"{summary['gpu']} | {native} 主命中 | 半分辨率 GI/GGX | 每级 {rows[0]['samples']} 帧", font=small_font, fill='#b9c6d7')
baseline = rows[0]['gpu_ms']
report = [f'# {args.title} isolated LOD comparison', '',
          f"GPU preference {summary['gpu']}; hardware triangles; native {native}; half-resolution GI/GGX.",
          'Each process loads one fixed level; paired runs reverse the level order.',
          f"Each run warms up {summary['warmup']} frames before sampling. Captures use fixed dt and frame 420.", '',
          '| LOD | Triangles | BLAS MiB | GPU ms | Primary ms | Lighting ms | App FPS | GPU throughput gain |',
          '| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |']
for row, image in zip(rows, images):
    level = row['level']
    log = (root / f'pair-1-lod-{level}.log').read_text(encoding='utf-8', errors='replace')
    resident = re.findall(r'VOX resident memory.*?blas=(\d+)', log)
    assert resident, f'No resident memory for LOD {level}'
    mib = int(resident[-1]) / 1024**2
    gain = (baseline / row['gpu_ms'] - 1) * 100
    row.update(triangles=counts[level], blas_mib=mib, gpu_throughput_gain_percent=gain)
    x, y = (level % 2) * w, header + (level // 2) * (h + band)
    percent = args.budgets[level] if args.budgets else round(100 * counts[level] / counts[0], 2)
    draw.text((x + 24, y + 8), f'LOD{level} · 预算 {percent:g}% · {counts[level]:,} 三角形 · BLAS {mib:.2f} MiB', font=label_font, fill='white')
    draw.text((x + 24, y + 56), f"GPU {row['gpu_ms']:.3f} ms | 主命中 {row['primary_ms']:.3f} ms | 光照 {row['lighting_ms']:.3f} ms | 应用 {row['app_fps']:.1f} FPS | GPU增益 {gain:+.1f}%", font=small_font, fill='#bed2eb')
    canvas.paste(image, (x, y + band))
    report.append(f"| {level} | {counts[level]:,} | {mib:.2f} | {row['gpu_ms']:.3f} | {row['primary_ms']:.3f} | {row['lighting_ms']:.3f} | {row['app_fps']:.1f} | {gain:+.2f}% |")
note = '候选组件固定所选 LOD，其余组件保持原始几何；三角形数为全场景。' if args.candidate_filtering else '每次仅建立所选层级 BLAS / 着色数据。'
draw.text((24, canvas.height - 57), note + '距离选择关闭；时间为两轮平均，GPU 增益相对 LOD0。', font=small_font, fill='#b9c6d7')
canvas.save(root / 'lod-comparison.png')
(root / 'report.md').write_text('\n'.join(report) + '\n', encoding='utf-8')
(root / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
print(root / 'lod-comparison.png')
