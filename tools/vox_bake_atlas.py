"""Bake exact RGBA8 surface atlas; integer-nearest, mip0, two pixel gutters."""
import pathlib,json,struct,hashlib
import numpy as np
from PIL import Image
import vox_quad_stats as base

def pack(sizes,width):
    shelves=[];positions={};height=0
    for i,(w,h) in sorted(enumerate(sizes),key=lambda x:(-x[1][1],-x[1][0])):
        if w>width:return None
        candidates=[(sh-h, width-x-w,j) for j,(y,sh,x) in enumerate(shelves) if sh>=h and x+w<=width]
        if candidates:
            _,_,j=min(candidates);y,sh,x=shelves[j];positions[i]=(x,y);shelves[j]=(y,sh,x+w)
        else:positions[i]=(0,height);shelves.append((height,h,w));height+=h
    return height,positions

source=pathlib.Path('projects/vox/vox model/vox/rom.vox');out=source.parent/'rom_baked'
if out.exists():raise RuntimeError('Output exists; will not overwrite')
g,chunks,sha=base.read_vox(source);data=source.read_bytes();p=20;palette=None
while p<len(data):
    k,n,c=struct.unpack_from('<4sII',data,p);p+=12
    if k==b'RGBA':
        palette=np.zeros((256,4),dtype=np.uint8);palette[1:]=np.frombuffer(data[p:p+255*4],dtype=np.uint8).reshape(255,4)
    p+=n+c
if palette is None:raise ValueError('RGBA chunk required')
occ=g!=0;blocks=[];keys={};faces=[];padding=2
for axis,sign in [(2,1),(2,-1),(0,-1),(0,1),(1,1),(1,-1)]:
    neighbor=np.zeros_like(occ);dst=[slice(None)]*3;src=dst.copy();dst[axis]=slice(0,-1) if sign>0 else slice(1,None);src[axis]=slice(1,None) if sign>0 else slice(0,-1);neighbor[tuple(dst)]=occ[tuple(src)];exposed=occ&~neighbor
    for plane in range(g.shape[axis]):
        colors=np.take(g,plane,axis=axis);mask=np.take(exposed,plane,axis=axis);left={tuple(map(int,v)) for v in np.argwhere(mask)}
        while left:
            u,v=min(left);w=1
            while (u+w,v) in left:w+=1
            h=1
            while all((u+x,v+h) in left for x in range(w)):h+=1
            patch=colors[u:u+w,v:v+h].copy();solid=bool(np.all(patch==patch[0,0]));stored=patch[:1,:1] if solid else patch
            key=(stored.shape,stored.tobytes())
            if key not in keys:keys[key]=len(blocks);blocks.append(stored)
            faces.append({'axis':axis,'sign':sign,'plane':plane+(sign>0),'u':u,'v':v,'width':w,'height':h,'solid':solid,'block':keys[key]})
            for x in range(w):
                for y in range(h):left.remove((u+x,v+y))
sizes=[(a.shape[0]+padding*2,a.shape[1]+padding*2) for a in blocks]
candidates=[]
for width in range(max(w for w,h in sizes),1025):
    height,pos=pack(sizes,width)
    candidates.append((width*height,max(width,height),width,height,pos))
_,_,width,height,pos=min(candidates,key=lambda x:x[:2]);atlas=np.zeros((height,width,4),dtype=np.uint8)
metadata=[]
for i,a in enumerate(blocks):
    x,y=pos[i];w,h=a.shape;rgba=palette[a.T];padded=np.pad(rgba,((padding,padding),(padding,padding),(0,0)),mode='edge');atlas[y:y+h+2*padding,x:x+w+2*padding]=padded
    metadata.append({'id':i,'x':x+padding,'y':y+padding,'width':w,'height':h})
for face in faces:
    b=metadata[face['block']];face['uv_edges_top_left']=[b['x']/width,b['y']/height,(b['x']+b['width'])/width,(b['y']+b['height'])/height]
    if face['solid']:face['uv_center']=[(b['x']+.5)/width,(b['y']+.5)/height]
# Round-trip every face from packed RGBA data, including solid broadcasts.
for f in faces:
    b=metadata[f['block']];decoded=atlas[b['y']:b['y']+b['height'],b['x']:b['x']+b['width']].transpose(1,0,2)
    expected=np.take(g,f['plane']-(f['sign']>0),axis=f['axis'])[f['u']:f['u']+f['width'],f['v']:f['v']+f['height']]
    assert np.array_equal(np.broadcast_to(decoded,(*expected.shape,4)),palette[expected])
expected=base.run(g)['totals']['geometry_greedy'];assert len(faces)==expected['quads']
out.mkdir();png=out/'rom_basecolor.png';Image.fromarray(atlas).save(png,optimize=True)
assert np.array_equal(np.array(Image.open(png)),atlas)
mapping={'source_sha256':sha,'grid_dimensions':[int(v) for v in g.shape],'coordinates':'assembled raw VOX axes; UV image origin top-left; convert V for glTF as required by exporter','atlas':png.name,'size':[width,height],'padding':padding,'sampling':'nearest; no mipmaps; clamp; texture base color interpreted sRGB','blocks':metadata,'faces':faces}
mp=out/'rom_faces.json';mp.write_text(json.dumps(mapping,indent=2),encoding='utf-8')
mips=[];w,h=width,height
while True:
    mips.append(w*h*4)
    if w==h==1:break
    w=max(1,w//2);h=max(1,h//2)
report={'source':str(source.resolve()),'source_sha256':sha,'quads':len(faces),'solid_quads':sum(f['solid'] for f in faces),'mixed_quads':sum(not f['solid'] for f in faces),'unique_blocks':len(blocks),'unique_solid_blocks':sum(a.size==1 for a in blocks),'unique_mixed_blocks':sum(a.size>1 for a in blocks),'atlas_size':[width,height],'format':'RGBA8888 sRGB base color','png_bytes':png.stat().st_size,'mapping_json_bytes':mp.stat().st_size,'gpu_mip0_bytes':width*height*4,'hypothetical_full_mip_chain_bytes':sum(mips),'mip_levels':len(mips),'payload_texels':sum(a.size for a in blocks),'padded_texels':sum(w*h for w,h in sizes),'allocation_note':'logical texture texel bytes only; GPU tiling/allocation alignment, staging, geometry, descriptors, material maps excluded; full mip chain estimated only, gutters do not guarantee coarse mip isolation','validation':'all 511 faces roundtrip exactly to source palette RGBA; saved PNG decoded byte-exact; greedy count parity passed','material_note':'base color and source palette alpha only; MATL PBR/emission not baked; no light/shadow baking'}
(out/'report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
manifest={p.name:{'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in out.iterdir() if p.is_file()};(out/'manifest.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
print(json.dumps(report,indent=2));print('OUTPUT',out.resolve())
