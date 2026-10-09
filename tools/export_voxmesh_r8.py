"""Export embedded VOX surface indices for inspection; no runtime input changes."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
from PIL import Image


def checksum(words):
    h = 2166136261
    for b in struct.pack('<%dI' % len(words), *words):
        h = ((h ^ b) * 16777619) & 0xffffffff
    return h


def leaves(words):
    assert words[0] == 0x534d5856 and checksum(words[:-1]) == words[-1]
    if words[1] == 4:
        assert words[2] == 2 and words[3] == 0x42464c02
        assert words[4] == len(words) - 6
        yield from leaves(words[5:-1])
    elif words[1] == 3:
        pos = 5
        for _ in range(words[3]):
            size = words[pos]
            yield from leaves(words[pos + 1:pos + 1 + size])
            pos += 1 + size
        assert pos + words[4] * 13 + 1 == len(words)
    else:
        assert words[1] in (2, 5), 'Expected v2/v5 leaf inside v4/v3'
        yield words


def pack_blocks(blocks):
    """Best-short-side MaxRects; no rotation, padding or filtering required for R8."""
    if not blocks:
        return 1, 1, {}
    area = sum(w * h for w, h, _ in blocks)
    minimum = max(w for w, _, _ in blocks)
    maximum = sum(w for w, _, _ in blocks)
    widths = {minimum, max(minimum, math.ceil(math.sqrt(area))), maximum}
    widths.update(range(((minimum + 7) // 8) * 8,
                        min(maximum, max(minimum, math.ceil(math.sqrt(area) * 2))) + 1, 8))
    order = sorted(range(len(blocks)), key=lambda i: (max(blocks[i][:2]),
                   blocks[i][0] * blocks[i][1]), reverse=True)
    best = None
    for width in sorted(widths):
        free = [(0, 0, width, sum(b[1] for b in blocks))]
        positions = {}
        used_width = used_height = 0
        for i in order:
            w, h, _ = blocks[i]
            choices = [(min(fw-w, fh-h), max(fw-w, fh-h), y, x)
                       for x, y, fw, fh in free if fw >= w and fh >= h]
            _, _, y, x = min(choices)
            positions[i] = (x, y)
            used_width, used_height = max(used_width, x+w), max(used_height, y+h)
            split = []
            for fx, fy, fw, fh in free:
                if x >= fx+fw or x+w <= fx or y >= fy+fh or y+h <= fy:
                    split.append((fx, fy, fw, fh))
                    continue
                if x > fx: split.append((fx, fy, x-fx, fh))
                if x+w < fx+fw: split.append((x+w, fy, fx+fw-x-w, fh))
                if y > fy: split.append((fx, fy, fw, y-fy))
                if y+h < fy+fh: split.append((fx, y+h, fw, fy+fh-y-h))
            split = list(dict.fromkeys(split))
            free = [r for j, r in enumerate(split) if not any(
                k != j and r[0] >= s[0] and r[1] >= s[1] and
                r[0]+r[2] <= s[0]+s[2] and r[1]+r[3] <= s[1]+s[3]
                for k, s in enumerate(split))]
        score = (max(used_width, used_height) > 1.5 * min(used_width, used_height),
                 used_width * used_height, abs(used_width-used_height))
        if best is None or score < best[0]:
            best = (score, used_width, used_height, positions)
    return best[1:]


def export(source, output):
    data = source.read_bytes()
    words = list(struct.unpack('<%dI' % (len(data) // 4), data))
    output.mkdir(parents=True, exist_ok=False)
    report = dict(source=str(source.resolve()), sha256=hashlib.sha256(data).hexdigest(),
                  runtime_input='embedded binary (unchanged)', meshes=[])
    for index, w in enumerate(leaves(words)):
        count, attr_count, planes = w[4], w[5], w[18]
        base = 19 + planes * 2
        attrs = w[base + count:-1]
        assert len(attrs) == attr_count
        palette_count = (attrs[1] - attrs[0]) // 2
        palette = [attrs[attrs[0] + i * 2:attrs[0] + i * 2 + 2]
                   for i in range(palette_count)]
        raw = struct.pack('<%dI' % (len(attrs) - attrs[2]), *attrs[attrs[2]:])
        prefix = 'mesh_%02d' % index
        # Exact packed R8 storage, including final uint32 padding; width is only
        # a display layout. Record byte count separately from PNG tail padding.
        raw_width = 256
        raw_height = max(1, math.ceil(len(raw) / raw_width))
        raw_image = Image.frombytes('L', (raw_width, raw_height),
                                   raw.ljust(raw_width * raw_height, b'\0'))
        raw_image.save(output / (prefix + '_embedded_r8.png'))
        assert Image.open(output / (prefix + '_embedded_r8.png')).tobytes()[:len(raw)] == raw
        rectangles, solids, blocks, block_ids = [], [], [], {}
        for q in range(count):
            packed = w[base + q]
            width, height = ((packed >> 16) & 255) + 1, (packed >> 24) + 1
            if w[1] == 5:
                descriptor = attrs[attrs[1] + q]
                value = descriptor & 0x7fffffff
                size = (width | (height << 16)) if descriptor & 0x80000000 else 0
            else:
                value, size = attrs[attrs[1] + q * 2:attrs[1] + q * 2 + 2]
            if not size:
                assert value < palette_count
                solids.append(dict(quad=q, palette_index=value, width=width, height=height))
                continue
            assert size == width | (height << 16)
            pixels = raw[value:value + width * height]
            assert len(pixels) == width * height and max(pixels) < palette_count
            key = (width, height, pixels)
            if key not in block_ids:
                block_ids[key] = len(blocks)
                blocks.append(key)
            rectangles.append((q, width, height, value, size, pixels, block_ids[key]))
        atlas_width, atlas_height, positions = pack_blocks(blocks)
        placements = []
        for q, width, height, value, size, pixels, block in rectangles:
            x, y = positions[block]
            placements.append(dict(quad=q, block=block, x=x, y=y, width=width, height=height,
                                   descriptor_value=value, descriptor_size=size))
        atlas = Image.new('L', (atlas_width, atlas_height))
        rgb = Image.new('RGB', atlas.size, (30, 30, 30))
        occupied = Image.new('L', atlas.size)
        for block, (width, height, pixels) in enumerate(blocks):
            tile = Image.frombytes('L', (width, height), pixels)
            pos = positions[block]
            box = (pos[0], pos[1], pos[0]+width, pos[1]+height)
            assert not occupied.crop(box).getbbox(), 'Atlas overlap'
            occupied.paste(255, box)
            atlas.paste(tile, pos)
            colors = bytes(c for p in pixels for c in
                           ((palette[p][0] >> 8) & 255, (palette[p][0] >> 16) & 255,
                            (palette[p][0] >> 24) & 255))
            rgb.paste(Image.frombytes('RGB', tile.size, colors), pos)
        atlas.save(output / (prefix + '_atlas_r8.png'))
        rgb.save(output / (prefix + '_atlas_color.png'))
        palette_image = Image.new('RGB', (256, 1))
        for p, (appearance, _) in enumerate(palette):
            palette_image.putpixel((p, 0), tuple((appearance >> shift) & 255 for shift in (8, 16, 24)))
        palette_image.save(output / (prefix + '_palette.png'))
        loaded = Image.open(output / (prefix + '_atlas_r8.png'))
        assert loaded.mode == 'L'
        for rect, placement in zip(rectangles, placements):
            xx, yy = placement['x'], placement['y']
            assert loaded.crop((xx, yy, xx + rect[1], yy + rect[2])).tobytes() == rect[5]
        report['meshes'].append(dict(mesh=index, quads=count, palette=palette,
                                     embedded_byte_count=len(raw), raw_png_size=raw_image.size,
                                     mixed_quads=len(rectangles), solid_quads=len(solids),
                                     unique_blocks=len(blocks),
                                     texture_texels=sum(b[0]*b[1] for b in blocks),
                                     packing_occupancy=sum(b[0]*b[1] for b in blocks)/(atlas.width*atlas.height),
                                     atlas_size=atlas.size, quads_atlas=placements,
                                     direct_palette_quads=solids))
    assert hashlib.sha256(source.read_bytes()).hexdigest() == report['sha256']
    (output / 'manifest.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(dict(output=str(output.resolve()), meshes=len(report['meshes']),
                         quads=sum(m['quads'] for m in report['meshes']),
                         verified='R8 bytes and all atlas rectangles roundtrip; source unchanged')))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    export(args.source, args.output)
