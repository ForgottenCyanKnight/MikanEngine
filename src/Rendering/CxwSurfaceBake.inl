// Adapter only: canonical exterior faces and occupancy queries go into the
// SAME BakeVoxSurfaces implementation used by VOX cooking.
struct CxwTile {
    // Relative sectors -1..2 on each axis; the central 2^3 form a 256^3 tile.
    // Only the central sectors and their six face-neighbor slabs are resident.
    std::array<std::shared_ptr<Sector>,64> sectors;
    uint16_t Cell(int x,int y,int z)const{
        const int sx=(x+128)>>7,sy=(y+128)>>7,sz=(z+128)>>7;
        if(sx<0||sx>3||sy<0||sy>3||sz<0||sz>3)return 0;
        const auto& s=sectors[sx+4*sy+16*sz];
        return s?s->Cell((x+128)&127,(y+128)&127,(z+128)&127):0;
    }
};
struct CxwCanonicalSource {
    std::vector<VoxRtQuad> quads;std::vector<uint32_t> materials;
    struct Metadata {
        struct Group {size_t firstIndex=0,indexCount=0;uint32_t faceDirection=0;};
        std::array<Group,6> faceGroups;
    } mesh;
    const auto& GetQuads()const{return quads;}
    const auto& GetQuadMaterials()const{return materials;}
    const auto& GetMeshData()const{return mesh;}
};
Mesh Build(const CxwTile& tile,bool mergeColors){
    struct Face {VoxRtQuad quad;uint32_t material;};
    std::array<std::vector<Face>,6> faces;
    Mesh mesh;mesh.extent=256;
    const int offsets[6][3]={{0,0,1},{0,0,-1},{-1,0,0},{1,0,0},{0,1,0},{0,-1,0}};
    for(int sz=1;sz<=2;++sz)for(int sy=1;sy<=2;++sy)for(int sx=1;sx<=2;++sx){
        const auto& sector=tile.sectors[sx+4*sy+16*sz];if(!sector)continue;
        mesh.bricks+=sector->bricks;++mesh.sourceSectors;
        for(int bi=0;bi<4096;++bi){const int id=sector->indices[bi];if(id<0)continue;
            const int bx=(sx-1)*128+(bi&15)*8,by=(sy-1)*128+((bi>>4)&15)*8,bz=(sz-1)*128+(bi>>8)*8;
            for(int vi=0;vi<512;++vi){const auto code=sector->blocks[size_t(id)*512+vi];if(!code)continue;++mesh.voxels;
                const int x=bx+(vi&7),y=by+((vi>>3)&7),z=bz+(vi>>6);const auto material=Material(code);
                for(int d=0;d<6;++d){const auto neighbor=tile.Cell(x+offsets[d][0],y+offsets[d][1],z+offsets[d][2]);
                    // Opaque surfaces remain visible through adjacent transparent cells.
                    if(neighbor&&(!(code&0xc000u)||(neighbor&0xc000u)))continue;
                    faces[d].push_back({{uint32_t(x)|(uint32_t(y)<<8)|(uint32_t(z)<<16),material[0]},material[1]});++mesh.exposedFaces;
                }
            }
        }
    }
    if(!mesh.exposedFaces)return mesh;
    CxwCanonicalSource source;source.quads.reserve(size_t(mesh.exposedFaces));source.materials.reserve(size_t(mesh.exposedFaces));
    for(uint32_t d=0;d<6;++d){auto& group=source.mesh.faceGroups[d];group.faceDirection=d;group.firstIndex=source.quads.size()*6;
        for(const auto& f:faces[d]){source.quads.push_back(f.quad);source.materials.push_back(f.material);}
        group.indexCount=faces[d].size()*6;std::vector<Face>().swap(faces[d]);
    }
    auto pair=[](int d,int plane,int u,int v){
        const bool positive=d==0||d==3||d==4;const int axis=d<2?2:(d<4?0:1),ua=d<2?0:(d<4?2:0),va=d<4?1:2;
        std::array<int,3> owner{};owner[axis]=plane-(positive?1:0);owner[ua]=u;owner[va]=v;
        auto neighbor=owner;neighbor[axis]+=positive?1:-1;return std::array{owner,neighbor};
    };
    VoxContactQuery contact=[&](int d,int p,int u,int v){const auto cells=pair(d,p,u,v);
        return (tile.Cell(cells[0][0],cells[0][1],cells[0][2])&0x8000u)&&(tile.Cell(cells[1][0],cells[1][1],cells[1][2])&0x8000u);};
    VoxContactQuery eligible=[&](int d,int p,int u,int v){const auto cells=pair(d,p,u,v);return (tile.Cell(cells[0][0],cells[0][1],cells[0][2])&0x8000u)!=0;};
    static const bool backfill=[](){const char* v=std::getenv("MIKAN_VOX_STATIC_CONTACT_BACKFILL");return !v||std::string(v)!="0";}();
    const bool optimize=mergeColors&&backfill;
    std::vector<VoxRtQuad> quads;std::vector<uint32_t> materials,attributes;std::vector<VoxRayTracingRange> ranges;
    VoxContactBackfillStats stats;
    BakeVoxSurfaces(source,mergeColors,quads,materials,ranges,attributes,
        optimize?contact:VoxContactQuery{},&stats,optimize?eligible:VoxContactQuery{},optimize);
    mesh.hiddenFaces=stats.emittedHidden;mesh.greedyQuads=stats.baselineQuads;
    for(uint32_t i=attributes[0];i<attributes[1];i+=2)mesh.palette.push_back({attributes[i],attributes[i+1]});
    mesh.descriptors.assign(attributes.begin()+attributes[1],attributes.begin()+attributes[2]);
    mesh.indexWords.assign(attributes.begin()+attributes[2],attributes.end());
    for(const auto& range:ranges)for(uint32_t i=range.firstQuad;i<range.firstQuad+range.quadCount;++i){
        const auto descriptor=mesh.descriptors[i];uint32_t id=descriptor&0x3fffffffu;
        if(descriptor&0x80000000u)id=(mesh.indexWords[id/4]>>((id%4)*8))&((descriptor&0x40000000u)?65535u:255u);
        mesh.quads.push_back({quads[i].geometry,quads[i].appearance,materials[i],id,range.direction});
    }
    return mesh;
}
