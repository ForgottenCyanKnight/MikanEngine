#include "vox_rt_color.glsl"
// Buffer-reference uvec2 addresses avoid requiring the optional shaderInt64 feature.
layout(buffer_reference,std430,buffer_reference_align=4) readonly buffer ModelWords {uint words[];};
struct ModelGeometry {uvec4 addresses;uvec4 counts;vec4 emission;};
layout(std430,binding=8) readonly buffer Models {ModelGeometry modelGeometries[];};
// Slot count must match kModelTextureSlots (RayTracingViewport.h) and the
// binding 9/21/38 descriptor counts. Constant-index switch keeps this
// independent of sampled-image nonuniform indexing features.
#define MODEL_TEXTURE_SLOTS 256
#define MODEL_SLOT_LIST(X) X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15) X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31) X(32) X(33) X(34) X(35) X(36) X(37) X(38) X(39) X(40) X(41) X(42) X(43) X(44) X(45) X(46) X(47) X(48) X(49) X(50) X(51) X(52) X(53) X(54) X(55) X(56) X(57) X(58) X(59) X(60) X(61) X(62) X(63) X(64) X(65) X(66) X(67) X(68) X(69) X(70) X(71) X(72) X(73) X(74) X(75) X(76) X(77) X(78) X(79) X(80) X(81) X(82) X(83) X(84) X(85) X(86) X(87) X(88) X(89) X(90) X(91) X(92) X(93) X(94) X(95) X(96) X(97) X(98) X(99) X(100) X(101) X(102) X(103) X(104) X(105) X(106) X(107) X(108) X(109) X(110) X(111) X(112) X(113) X(114) X(115) X(116) X(117) X(118) X(119) X(120) X(121) X(122) X(123) X(124) X(125) X(126) X(127) X(128) X(129) X(130) X(131) X(132) X(133) X(134) X(135) X(136) X(137) X(138) X(139) X(140) X(141) X(142) X(143) X(144) X(145) X(146) X(147) X(148) X(149) X(150) X(151) X(152) X(153) X(154) X(155) X(156) X(157) X(158) X(159) X(160) X(161) X(162) X(163) X(164) X(165) X(166) X(167) X(168) X(169) X(170) X(171) X(172) X(173) X(174) X(175) X(176) X(177) X(178) X(179) X(180) X(181) X(182) X(183) X(184) X(185) X(186) X(187) X(188) X(189) X(190) X(191) X(192) X(193) X(194) X(195) X(196) X(197) X(198) X(199) X(200) X(201) X(202) X(203) X(204) X(205) X(206) X(207) X(208) X(209) X(210) X(211) X(212) X(213) X(214) X(215) X(216) X(217) X(218) X(219) X(220) X(221) X(222) X(223) X(224) X(225) X(226) X(227) X(228) X(229) X(230) X(231) X(232) X(233) X(234) X(235) X(236) X(237) X(238) X(239) X(240) X(241) X(242) X(243) X(244) X(245) X(246) X(247) X(248) X(249) X(250) X(251) X(252) X(253) X(254) X(255)
layout(binding=9) uniform sampler2D modelAlbedo[MODEL_TEXTURE_SLOTS];
// glTF metallicRoughness textures (g=roughness, b=metallic), same
// constant-index scheme as albedo. Slot travels in instance.material.z.
layout(binding=21) uniform sampler2D modelMetallicRoughness[MODEL_TEXTURE_SLOTS];
layout(binding=38) uniform sampler2D modelEmissive[MODEL_TEXTURE_SLOTS];
vec3 sampleModelEmission(uint slot,vec2 uv){
    switch(slot){
#define EMI_CASE(i) case i: return textureLod(modelEmissive[i],uv,0).rgb;
        MODEL_SLOT_LIST(EMI_CASE)
#undef EMI_CASE
        default:return vec3(0);
    }
}

