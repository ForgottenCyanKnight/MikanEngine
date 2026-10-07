"""CPU-only VOX face/greedy statistics; no renderer or assets modified.
Static first-frame scene assembly follows VoxSceneAssembly.h.
Geometry greedy uses identical slice/color grouping and lexicographic (u,v)
width-first rectangle order to VoxRenderer, except color is ignored when requested.
"""
import argparse, collections, hashlib, json, pathlib, struct, time
import numpy as np

def read_vox(path):
    data=path.read_bytes()
    if data[:4]!=b'VOX ': raise ValueError('Not VOX')
    models=[];size=None;nodes={};refs=set();hidden_layers=set();chunks=collections.Counter();p=20
    while p<len(data):
        if p+12>len(data): raise ValueError('Truncated chunk')
        kind,n,children=struct.unpack_from('<4sII',data,p);p+=12
        if p+n+children>len(data): raise ValueError('Truncated payload')
        payload=data[p:p+n];chunks[kind.decode()]+=1
        if kind==b'SIZE':size=struct.unpack_from('<III',payload)
        elif kind==b'XYZI':
            count=struct.unpack_from('<I',payload)[0]
            if len(payload)!=4+count*4 or size is None:raise ValueError('Invalid XYZI')
            models.append((size,np.frombuffer(payload[4:],dtype=np.uint8).reshape(-1,4).copy()))
        elif kind in (b'nTRN',b'nGRP',b'nSHP',b'LAYR'):
            cursor=0
            def integer():
                nonlocal cursor
                value=struct.unpack_from('<i',payload,cursor)[0];cursor+=4;return value
            def string():
                nonlocal cursor
                n=integer()
                if n<0 or cursor+n>len(payload):raise ValueError('Invalid string')
                value=payload[cursor:cursor+n].decode('utf-8');cursor+=n;return value
            def dictionary():
                d={}
                for _ in range(integer()):
                    key=string();d[key]=string()
                return d
            ident=integer();attrs=dictionary()
            if kind==b'LAYR':
                if attrs.get('_hidden')=='1':hidden_layers.add(ident)
            else:
                node={'kind':kind,'attrs':attrs,'children':[],'r':np.eye(3,dtype=int),'t':np.zeros(3,dtype=int),'layer':-1}
                if kind==b'nTRN':
                    child=integer();node['children']=[child];refs.add(child);integer();node['layer']=integer()
                    frames=[dictionary() for _ in range(integer())];f=min(frames,key=lambda d:int(d.get('_f',0)))
                    if f.get('_hidden')=='1':attrs['_hidden']='1'
                    node['t']=np.array([int(x) for x in f.get('_t','0 0 0').split()])
                    if '_r' in f:
                        code=int(f['_r']);a=code&3;b=(code>>2)&3
                        if a>2 or b>2 or a==b:raise ValueError('Invalid rotation')
                        rot=np.zeros((3,3),dtype=int)
                        for row,axis in enumerate((a,b,3-a-b)):rot[row,axis]=-1 if code&(1<<(4+row)) else 1
                        node['r']=rot
                elif kind==b'nGRP':
                    node['children']=[integer() for _ in range(integer())];refs.update(node['children'])
                else:
                    choices=[]
                    for _ in range(integer()):
                        model=integer();choices.append((model,dictionary()))
                    node['model']=min(choices,key=lambda v:int(v[1].get('_f',0)))
                if ident in nodes:raise ValueError('Duplicate node')
                nodes[ident]=node
        p+=n+children
    if nodes:
        occupied={};active=set()
        def visit(ident,r,t):
            if ident in active:raise ValueError('Scene cycle')
            active.add(ident);node=nodes[ident]
            if node['attrs'].get('_hidden')=='1' or node['layer'] in hidden_layers:
                active.remove(ident);return
            if node['kind']==b'nTRN':t,r=t+r@node['t'],r@node['r']
            if node['kind']==b'nSHP':
                index,attrs=node['model']
                if attrs.get('_hidden')!='1':
                    dims,voxels=models[index]
                    local=voxels[:,:3].astype(int)-np.array(dims)//2
                    positions=local@r.T+t+(r<0).sum(axis=1)*-1
                    for pos,c in zip(positions,voxels[:,3]):
                        if c:occupied[tuple(pos)]=int(c)
            for child in node['children']:visit(child,r,t)
            active.remove(ident)
        roots=sorted(set(nodes)-refs)
        if not roots:raise ValueError('No scene root')
        for root in roots:visit(root,np.eye(3,dtype=int),np.zeros(3,dtype=int))
        if not occupied:raise ValueError('Empty scene')
        positions=np.array(list(occupied));minimum=positions.min(axis=0);dims=positions.max(axis=0)-minimum+1
        if np.any(dims>256):raise ValueError('Scene exceeds engine 256 cell extent')
        grid=np.zeros(tuple(dims),dtype=np.uint8)
        for pos,c in occupied.items():grid[tuple(np.array(pos)-minimum)]=c
    else:
        if len(models)!=1:raise ValueError('Multiple unpositioned models')
        dims,voxels=models[0];grid=np.zeros(dims,dtype=np.uint8)
        for x,y,z,c in voxels:
            if c==0 or x>=dims[0] or y>=dims[1] or z>=dims[2]:raise ValueError('Invalid voxel')
            grid[x,y,z]=c
        if np.count_nonzero(grid)!=len(voxels):raise ValueError('Duplicate voxels')
    return grid,dict(chunks),hashlib.sha256(data).hexdigest()

