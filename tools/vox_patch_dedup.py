"""Analyze exact palette-index surface patch dedup; no asset modifications."""
import pathlib,json,collections,hashlib
import numpy as np
import vox_quad_stats as base

def key(a):return (a.shape,a.tobytes())
def variants(a):
    for k in range(4):
        b=np.rot90(a,k)
        yield key(b);yield key(np.fliplr(b))

def analyze(path):
    grid,chunks,sha=base.read_vox(path);occ=grid!=0;patches=[];solid=0;area=0
    for axis,sign in [(2,1),(2,-1),(0,-1),(0,1),(1,1),(1,-1)]:
        neighbor=np.zeros_like(occ);dst=[slice(None)]*3;src=dst.copy()
        dst[axis]=slice(0,-1) if sign>0 else slice(1,None);src[axis]=slice(1,None) if sign>0 else slice(0,-1)
        neighbor[tuple(dst)]=occ[tuple(src)];exposed=occ&~neighbor
        for plane in range(grid.shape[axis]):
            colors=np.take(grid,plane,axis=axis);mask=np.take(exposed,plane,axis=axis)
            left={(int(u),int(v)) for u,v in np.argwhere(mask)}
            while left:
                u,v=min(left);w=1
                while (u+w,v) in left:w+=1
                h=1
                while all((u+x,v+h) in left for x in range(w)):h+=1
                patch=colors[u:u+w,v:v+h].copy();area+=w*h
                if np.all(patch==patch[0,0]):solid+=1
                else:patches.append(patch)
                for x in range(w):
                    for y in range(h):left.remove((u+x,v+y))
    result={'source':str(path.resolve()),'sha256':sha,'solid_quads':solid,'mixed_quads':len(patches),'mixed_texels':sum(p.size for p in patches),'all_surface_texels':area,'units':'palette indices, 1 byte each; excludes descriptors, alignment, atlas gutters, mipmaps; no RGB equivalence or arbitrary subrectangle reuse'}
    for mode,canonical in [('exact',key),('rotation_flip',lambda a:min(variants(a)))]:
        groups=collections.defaultdict(list)
        for i,p in enumerate(patches):groups[canonical(p)].append(i)
        unique_bytes=sum(len(k[1]) for k in groups);raw=result['mixed_texels']
        result[mode]={'unique_patches':len(groups),'reused_instances':len(patches)-len(groups),'repeated_groups':sum(len(v)>1 for v in groups.values()),'stored_index_bytes':unique_bytes,'saved_index_bytes':raw-unique_bytes,'saved_percent':100*(raw-unique_bytes)/raw,'groups':[{'shape':list(k[0]),'instances':len(v),'patch_ids':v,'bytes_per_patch':len(k[1])} for k,v in groups.items() if len(v)>1]}
    expected=base.run(grid)['totals']['geometry_greedy'];assert solid==expected['solid_quads'] and len(patches)==expected['mixed_quads'] and area==expected['unit_face_area']
    return result
if __name__=='__main__':
    a=np.array([[1,2,3],[4,5,6]],dtype=np.uint8);assert min(variants(a))==min(variants(np.rot90(a)))==min(variants(np.fliplr(a)))
    assert key(a)!=key(a.T);assert min(variants(a))!=min(variants(a+1))
    r=analyze(pathlib.Path('projects/vox/vox model/vox/rom.vox'));r['validation']='patch counts and total area match base greedy; rotation/reflection synthetic tests passed'
    p=pathlib.Path('out/rom_quad_stats_20261007/dedup.json');p.write_text(json.dumps(r,indent=2),encoding='utf-8');print(json.dumps(r,indent=2))
