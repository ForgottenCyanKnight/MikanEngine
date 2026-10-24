"""Geometry grouping must ignore appearance and preserve placement counts."""
import importlib.util
import pathlib
import struct
import tempfile
import unittest
import zlib

root=pathlib.Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('component_summary',root/'tools/summarize_vox_components.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)

def leaf(color=123,geometry=0):
    # A compact test leaf; grouping reads only its geometry sections.
    return [0x534d5856,5,0,1,1,6,0,0,0,1,1,1,1,0,0,0,0,0,1,0,0,geometry,4,6,7,0,color,0,0]

def write(path,leaves,indices):
    words=[0x534d5856,3,0,len(leaves),len(indices)]
    for mesh in leaves:words.extend([len(mesh),*mesh])
    for index in indices:words.extend([index]+[0]*12)
    words.append(0)
    raw=struct.pack('<'+'I'*len(words),*words);packed=zlib.compress(raw)
    path.write_bytes(struct.pack('<12I',0x5a4d5856,1,0,0,0,0,0,0,len(raw),0,len(packed),0)+packed)

class GroupingTests(unittest.TestCase):
    def test_appearance_does_not_split_blas(self):
        with tempfile.TemporaryDirectory() as folder:
            path=pathlib.Path(folder)/'asset.voxmesh';write(path,[leaf(1),leaf(2),leaf(3,1)],[0,0,1,2])
            meshes,uses=module.compiled_meshes(path)
            self.assertEqual(uses,[2,1,1])
            self.assertEqual(meshes[0]['geometry'],meshes[1]['geometry'])
            self.assertNotEqual(meshes[0]['geometry'],meshes[2]['geometry'])
    def test_truncated_leaf_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            path=pathlib.Path(folder)/'asset.voxmesh';write(path,[leaf()[:-1]],[0])
            with self.assertRaises(ValueError):module.compiled_meshes(path)
    def test_compressed_size_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            path=pathlib.Path(folder)/'asset.voxmesh';write(path,[leaf()],[0])
            path.write_bytes(path.read_bytes()+b'bad')
            with self.assertRaises(ValueError):module.compiled_meshes(path)

if __name__=='__main__':unittest.main()