def rectangles(mask,colors,by_color):
    # Mirrors C++ std::set<pair<int,int>> ordering: u first, then v.
    left={(int(u),int(v)) for u,v in np.argwhere(mask)}
    counts={'quads':0,'solid_quads':0,'mixed_quads':0,'unit_face_area':0,'max_quad_area':0}
    while left:
        u,v=min(left);c=colors[u,v];w=1
        while (u+w,v) in left and (not by_color or colors[u+w,v]==c):w+=1
        h=1
        while all((u+x,v+h) in left and (not by_color or colors[u+x,v+h]==c) for x in range(w)):h+=1
        values=colors[u:u+w,v:v+h];solid=bool(np.all(values==c))
        counts['quads']+=1;counts['solid_quads' if solid else 'mixed_quads']+=1
        area=w*h;counts['unit_face_area']+=area;counts['max_quad_area']=max(counts['max_quad_area'],area)
        for x in range(w):
            for y in range(h):left.remove((u+x,v+y))
    return counts

def run(grid):
    result=[];occupied=grid!=0
    # Raw VOX face order used in BuildGreedyMeshForFace: +Z,-Z,-X,+X,+Y,-Y.
    for name,axis,sign,uv in [('+Z',2,1,(0,1)),('-Z',2,-1,(0,1)),('-X',0,-1,(1,2)),('+X',0,1,(1,2)),('+Y',1,1,(0,2)),('-Y',1,-1,(0,2))]:
        neighbor=np.zeros_like(occupied)
        dst=[slice(None)]*3;src=dst.copy()
        dst[axis]=slice(0,-1) if sign>0 else slice(1,None)
        src[axis]=slice(1,None) if sign>0 else slice(0,-1)
        neighbor[tuple(dst)]=occupied[tuple(src)]
        exposed=occupied&~neighbor
        sums={mode:collections.Counter() for mode in ('palette_greedy','geometry_greedy')}
        for s in range(grid.shape[axis]):
            colors=np.take(grid,s,axis=axis);mask=np.take(exposed,s,axis=axis)
            for mode,by_color in [('palette_greedy',True),('geometry_greedy',False)]:
                if by_color:
                    pieces=[rectangles(mask&(colors==c),colors,True) for c in np.unique(colors[mask])]
                else:pieces=[rectangles(mask,colors,False)]
                for stats in pieces:
                    for k,value in stats.items():
                        if k=='max_quad_area':sums[mode][k]=max(sums[mode][k],value)
                        else:sums[mode][k]+=value
        item={'direction':name,'exposed_unit_faces':int(exposed.sum()),**{m:dict(v) for m,v in sums.items()}}
        for m in sums:assert item[m]['unit_face_area']==item['exposed_unit_faces']
        result.append(item)
    totals={}
    for mode in ('palette_greedy','geometry_greedy'):
        totals[mode]={k:(max(r[mode].get(k,0) for r in result) if k=='max_quad_area' else sum(r[mode].get(k,0) for r in result)) for k in ('quads','solid_quads','mixed_quads','unit_face_area','max_quad_area')}
        assert totals[mode]['quads']==totals[mode]['solid_quads']+totals[mode]['mixed_quads']
    return {'directions':result,'totals':totals}

def self_test():
    g=np.ones((4,4,1),dtype=np.uint8);g[::2,:,0]=2
    a=run(g)['totals'];assert a['geometry_greedy']['quads']==6 and a['geometry_greedy']['mixed_quads']==4
    g=np.ones((3,3,1),dtype=np.uint8);g[1,1,0]=0
    a=run(g)['totals'];assert a['geometry_greedy']['unit_face_area']==32
    g=np.ones((1,1,1),dtype=np.uint8);assert run(g)['totals']['geometry_greedy']['solid_quads']==6

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('vox',type=pathlib.Path);parser.add_argument('--output',type=pathlib.Path,required=True);args=parser.parse_args()
    self_test();start=time.perf_counter();grid,chunks,sha=read_vox(args.vox)
    report={'source':str(args.vox.resolve()),'sha256':sha,'dimensions':[int(v) for v in grid.shape],'occupied_voxels':int(np.count_nonzero(grid)),'used_palette_indices':int(len(np.unique(grid[grid!=0]))),'chunks':chunks,'classification':'palette index, not RGB equivalence; opaque occupancy; no material or emission boundary splitting','scene_scope':'static first frame, scene transforms/hidden layers/overlap assembly; not renderer runtime validation',**run(grid)}
    report['elapsed_seconds']=time.perf_counter()-start;report['self_tests']='passed: uniform cube, multicolor slab, hole coverage'
    args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_text(json.dumps(report,indent=2),encoding='utf-8');print(json.dumps(report,indent=2))
