#include "assimp/Importer.hpp"
#include "assimp/scene.h"
#include "assimp/postprocess.h"
#include "assimp/material.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <filesystem>
int main(int argc,char** argv){if(argc<3)return 1;const char*in=argv[1],*out=argv[2];Assimp::Importer imp;const aiScene*s=imp.ReadFile(in,aiProcess_Triangulate|aiProcess_GenNormals|aiProcess_FlipUVs|aiProcess_CalcTangentSpace|aiProcess_LimitBoneWeights);FILE*f=fopen(out,"w");if(!f)return 1; if(!s||!s->mRootNode){fprintf(f,"LOAD FAILED\n");fclose(f);return 0;}fprintf(f,"materials=%u submeshes=%u\n",s->mNumMaterials,s->mNumMeshes); for(unsigned i=0;i<s->mNumMaterials;i++){aiMaterial*m=s->mMaterials[i];aiString nm;std::string name=(m->Get(AI_MATKEY_NAME,nm)==AI_SUCCESS)?nm.C_Str():"";int twosided=0;float opacity=1.0f;m->Get<int>(AI_MATKEY_TWOSIDED,twosided);if(m->Get<float>(AI_MATKEY_OPACITY,opacity)!=AI_SUCCESS)opacity=1.0f;fprintf(f,"mat[%u] name=\"%s\" twosided=%d opacity=%.3f\n",i,name.c_str(),twosided,opacity);} for(unsigned i=0;i<s->mNumMeshes;i++){aiMesh*mm=s->mMeshes[i];fprintf(f,"mesh[%u] name=\"%s\" mat=%u\n",i,mm->mName.C_Str(),mm->mMaterialIndex);}fclose(f);return 0;}
