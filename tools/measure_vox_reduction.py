"""Batch source VOX reduction analysis using the engine's CPU-only baker.

Example: python tools/measure_vox_reduction.py --root projects --threshold 50
Requires MikanVoxRTValidate.exe built with --surface-stats support.
No source VOX, project JSON or adjacent cooked assets are written.
"""
import argparse
import concurrent.futures
import csv
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


def main():
    engine = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, action='append', help='Repeat for multiple roots; default: projects')
    parser.add_argument('--exe', type=Path, default=engine/'out/build/x64-Release/MikanVoxRTValidate.exe')
    parser.add_argument('--out', type=Path, default=engine/'out/vox-reduction-scan'/datetime.datetime.now().strftime('%Y%m%d_%H%M%S'))
    parser.add_argument('--threshold', type=float, default=50, help='Strictly below this reduction percentage')
    parser.add_argument('--workers', type=int, default=2)
    parser.add_argument('--timeout', type=float, default=180, help='Seconds per unique source')
    parser.add_argument('--uniform-materials', action='store_true')
    args = parser.parse_args()
    if not 0 <= args.threshold <= 100 or not 1 <= args.workers <= 8:
        parser.error('threshold must be 0..100; workers must be 1..8')
    exe, output = args.exe.resolve(), args.out.resolve()
    if not exe.is_file(): parser.error(f'Executable not found: {exe}')
    roots = [(p if p.is_absolute() else engine/p).resolve() for p in (args.root or [Path('projects')])]
    for root in roots:
        if not root.is_dir(): parser.error(f'Root not found: {root}')
    output.mkdir(parents=True, exist_ok=False)
    paths = sorted({p.resolve() for root in roots for p in root.rglob('*.vox')
                    if not any(part in ('.meta','out','.git') for part in p.relative_to(root).parts)}, key=lambda p:str(p).casefold())
    groups = {}
    for path in paths:
        with path.open('rb') as stream: digest = hashlib.file_digest(stream,'sha256').hexdigest()
        groups.setdefault(digest,[]).append(path)
    env = os.environ.copy()
    env['MIKAN_VOX_STATIC_CONTACT_BACKFILL'] = '1'
    env.pop('MIKAN_HWRT_VOXEL_DDA',None)
    manifest = dict(roots=list(map(str,roots)), files=[dict(path=str(p),sha256=h) for h,ps in groups.items() for p in ps],
                    executable=str(exe), executable_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                    game_sha256=hashlib.sha256(exe.with_name('Game.dll').read_bytes()).hexdigest(),
                    uniform_materials=args.uniform_materials, contact_backfill=True,
                    threshold_percent=args.threshold, workers=args.workers,
                    metric='100 * (1 - finalQuadCount/exposedFaceCount); strict less-than; empty geometry excluded')
    (output/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2),encoding='utf-8')

    def measure(item):
        digest, sources = item
        job = output/'jobs'/digest
        job.mkdir(parents=True)
        target = job/'stats.json'
        command = [str(exe),'--surface-stats',str(sources[0]),str(target)]
        if args.uniform_materials: command.append('--uniform-materials')
        (job/'command.json').write_text(json.dumps(command,ensure_ascii=False),encoding='utf-8')
        start = time.perf_counter()
        try:
            with (job/'stdout.log').open('wb') as log:
                process = subprocess.run(command,cwd=job,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=args.timeout)
            if process.returncode: raise RuntimeError(f'exit code {process.returncode}')
            data = json.loads(target.read_text(encoding='utf-8-sig'))
            raw, quads = data['exposedFaceCount'], data['finalQuadCount']
            if not (0 <= quads <= raw and data['triangleCount']==quads*2):
                raise RuntimeError('invalid surface counts')
            data.update(status='ok' if raw else 'empty',reductionPercent=data['reductionFraction']*100,
                        greedyReductionPercent=data['greedyReductionFraction']*100)
        except Exception as error:
            data = dict(status='error',error=str(error))
        data.update(sha256=digest,seconds=time.perf_counter()-start,log=str(job/'stdout.log'))
        return sources,data

    rows = []
    print(f'Files={len(paths)} unique={len(groups)} output={output}',flush=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
        futures = [pool.submit(measure,item) for item in groups.items()]
        for index,future in enumerate(concurrent.futures.as_completed(futures),1):
            sources,data = future.result()
            rows.extend(dict(path=str(path),**data) for path in sources)
            if index%10==0 or index==len(groups): print(f'Completed {index}/{len(groups)} unique assets',flush=True)
    rows.sort(key=lambda r:(r.get('reductionPercent',float('inf')), -r.get('exposedFaceCount',0),r['path'].casefold()))
    low = [r for r in rows if r['status']=='ok' and r['reductionPercent']<args.threshold]
    greedy_low = sorted([r for r in rows if r['status']=='ok' and r['greedyReductionPercent']<args.threshold],
                        key=lambda r:(r['greedyReductionPercent'],-r['exposedFaceCount'],r['path'].casefold()))
    errors = [r for r in rows if r['status']=='error']
    summary = dict(files=len(rows),unique_files=len(groups),ok=sum(r['status']=='ok' for r in rows),
                   empty=sum(r['status']=='empty' for r in rows),errors=len(errors),below_threshold=len(low),
                   unique_below_threshold=len({r['sha256'] for r in low}),threshold_percent=args.threshold)
    summary.update(below_greedy_threshold=len(greedy_low),unique_below_greedy_threshold=len({r['sha256'] for r in greedy_low}))
    (output/'results.json').write_text(json.dumps(dict(summary=summary,below_threshold=low,below_greedy_threshold=greedy_low,results=rows),ensure_ascii=False,indent=2),encoding='utf-8')
    columns=['path','status','reductionPercent','greedyReductionPercent','exposedFaceCount','greedyQuadCount','finalQuadCount','triangleCount','mergeFactor','sha256','seconds','error']
    for name,selected in [('all.csv',rows),('below_threshold.csv',low),('below_greedy_threshold.csv',greedy_low)]:
        with (output/name).open('w',encoding='utf-8-sig',newline='') as stream:
            writer=csv.DictWriter(stream,fieldnames=columns,extrasaction='ignore');writer.writeheader();writer.writerows(selected)
    lines=['# VOX reduction scan','',f'Files: {len(rows)}; unique content: {len(groups)}; below {args.threshold:g}%: {len(low)} files / {summary["unique_below_threshold"]} unique assets; errors: {len(errors)}.',
           '', 'Metric: `100 * (1 - finalQuadCount / exposedFaceCount)`. Assembly counts are instance-weighted. Default source materials with static-contact backfill enabled; entity-level uniform/emission overrides are not inferred.',
           '', '| File | Reduction | Exposed faces | Final quads | Triangles |','|---|---:|---:|---:|---:|']
    for row in low:
        label=os.path.relpath(row['path'],engine).replace('\\','/').replace('|','\\|')
        lines.append(f'| {label} | {row["reductionPercent"]:.2f}% | {row["exposedFaceCount"]} | {row["finalQuadCount"]} | {row["triangleCount"]} |')
    lines+=['',f'## Ordinary greedy reduction below {args.threshold:g}%','',
            'This excludes the additional effect of contact backfill. Duplicate paths are retained; SHA-256 identifies repeated content.',
            '', '| File | Greedy reduction | Final reduction | Exposed faces | Greedy quads | Final quads |',
            '|---|---:|---:|---:|---:|---:|']
    for row in greedy_low:
        label=os.path.relpath(row['path'],engine).replace('\\','/').replace('|','\\|')
        lines.append(f'| {label} | {row["greedyReductionPercent"]:.2f}% | {row["reductionPercent"]:.2f}% | {row["exposedFaceCount"]} | {row["greedyQuadCount"]} | {row["finalQuadCount"]} |')
    if errors:
        lines+=['','Errors (not classified as low reduction):']+[f'- {r["path"]}: {r["error"]}' for r in errors]
    (output/'report.md').write_text('\n'.join(lines)+'\n',encoding='utf-8')
    print(json.dumps(summary,ensure_ascii=False),flush=True)
    return 1 if errors else 0


if __name__=='__main__':
    raise SystemExit(main())
