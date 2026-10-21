"""Brick-local DDA versus independent exposed-voxel face intersections.

CPU design check for tight bounds, empty coarse blocks, brick boundaries,
solid starts, camera back-face culling, axis-parallel rays and finite segments.
Non-degenerate rays require matching distance, palette and face. Exact edge
and corner grazing remains implementation-dependent and is not GPU validation.
"""
import math, random, json
rng=random.Random(814)
def reference(grid,dims,o,d,limit,camera,_):
 best=None
 for cell,voxel in grid.items():
  for axis in range(3):
   if abs(d[axis])<1e-20:continue
   for sign in (-1,1):
    neighbor=list(cell);neighbor[axis]+=sign
    if tuple(neighbor) in grid:continue
    if camera and d[axis]*sign>=0:continue
    t=(cell[axis]+int(sign>0)-o[axis])/d[axis]
    if t<.001 or t>=limit or (best and t>=best[0]):continue
    if all(cell[a]-1e-9<=o[a]+t*d[a]<=cell[a]+1+1e-9 for a in range(3) if a!=axis):best=(t,voxel,axis*2+int(sign>0))
 return best

def brick_ray(grid,dims,box,origin,direction,limit,camera):
 bo,minimum,maximum=box;o=origin;d=direction
 parallel=[abs(x)<1e-20 for x in d]
 if any(parallel[a] and (o[a]<minimum[a] or o[a]>=maximum[a]) for a in range(3)):return None
 inv=[1/(1e-20 if parallel[a] else d[a]) for a in range(3)]
 slab0=[(minimum[a]-o[a])*inv[a] for a in range(3)];slab1=[(maximum[a]-o[a])*inv[a] for a in range(3)]
 near=[-1e30 if parallel[a] else min(slab0[a],slab1[a]) for a in range(3)]
 far=[1e30 if parallel[a] else max(slab0[a],slab1[a]) for a in range(3)]
 enter=max(near);axis=near.index(enter);leave=min(limit,min(far))
 leave+=max(1e-5,abs(leave)*1e-6)
 t=max(enter,.001)
 if t>leave or t>=limit:return None
 step=[(x>0)-(x<0) for x in d]
 cell=[min(maximum[a]-1,max(minimum[a],math.floor(o[a]+t*d[a]+step[a]*1e-4))) for a in range(3)]
 entered=enter>=.001
 next_t=[(cell[a]+max(step[a],0)-o[a])*inv[a] if abs(d[a])>=1e-20 else 1e30 for a in range(3)]
 def inside(p):return all(bo[a]<=p[a]<bo[a]+edge for a in range(3))
 for _ in range(3*edge+2):
  if t>leave or t>=limit or not inside(cell):return None
  block=tuple((c>>2)<<2 for c in cell)
  occupied=any(tuple(block[a]+p[a] for a in range(3)) in grid for p in ((x,y,z) for z in range(4) for y in range(4) for x in range(4)))
  if edge==8 and not occupied:
   boundary=[(block[a]+(4 if step[a]>0 else 0)-o[a])*inv[a] if abs(d[a])>=1e-20 else 1e30 for a in range(3)]
   t=min(boundary);axis=boundary.index(t);entered=True
   cell=[min(block[a]+3,max(block[a],math.floor(o[a]+t*d[a]))) for a in range(3)]
   for a in range(3):
    if boundary[a]<=t:cell[a]=block[a]+4 if step[a]>0 else block[a]-1
    if abs(d[a])>=1e-20:next_t[a]=(cell[a]+max(step[a],0)-o[a])*inv[a]
   continue
  crossing=min(next_t);exit_axis=next_t.index(crossing);data=grid.get(tuple(cell),0)
  if data:
   face=axis*2+int(step[axis]<0)
   if entered and data&(1<<(8+face)):return (t,data&255,face)
   face=exit_axis*2+int(step[exit_axis]>0)
   if not camera and crossing<limit and crossing<=leave and data&(1<<(8+face)):return (crossing,data&255,face)
  axis=exit_axis;entered=True;t=crossing
  for a in range(3):
   if next_t[a]<=t:cell[a]+=step[a];next_t[a]+=abs(inv[a])
 return None
cases=0;edge_ties=0
for edge in (2,4,8):
 for dims in ((9,9,9),(17,8,5),(16,16,16),(32,17,9)):
  for kind in ('empty','solid','shell','sparse'):
   grid={(x,y,z):1+(x+3*y+7*z)%254 for z in range(dims[2]) for y in range(dims[1]) for x in range(dims[0]) if kind=='solid' or (kind=='shell' and any(p in (0,n-1) for p,n in zip((x,y,z),dims))) or (kind=='sparse' and rng.random()<.03)}
   surface={}
   for cell,voxel in grid.items():
    faces=0
    for a in range(3):
     for sign in (-1,1):
      n=list(cell);n[a]+=sign
      if tuple(n) not in grid:faces|=1<<(a*2+int(sign>0))
    if faces:surface[cell]=voxel|(faces<<8)
   bins={}
   for cell in surface:bins.setdefault(tuple(c//edge*edge for c in cell),[]).append(cell)
   boxes=[(bo,[min(c[a] for c in cells) for a in range(3)],[max(c[a] for c in cells)+1 for a in range(3)]) for bo,cells in bins.items()]
   for i in range(120):
    o=[rng.uniform(-3,n+3) for n in dims];d=[rng.uniform(-1,1) for _ in range(3)]
    if i%4==0:d=[0.,0.,0.];d[i%3]=1. if i%2 else -1.
    if i%9==0:o=[float(rng.randrange(n+1))+(a+1)*.1231 for a,n in enumerate(dims)];d=[1.,-1.,1.]
    if i%17==0:d=[1e-24,0.,1.]
    length=math.sqrt(sum(x*x for x in d));d=[x/length for x in d]
    if i%11==0:d=[x*.25 for x in d]
    for camera in (False,True):
     limit=rng.choice((1.,10.,1000.));expected=reference(grid,dims,o,d,limit,camera,False);best=None
     for box in boxes:
      hit=brick_ray(surface,dims,box,o,d,best[0] if best else limit,camera)
      if hit and (best is None or hit[0]<best[0]):best=hit
     assert (best is None)==(expected is None),(dims,kind,o,d,limit,camera,best,expected)
     if best:
      assert abs(best[0]-expected[0])<1e-7,(dims,kind,o,d,limit,camera,best,expected)
      if best[1:]!=expected[1:]:
       point=[o[a]+best[0]*d[a] for a in range(3)]
       assert sum(abs(x-round(x))<1e-7 for x in point)>=2,(dims,kind,o,d,limit,camera,best,expected)
       touching={v for c,v in grid.items() if all(c[a]-1e-7<=point[a]<=c[a]+1+1e-7 for a in range(3))}
       assert best[1] in touching and expected[1] in touching,(best,expected,point)
       edge_ties+=1
     cases+=1
result=dict(result='PASS',rays=cases,seed=814,valid_palette_or_normal_ties_at_cell_edges=edge_ties);print(json.dumps(result))
