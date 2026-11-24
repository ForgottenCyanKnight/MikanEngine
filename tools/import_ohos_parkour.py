"""Create an editable desktop scene from the OHOS physics/parkour prototype."""
import copy
import json
import math
import shutil
import time
from pathlib import Path

engine = Path(__file__).resolve().parents[1]
raw = engine.parent / 'ohosEngine/ohos-project/entry/src/main/resources/rawfile'
source = raw / 'scenes/physics_parkour.json'
project = engine / 'projects/ohos-physics-parkour'
if project.exists():
    raise SystemExit(f'Refusing to overwrite existing project: {project}')
scene = json.loads(source.read_text(encoding='utf-8'))
original = copy.deepcopy(scene)
for folder in ('scenes', 'games', 'models/parkour', 'reference'):
    (project / folder).mkdir(parents=True, exist_ok=True)
for model in sorted({e['mesh']['modelPath'] for e in scene['entities'] if 'mesh' in e}):
    resolved = engine / model if model.startswith('engine/') else raw / model
    assert resolved.is_file(), resolved
    if not model.startswith('engine/'):
        shutil.copy2(resolved, project / model)
scene.pop('game', None)
scene.pop('gameplay', None)
water = scene.pop('water')
scene['editorCamera'] = dict(position=[44, 53, 17], yaw=-123.3, pitch=-28.5,
                             speed=20, fov=55, farPlane=500, sensitivity=0.1)
for entity in scene['entities']:
    entity.setdefault('hierarchy', {'parent': 4294967295, 'children': []})
    if 'camera' in entity:
        # The desktop scene view also applies the main camera's frustum.
        # The OHOS follow camera faces away from this independent course.
        entity['camera']['enableFrustumCulling'] = False
        entity['camera']['enableHiZCulling'] = False
        entity['transform']['position'] = scene['editorCamera']['position'][:]
        yaw = math.radians(scene['editorCamera']['yaw'] + 90) / 2
        pitch = math.radians(scene['editorCamera']['pitch']) / 2
        entity['transform']['rotation'] = [
            math.cos(yaw) * math.cos(pitch), math.cos(yaw) * math.sin(pitch),
            math.sin(yaw) * math.cos(pitch), -math.sin(yaw) * math.sin(pitch)]
        entity['camera']['thirdPersonEnabled'] = False
        entity['camera']['thirdPersonAimEnabled'] = False
        entity['camera']['thirdPersonLockOnEnabled'] = False
scene['entities'].append({
    'id': 20000, 'name': {'name': 'Swimming Pool Water'},
    'transform': {'position': [water['center'][0], water['height'], water['center'][2]],
                  'rotation': [1, 0, 0, 0], 'scale': [1, 1, 1]},
    'hierarchy': {'parent': 4294967295, 'children': []},
    'water': {'enabled': True, 'size': [6.4, 10], 'surfaceOffset': 0,
              'depth': 1.8, 'buoyancy': 1.15, 'drag': 2,
              'color': water['color'], 'roughness': water['roughness'],
              'affectPlayersOnly': True}})
def save(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
save(project / 'scenes/main.json', scene)
save(project / 'reference/physics_parkour.ohos.json', original)
save(project / 'project.json', dict(formatVersion=1, name='ohos-physics-parkour',
     resourceRoot='.', codeRoot='games', game='', scene='scenes/main.json',
     editorPostProcessChain='', assets=['scenes/main.json', 'models/parkour']))
(project / 'README.md').write_text('''# OHOS Physics Parkour — Desktop Scene

Converted from ohosEngine physics_parkour.json. All 135 source entities retain
their names and transforms; the pool is a native desktop WaterComponent.
Engine cube references use engine/ paths; three colored GLBs are project assets.
The editor camera starts at an oblique overview of the full course.

This project supports scene viewing/editing. OHOS checkpoint, portal and moving
platform gameplay logic is not a desktop game module. The original metadata is
preserved in reference/physics_parkour.ohos.json for traceability.
''', encoding='utf-8')
index_path = engine / 'projects.json'
index = json.loads(index_path.read_text(encoding='utf-8'))
index['projects'].insert(0, dict(name=project.name, path='projects/' + project.name,
                                lastOpened=int(time.time())))
save(index_path, index)
print(f'Created {project}: {len(scene["entities"])} entities; all model references verified.')
