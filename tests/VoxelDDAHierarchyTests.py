"""Compare hierarchical empty-space traversal with independent unit-cell DDA.

Checks exact palette IDs, face IDs and distance, including solid starts,
axis-parallel rays, ties, odd dimensions, holes, and finite ray segments.
CPU checks validate the traversal design; GPU captures validate production GLSL.
"""
import math, random, json

rng = random.Random(71931)

def trace(grid, dims, origin, direction, limit, camera, hierarchy):
    max_level = (max(dims)-1).bit_length()
    occupied = [{(x >> level, y >> level, z >> level) for x,y,z in grid}
                for level in range(max_level+1)]
    enter, leave, axis = -1e30, limit, 0
    for a in range(3):
        if abs(direction[a]) < 1e-20:
            if origin[a] < 0 or origin[a] >= dims[a]: return None
        else:
            pair = sorted((-origin[a]/direction[a], (dims[a]-origin[a])/direction[a]))
            if pair[0] > enter: enter, axis = pair[0], a
            leave = min(leave, pair[1])
    t = max(enter, .001)
    if leave < t or t >= limit: return None
    step = [(d > 0)-(d < 0) for d in direction]
    cell = [min(dims[a]-1, max(0, math.floor(origin[a]+t*direction[a]+step[a]*1e-4))) for a in range(3)]
    exiting = enter < .001 and tuple(cell) in grid
    previous, level = 0, max_level if hierarchy else 0
    next_t = [(cell[a]+max(step[a],0)-origin[a])/direction[a] if step[a] else 1e30 for a in range(3)]
    delta = [abs(1/d) if d else 1e30 for d in direction]
    for _ in range(sum(dims)+3):
        if t > leave or t >= limit: return None
        outside = any(cell[a] < 0 or cell[a] >= dims[a] for a in range(3))
        if not outside and hierarchy and not exiting:
            while level > 0 and tuple(c >> level for c in cell) in occupied[level]: level -= 1
            if level > 0:
                size = 1 << level
                block = [(c >> level) << level for c in cell]
                boundary = [(block[a]+(size if step[a]>0 else 0)-origin[a])/direction[a] if step[a] else 1e30 for a in range(3)]
                t = min(boundary); axis = boundary.index(t); old = cell[:]
                cell = [min(block[a]+size-1, max(block[a],math.floor(origin[a]+t*direction[a]))) for a in range(3)]
                for a in range(3):
                    if boundary[a] <= t: cell[a] = block[a]+size if step[a]>0 else block[a]-1
                    if step[a]: next_t[a] = (cell[a]+max(step[a],0)-origin[a])/direction[a]
                changed = (old[0]^cell[0]) | (old[1]^cell[1]) | (old[2]^cell[2])
                level = min(max_level, changed.bit_length()-1 if changed >= 0 else max_level)
                continue
        voxel = 0 if outside else grid.get(tuple(cell),0)
        if (not exiting and voxel) or (exiting and not voxel and previous):
            if camera and exiting:
                exiting = False; previous = 0; continue
            face = axis*2 + int(step[axis]>0 if exiting else step[axis]<0)
            return (round(t,9), previous if exiting else voxel, face)
        if outside: return None
        previous = voxel; t = min(next_t); axis = next_t.index(t); old = cell[:]
        for a in range(3):
            if next_t[a] <= t: cell[a] += step[a]; next_t[a] += delta[a]
        if hierarchy and not exiting:
            changed = (old[0]^cell[0]) | (old[1]^cell[1]) | (old[2]^cell[2])
            level = min(max_level, changed.bit_length()-1 if changed >= 0 else max_level)
    return None

cases = 0
for dims in ((1,1,1),(3,5,9),(16,8,4),(16,16,16),(32,16,8)):
    for kind in ('empty','solid','shell','sparse'):
        grid = {}
        for z in range(dims[2]):
            for y in range(dims[1]):
                for x in range(dims[0]):
                    if kind=='solid' or (kind=='shell' and (x in (0,dims[0]-1) or y in (0,dims[1]-1) or z in (0,dims[2]-1))) or (kind=='sparse' and rng.random()<.03):
                        grid[x,y,z] = 1 + (x+3*y+7*z)%254
        for i in range(200):
            origin = [rng.uniform(-3,dims[a]+3) for a in range(3)]
            if i%5 == 0: origin = [float(rng.randrange(d+1)) for d in dims]
            direction = [rng.uniform(-1,1) for _ in range(3)]
            if i%4 == 0: direction[i%3] = 0
            if i%7 == 0: direction = [1.0,-1.0,1.0]
            if i%11 == 0:
                direction = [0.0,0.0,0.0]; direction[i%3] = -1.0 if i%2 else 1.0
            if i%13 == 0: direction = [1e-24,0.0,1.0]
            length = math.sqrt(sum(d*d for d in direction))
            direction = [d/length for d in direction]
            # Grid-space directions need not be normalized under instance scale.
            if i%3 == 0: direction = [d*.25 for d in direction]
            for camera in (False,True):
                limit = rng.choice((.01,1.0,10.0,1000.0))
                a = trace(grid,dims,origin,direction,limit,camera,False)
                b = trace(grid,dims,origin,direction,limit,camera,True)
                assert a == b, (dims,kind,origin,direction,limit,camera,a,b)
                cases += 1
result = dict(cases=cases, seed=71931, result='PASS')
print(json.dumps(result))
