"""Cross-check production component statistics against the current compiled VOX.

Usage: python tools/summarize_vox_components.py report.json --compiled file.voxmesh
The grouping is expected default triangle-BLAS sharing, not measured GPU memory.
"""
import argparse
import csv
import hashlib
import json
import pathlib
import struct
import zlib


def compiled_meshes(path):
    blob=path.read_bytes()
    h=struct.unpack_from('<12I',blob)
    if h[0]!=0x5a4d5856 or h[1]!=1 or len(blob)!=48+h[10]:raise ValueError('invalid compiled container')
    raw=zlib.decompress(blob[48:])
    if len(raw)!=h[8]:raise ValueError('compiled raw length mismatch')
    words=memoryview(raw).cast('I')
    def unwrap(w):
        if w[0]!=0x534d5856:raise ValueError('invalid surface magic')
        if w[1]==4:return unwrap(w[5:5+w[4]])
        if w[1] in (6,7):return unwrap(w[8:8+w[3]])
        return w
    root=unwrap(words)
    if root[1]!=3:raise ValueError('expected a compiled component assembly')
    offset=5;meshes=[]
    for _ in range(root[3]):
        size=root[offset];offset+=1
        leaf=unwrap(root[offset:offset+size]);offset+=size
        if leaf[1] not in (5,8):raise ValueError('expected compact triangle surface')
        n=leaf[4];planes=leaf[18];base=19+2*planes
        if base+n+leaf[5]+1!=len(leaf):raise ValueError('invalid compact surface length')
        # Geometry-only identity: normalized bounds, plane runs, packed quads,
        # and direction counts. Colors, palette and materials do not split BLAS.
        digest=hashlib.sha256()
        digest.update(leaf[6:18].cast('B'))
        digest.update(leaf[19:base+n].cast('B'))
        meshes.append(dict(quads=n,geometry=digest.hexdigest()))
    if offset+root[4]*13+1!=len(root):raise ValueError('invalid compiled instance table')
    uses=[0]*len(meshes)
    for i in range(root[4]):uses[root[offset+13*i]]+=1
    return meshes,uses


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report',type=pathlib.Path)
    parser.add_argument('--compiled',required=True,type=pathlib.Path)
    args=parser.parse_args()
    report=json.loads(args.report.read_text(encoding='utf-8'))
    meshes,uses=compiled_meshes(args.compiled)
    rows=report['components'];used=[r for r in rows if r['runtime_component']>=0]
    if len(used)!=len(meshes):raise ValueError('source and compiled component counts differ')
    groups={}
    for row in used:
        index=row['runtime_component'];mesh=meshes[index]
        if mesh['quads']!=row['final_quads'] or uses[index]!=row['instances']:
            raise ValueError(f"component {index}: source bake and compiled cache differ; regenerate the cache before ranking")
        key=mesh['geometry']
        group=groups.setdefault(key,dict(blas_group=len(groups),geometry_hash=key,models=[],runtime_components=[],instances=0,
            triangles=row['triangles'],merge_factor_min=row['merge_factor'],merge_factor_max=row['merge_factor'],
            instance_weighted_triangles=0,proxy_candidate=False))
        group['models'].append(row['model_index']);group['runtime_components'].append(index)
        group['instances']+=row['instances'];group['instance_weighted_triangles']+=row['instance_weighted_triangles']
        group['merge_factor_min']=min(group['merge_factor_min'],row['merge_factor'])
        group['merge_factor_max']=max(group['merge_factor_max'],row['merge_factor'])
        group['proxy_candidate']|=row['proxy_candidate'];row['blas_group']=group['blas_group']
    groups=sorted(groups.values(),key=lambda g:g['instance_weighted_triangles'],reverse=True)
    total=report['summary']['instance_weighted_triangles']
    for g in groups:g['instance_triangle_share_percent']=100*g['instance_weighted_triangles']/total if total else 0
    report['compiled_verification']={'file':str(args.compiled.resolve()),'components':'all counts match','instances':'all counts match',
        'expected_unique_triangle_blas':len(groups),'unique_geometry_triangles':sum(g['triangles'] for g in groups),
        'scope':'geometry-only grouping for default sharing, DAG disabled; no AS allocation bytes or per-component traversal time measured'}
    report['blas_groups']=groups
    hist=[]
    for lo,hi in ((1,2),(2,4),(4,8),(8,16),(16,float('inf'))):
        selected=[r for r in used if lo<=r['merge_factor']<hi]
        triangles=sum(r['instance_weighted_triangles'] for r in selected)
        hist.append(dict(merge_factor=f'{lo}–{hi if hi!=float("inf") else "∞"}',components=len(selected),
            instance_triangles=triangles,triangle_share_percent=100*triangles/total if total else 0))
    report['merge_histogram']=hist
    args.report.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    with args.report.with_suffix('.blas.csv').open('w',newline='',encoding='utf-8-sig') as stream:
        fields=['blas_group','models','runtime_components','instances','triangles','merge_factor_min','merge_factor_max','instance_weighted_triangles','instance_triangle_share_percent','proxy_candidate']
        writer=csv.DictWriter(stream,fieldnames=fields,extrasaction='ignore');writer.writeheader()
        for group in groups:writer.writerow({k:(';'.join(map(str,v)) if isinstance(v,list) else v) for k,v in group.items()})
    summary=report['summary'];threshold=report['thresholds']
    lines=['# 城堡逐组件／BLAS 几何合并报告','',
        f"源文件：`{report['source']}`。复用生产 `BuildVoxelFaces`，按当前静态接触回填设置重新计算；与 `{args.compiled.name}` 的全部组件最终四边形数、实例数逐项吻合。",'',
        f"{summary['source_models']} 个源模型，{summary['used_components']} 个使用中的组件，{summary['instances']} 个实例；按默认几何共享规则预计 {len(groups)} 份唯一三角形 BLAS 几何。",'',
        f"组件几何三角形合计 {summary['component_triangles']:,}；共享后唯一几何 {sum(g['triangles'] for g in groups):,}；实例加权 {total:,}。这三个口径分别表示组件规模、共享几何规模和展开后的场景规模，不代表实际 GPU 遍历次数。",'',
        '## 指标与筛选','',
        '- 合并倍率 = 原始暴露单位面数 ÷ 最终四边形数；减面率 = 1 − 最终四边形数 ÷ 暴露单位面数。隐藏接触回填面不计入原始暴露面。',
        '- 普通 greedy 与接触回填后的结果分别列出，不能把 BLAS 压实当作几何减面。',
        '- CSV 包含模型索引、运行时组件索引、实例数、占用率、属性字节数和实例加权占比；JSON 另保留占用尺寸。索引从 0 开始。属性字节数不是 BLAS 存储大小。',
        f"- 当前代理候选阈值：单组件 ≥ {threshold['minimum_triangles']:,} 三角形，且合并倍率 ≤ {threshold['maximum_merge_factor']:g}；共 {summary['proxy_candidates']} 个候选，覆盖 {summary['proxy_instance_triangle_share_percent']:.2f}% 的实例加权三角形。阈值用于几何筛选，不是 GPU 时间结论。",
        '- 小于等于 1024 三角形的组件另作标记，不能只因合并倍率低就优先做代理。跨组件批处理还需空间邻接信息，且应避免破坏实例共享。','',
        '## 合并倍率分布','', '| 倍率区间 | 组件数 | 实例加权三角形占比 |','|---|---:|---:|']
    lines += [f"| {r['merge_factor']} | {r['components']} | {r['triangle_share_percent']:.2f}% |" for r in hist]
    lines += ['', '## 实例加权规模最大的 BLAS 几何组','', '| 组 | 源模型（最多列 8 个） | 实例数 | 单份三角形 | 合并倍率范围 | 场景三角形占比 | 代理候选 |','|---|---|---:|---:|---:|---:|---|']
    for g in groups[:20]:
        models=', '.join(map(str,g['models'][:8]))+(' …' if len(g['models'])>8 else '')
        lines.append(f"| {g['blas_group']} | {models} | {g['instances']} | {g['triangles']:,} | {g['merge_factor_min']:.2f}–{g['merge_factor_max']:.2f} | {g['instance_triangle_share_percent']:.2f}% | {'是' if g['proxy_candidate'] else '否'} |")
    lines += ['', '## 重新统计与调整阈值', '', '```powershell',
        './tools/build.ps1 -Target MikanVoxComponentStats',
        './out/build/x64-release/MikanVoxComponentStats.exe C:/Users/Forgo/Downloads/castle.vox --output out/bench-rt-test/component-stats/castle --min-triangles 10000 --max-merge-factor 4',
        'python tools/summarize_vox_components.py out/bench-rt-test/component-stats/castle.json --compiled C:/Users/Forgo/Downloads/castle.voxmesh',
        '```', '', '原生工具只重算 CPU 几何并写 JSON/CSV；Python 脚本核对现有编译资产、生成 BLAS 分组 CSV 和本报告。不初始化 Vulkan，不改变场景或自动替换代理。']
    args.report.with_suffix('.md').write_text('\n'.join(lines)+'\n',encoding='utf-8')
    print(json.dumps(dict(verification=report['compiled_verification'],histogram=hist,top_groups=groups[:5]),ensure_ascii=False,indent=2))


if __name__=='__main__':main()
