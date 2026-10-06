// Same panorama projection, linear LogLuv decode and SH basis as fullscreen.frag.
layout(binding=5) uniform sampler2D skyPanorama;
layout(std140,binding=6) uniform SkyIrradiance {vec4 coefficients[9];} skySH;
layout(std140,binding=7) uniform SkySettings {vec4 tintIntensity;} skySettings;
const float RT_PI=3.14159265358979323846;

vec3 skyModulation(){return max(skySettings.tintIntensity.rgb,vec3(0))*max(skySettings.tintIntensity.a,0);}
vec2 skyPanoramaUV(vec3 direction){
    float altitude=max(pc.eye.w,0),radius=6371.0+altitude*.001;
    float horizon=acos(clamp(sqrt(max(radius*radius-6371.0*6371.0,0))/radius,0,1));
    float angle=horizon-acos(clamp(direction.y,-1,1));
    vec2 uv=vec2(atan(-direction.x,-direction.z)/(2*RT_PI)+.5,.5+angle/RT_PI);
    float s=6371.0/radius,t=clamp(.5-.5*sqrt(max(1-s*s,0)),0,1);
    bool below=uv.y<t;
    float x=clamp(below?(uv.y-t)/min(-t,-1e-6):(uv.y-t)/max(1-t,1e-6),0,1);
    float exponent=mix(4,1,clamp(altitude/60000,0,1));x=pow(x,1/exponent);
    uv.y=below?t-x*t:t+x*(1-t);
    return uv*.996+.002;
}
vec3 skyBackground(vec3 direction){
    if(pc.extent.z<.5)return vec3(.045,.065,.09);
    vec4 encoded=textureLod(skyPanorama,skyPanoramaUV(direction),0);
    if(all(lessThanEqual(encoded,vec4(0))))return vec3(0);
    float luminance=exp2((encoded.z*255+encoded.w-127)*.5);
    float sum=luminance/max(encoded.y,1e-6);
    const mat3 inverseLogLuv=mat3(6.0014,-2.7008,-1.7996,-1.3320,3.1029,-5.7721,.3008,-1.0882,5.6268);
    return max(inverseLogLuv*vec3(encoded.x*sum,luminance,sum),vec3(0))*skyModulation();
}
vec3 skyIrradiance(vec3 normal){
    if(pc.extent.z<.5)return vec3(0);
    float x=normal.x,y=normal.y,z=normal.z;
    vec3 irradiance=skySH.coefficients[0].rgb*.282095;
    irradiance+=skySH.coefficients[1].rgb*(-.488603*y);
    irradiance+=skySH.coefficients[2].rgb*(.488603*z);
    irradiance+=skySH.coefficients[3].rgb*(-.488603*x);
    irradiance+=skySH.coefficients[4].rgb*(1.092548*x*y);
    irradiance+=skySH.coefficients[5].rgb*(1.092548*y*z);
    irradiance+=skySH.coefficients[6].rgb*(.315392*(3*z*z-1));
    irradiance+=skySH.coefficients[7].rgb*(1.092548*x*z);
    irradiance+=skySH.coefficients[8].rgb*(.546274*(x*x-y*y));
    return max(irradiance,vec3(0))*skyModulation();
}