vec2 sampleModelMR(uint slot,vec2 uv){
    switch(slot){
#define MR_CASE(i) case i: return textureLod(modelMetallicRoughness[i],uv,0).gb;
        MODEL_SLOT_LIST(MR_CASE)
#undef MR_CASE
        default:return vec2(1.0,0.0);
    }
}

vec3 sampleModelAlbedo(uint slot,vec2 uv){
    switch(slot){
#define ALB_CASE(i) case i: return textureLod(modelAlbedo[i],uv,0).rgb;
        MODEL_SLOT_LIST(ALB_CASE)
#undef ALB_CASE
        default:return vec3(1);
    }
}
vec3 baseColorToLinear(vec3 color){
    return rtSrgbToLinear(color);
}
void modelVertex(ModelWords vertices,uint index,out vec3 position,out vec3 normal,out vec2 uv){
    uint base=index*8u;
    position=uintBitsToFloat(uvec3(vertices.words[base],vertices.words[base+1],vertices.words[base+2]));
    normal=unpackSnorm4x8(vertices.words[base+3]).xyz;
    uv=unpackHalf2x16(vertices.words[base+4]);
}
vec3 modelEmission(uint geometryIndex,uint primitive,vec2 barycentric,uint textureInfo){
    ModelGeometry geometry=modelGeometries[geometryIndex];
    vec3 emission=geometry.emission.rgb;
    if(textureInfo==0xffffffffu)return emission;
    ModelWords vertices=ModelWords(geometry.addresses.xy),indices=ModelWords(geometry.addresses.zw);
    uint first=primitive*3u;
    vec3 p,n;vec2 uv0,uv1,uv2;
    modelVertex(vertices,indices.words[first],p,n,uv0);
    modelVertex(vertices,indices.words[first+1u],p,n,uv1);
    modelVertex(vertices,indices.words[first+2u],p,n,uv2);
    vec2 uv=uv0*(1.0-barycentric.x-barycentric.y)+uv1*barycentric.x+uv2*barycentric.y;
    vec3 texel=sampleModelEmission(textureInfo&255u,uv);
    if((textureInfo&256u)!=0u)texel=rtSrgbToLinear(texel);
    return emission*texel;
}
void modelSurface(uint geometryIndex,uint primitive,vec2 barycentric,uint textureInfo,uint mrInfo,
    float metalFallback,float roughFallback,
    out vec3 color,out vec3 normal,out vec3 geometricNormal,out float metallic,out float roughness){
    ModelGeometry geometry=modelGeometries[geometryIndex];
    ModelWords vertices=ModelWords(geometry.addresses.xy),indices=ModelWords(geometry.addresses.zw);
    uint first=primitive*3u;
    vec3 p0,p1,p2,n0,n1,n2;vec2 uv0,uv1,uv2;
    modelVertex(vertices,indices.words[first],p0,n0,uv0);
    modelVertex(vertices,indices.words[first+1],p1,n1,uv1);
    modelVertex(vertices,indices.words[first+2],p2,n2,uv2);
    vec3 weights=vec3(1-barycentric.x-barycentric.y,barycentric);
    geometricNormal=normalize(cross(p1-p0,p2-p0));
    normal=n0*weights.x+n1*weights.y+n2*weights.z;
    normal=dot(normal,normal)>1e-8?normalize(normal):geometricNormal;
    color=vec3(1);
    vec2 uv=uv0*weights.x+uv1*weights.y+uv2*weights.z;
    metallic=metalFallback;roughness=roughFallback;
    if(textureInfo!=0xffffffffu){
        color=sampleModelAlbedo(textureInfo&255u,uv);
        if((textureInfo&256u)!=0u)color=baseColorToLinear(color);
    }
    if(mrInfo!=0xffffffffu){
        // Same combined image the raster pipeline binds to roughness/metallic slots.
        vec2 mr=sampleModelMR(mrInfo&255u,uv);
        roughness=clamp(mr.x,0.0,1.0);
        metallic=clamp(mr.y,0.0,1.0);
    }
}
