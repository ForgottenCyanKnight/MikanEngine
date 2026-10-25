"""Compose a benchmark figure from unchanged engine screenshots and measured data."""
import argparse
import json
import pathlib
import re
from PIL import Image, ImageDraw, ImageFont

parser = argparse.ArgumentParser()
parser.add_argument('directory', type=pathlib.Path)
parser.add_argument('--budgets', nargs=4, type=float, help='LOD triangle budget percentages, including LOD0')
args = parser.parse_args()
root = args.directory
summary = json.loads((root / 'summary.json').read_text(encoding='utf-8-sig'))
bake = json.loads((root / 'bake.json').read_text(encoding='utf-8-sig'))
component = bake['components'][0]
counts = [component['original_triangles'], *component['lod_triangles']]
rows = summary['aggregate']
font = 'C:/Windows/Fonts/msyh.ttc'
title_font = ImageFont.truetype(font, 40)
label_font = ImageFont.truetype(font, 32)
small_font = ImageFont.truetype(font, 26)
images = [Image.open(root / f'lod-{level}.png').convert('RGB') for level in range(4)]
w, h = images[0].size
assert all(im.size == (w, h) for im in images), 'Screenshot sizes differ'
header, band, footer = 100, 124, 74
canvas = Image.new('RGB', (w * 2, header + (h + band) * 2 + footer), '#171c25')
draw = ImageDraw.Draw(canvas)
draw.text((28, 16), 'Menger · 独立加载四级 LOD · 同一游戏相机', font=title_font, fill='white')
draw.text((w + 12, 26), f"{summary['gpu']} | 960×540 主命中 | 半分辨率 GI/GGX | 每级 {rows[0]['samples']} 帧", font=small_font, fill='#b9c6d7')
baseline = rows[0]['gpu_ms']
report = ['# Menger isolated LOD comparison', '',
          'AMD Radeon 780M; hardware triangles; native 960×540; half-resolution GI/GGX.',
          'Each process loads one fixed level. Two rounds: 0/1/2/3 then 3/2/1/0.',
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
draw.text((24, canvas.height - 57), '每次仅建立所选层级 BLAS / 着色数据；距离选择关闭；原始截图无修饰。时间为两轮平均，GPU 增益相对 LOD0。', font=small_font, fill='#b9c6d7')
canvas.save(root / 'lod-comparison.png')
(root / 'report.md').write_text('\n'.join(report) + '\n', encoding='utf-8')
(root / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
print(root / 'lod-comparison.png')
