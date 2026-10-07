"""Generate R8 index PNG + RGBA8 palette + lossless source MATL table."""
import pathlib,struct,json,hashlib
import numpy as np
from PIL import Image
import vox_quad_stats as base
src=pathlib.Path('projects/vox/vox model/vox/rom.vox');root=src.parent/'rom_baked';out=root/'indexed'
if out.exists():raise RuntimeError('Do not overwrite')
m=json.loads((root/'rom_faces.json').read_text());g,_,sha=base.read_vox(src);assert sha==m['source_sha256'];w,h=m['size'];pad=m['padding'];indices=np.zeros((h,w),dtype=np.uint8);blocks={b['id']:b for b in m['blocks']};seen=set()
for f in m['faces']:
 b=blocks[f['block']]
 if b['id'] in seen:continue
 seen.add(b['id']);a=np.take(g,f['plane']-(f['sign']>0),axis=f['axis'])[f['u']:f['u']+f['width'],f['v']:f['v']+f['height']]
 if f['solid']:a=a[:1,:1]
 assert a.shape==(b['width'],b['height'])
 a=np.pad(a.T,((pad,pad),(pad,pad)),mode='edge');x=b['x']-pad;y=b['y']-pad;indices[y:y+a.shape[0],x:x+a.shape[1]]=a
raw=src.read_bytes();palette=np.zeros((256,4),dtype=np.uint8);materials={};p=20
while p<len(raw):
 kind,n,c=struct.unpack_from('<4sII',raw,p);p+=12;payload=raw[p:p+n]
 if kind==b'RGBA':palette[1:]=np.frombuffer(payload[:255*4],dtype=np.uint8).reshape(255,4)
 if kind==b'MATL':
  ident,count=struct.unpack_from('<ii',payload);cursor=8;d={}
  def string():
   global cursor
   size=struct.unpack_from('<i',payload,cursor)[0];cursor+=4;s=payload[cursor:cursor+size].decode();cursor+=size;return s
  for _ in range(count):k=string();d[k]=string()
  materials[ident]=d
 p+=n+c
out.mkdir();Image.fromarray(indices).save(out/'rom_indices_r8.png',optimize=True);Image.fromarray(palette.reshape(16,16,4)).save(out/'rom_palette_rgba8.png',optimize=True)
# Preserve all original MATL strings instead of guessing runtime PBR conversions.
(out/'rom_palette_materials.json').write_text(json.dumps({'format':'source VOX MATL, keyed by original palette ID; convert to engine PBR semantics on upload','materials':materials},indent=2),encoding='utf-8')
assert np.array_equal(palette[indices],np.array(Image.open(root/'rom_basecolor.png')))
assert np.array_equal(np.array(Image.open(out/'rom_indices_r8.png')),indices)
used=sorted(set(int(x) for x in indices.flat));mattypes=sorted(set(materials.get(i,{}).get('_type','_diffuse') for i in used if i))
def chain(w,h,bpp):
 total=0
 while True:
  total+=w*h*bpp
  if w==h==1:return total
  w=max(1,w//2);h=max(1,h//2)
old=(root/'rom_basecolor.png').stat().st_size;pngs=sum((out/n).stat().st_size for n in ('rom_indices_r8.png','rom_palette_rgba8.png'));rgba=w*h*4;r8=w*h;pal=256*4
# Same material representation on both sides: material-table entries are estimates,
# not generated upload-ready binaries. Colored emission = palette RGB * scalar.
report={'atlas_size':[w,h],'palette_entries':256,'used_nonzero_palette_ids':len([x for x in used if x]),'used_source_material_types':mattypes,'files':{p.name:p.stat().st_size for p in out.iterdir()},'color_only':{'before_png_bytes':old,'after_two_png_bytes':pngs,'disk_saved_bytes':old-pngs,'before_gpu_mip0_bytes':rgba,'after_gpu_mip0_bytes':r8+pal,'gpu_saved_percent':100*(1-(r8+pal)/rgba),'before_full_mip_chain_bytes':chain(w,h,4),'after_full_mip_chain_plus_palette_bytes':chain(w,h,1)+pal},'material_gpu_estimates':{'note':'Examples only; source MATL JSON retained losslessly. Same table cost applies to old RGBA and new R8 designs for fair comparison. More fields/types may require more storage.','rgba8_color_plus_16_byte_material_entry_table_bytes':256*(4+16),'before_rgba_atlas_plus_16_byte_material_table_bytes':rgba+256*16,'after_r8_plus_color_palette_plus_16_byte_material_table_bytes':r8+256*(4+16)},'single_r8_texture_option':{'note':'Byte-packed palette inside extra rows; color needs 4 R8 texels per entry; 16 byte material entry needs 16 more. Fetch and decode manually, no RGBA typed alias assumed.','extra_rows_color_only':int(np.ceil(pal/w)),'gpu_color_only_bytes':w*(h+int(np.ceil(pal/w))),'extra_rows_color_and_16byte_material':int(np.ceil(256*20/w)),'gpu_color_and_material_bytes':w*(h+int(np.ceil(256*20/w)))},'validation':'palette[index] byte-exact equals original RGBA atlas including gutters and background; index PNG roundtrip exact','sampling':'indices must use integer nearest/no ordinary averaging mip; palette RGB sRGB decoded after lookup, material fields linear. Existing UV mapping reused for two-texture version.','excluded':'GPU allocation alignment, geometry, descriptors, staging; MATL upload conversion and runtime validation not performed'}
(out/'report.json').write_text(json.dumps(report,indent=2),encoding='utf-8');manifest={p.name:{'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in out.iterdir()};(out/'manifest.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8');print(json.dumps(report,indent=2))
