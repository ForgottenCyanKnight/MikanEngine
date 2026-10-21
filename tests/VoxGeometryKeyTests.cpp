#include "Rendering/RayTracing/VoxGeometryKey.h"
#include <cassert>
#include <iostream>
int main(){
 const std::vector<uint32_t> q{1,2,3},p{0,5},r{0,3,2};
 const std::array<float,4> bounds{0,1,2,1};
 const auto a=mikan::rt::MakeVoxGeometryKey(q,p,r,bounds);
 assert(a==mikan::rt::MakeVoxGeometryKey(q,p,r,bounds));
 auto q2=q;q2[0]++;auto b=mikan::rt::MakeVoxGeometryKey(q2,p,r,bounds);
 b.hash=a.hash;assert(!(a==b));
 auto p2=p;p2[1]++;assert(!(a==mikan::rt::MakeVoxGeometryKey(q,p2,r,bounds)));
 auto r2=r;r2[2]++;assert(!(a==mikan::rt::MakeVoxGeometryKey(q,p,r2,bounds)));
 auto v=bounds;v[3]=2;assert(!(a==mikan::rt::MakeVoxGeometryKey(q,p,r,v)));
 assert(!(mikan::rt::MakeVoxGeometryKey(q,p,{},bounds)==mikan::rt::MakeVoxGeometryKey({},q,p,bounds)));
 std::cout<<"Geometry key tests passed\n";
}
