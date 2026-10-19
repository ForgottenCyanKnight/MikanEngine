"""Reuse production tone mapping functions in the headless compute exporter."""
from pathlib import Path
import sys
color_path = Path(sys.argv[1]).parent / 'vox_rt_color.glsl'
# Same functions and operation order as hardware_rt_present.frag, not the raster tone mapper.
functions = color_path.read_text() + "\nvec3 tonemapLinear(vec3 hdr){return rtLinearToSrgb(rtDisplayMap(max(hdr,vec3(0))));}\n"
shader = '''#version 450
#define TONEMAP_MODE 1
layout(local_size_x=8,local_size_y=8) in;
layout(rgba16f,binding=0) uniform readonly image2D hdrImage;
layout(std430,binding=1) buffer Sum {vec4 sum[];};
layout(std430,binding=2) buffer Pixels {uint pixels[];};
layout(rgba16f,binding=3) uniform readonly image2D diffuseImage;
layout(rgba16f,binding=4) uniform readonly image2D materialImage;
layout(std430,binding=5) buffer GISum {vec4 giSum[];};
layout(std430,binding=6) buffer GIPixels {uint giPixels[];};
layout(std430,binding=7) buffer DirectPixels {uint directPixels[];};
layout(push_constant) uniform Settings {uvec4 extent;} pc;
''' + functions + '''
void main(){
 uvec2 p=gl_GlobalInvocationID.xy;if(any(greaterThanEqual(p,pc.extent.xy)))return;
 uint i=p.y*pc.extent.x+p.x;sum[i]+=imageLoad(hdrImage,ivec2(p));
 vec4 material=imageLoad(materialImage,ivec2(p));
 giSum[i]+=vec4(material.a>.5?imageLoad(diffuseImage,ivec2(p)).rgb*material.rgb:vec3(0),material.a>.5?1.:0.);
 if(pc.extent.w==0u)return;
 vec3 c=tonemapLinear(sum[i].rgb/float(pc.extent.z));
 c=clamp(c,0.,1.);
 pixels[i]=packUnorm4x8(vec4(c,1));
 giPixels[i]=packUnorm4x8(vec4(tonemapLinear(giSum[i].rgb/float(pc.extent.z)),1));
 directPixels[i]=packUnorm4x8(vec4(tonemapLinear(max(sum[i].rgb-giSum[i].rgb,vec3(0))/float(pc.extent.z)),1));
}
'''
Path(sys.argv[2]).write_text(shader)

# Single-frame exporter: deliberately performs no external frame accumulation.
Path(sys.argv[2]).with_name('preview_single.comp').write_text("""#version 450
#define TONEMAP_MODE 1
layout(local_size_x=8,local_size_y=8) in;
layout(rgba16f,binding=0) uniform readonly image2D hdrImage;
layout(std430,binding=1) buffer Pixels {uint pixels[];};
""" + functions + """
void main(){ivec2 p=ivec2(gl_GlobalInvocationID.xy);ivec2 size=imageSize(hdrImage);
 if(any(greaterThanEqual(p,size)))return;
 vec3 c=tonemapLinear(imageLoad(hdrImage,p).rgb);
 c=clamp(c,0.,1.);
 pixels[p.y*size.x+p.x]=packUnorm4x8(vec4(c,1));}
""")
Path(sys.argv[2]).with_name('preview_copy.comp').write_text("""#version 450
layout(local_size_x=8,local_size_y=8) in;
layout(rgba16f,binding=0) uniform readonly image2D srcImage;
layout(rgba16f,binding=1) uniform writeonly image2D dstImage;
void main(){ivec2 p=ivec2(gl_GlobalInvocationID.xy);if(any(greaterThanEqual(p,imageSize(dstImage))))return;imageStore(dstImage,p,imageLoad(srcImage,p));}
""")
