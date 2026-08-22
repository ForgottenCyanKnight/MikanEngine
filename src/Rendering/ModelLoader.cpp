#include "ModelLoader.h"
#include "Core/ProjectManager.h"
#include "Rendering/JsonLite.h"


#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/material.h>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <map>
#include <unordered_map>
#include <functional>
#include <algorithm>
#include <filesystem>
#include <cstddef>
#include <cstdlib>
#include <glm/gtc/matrix_transform.hpp>
#include <SDL3/SDL.h>
#include <SDL3/SDL_iostream.h>
#include <vector>

std::unordered_map<std::string, ModelLoadResult> ModelLoader::s_ModelCache;
std::mutex ModelLoader::s_CacheMutex;

#ifdef __ANDROID__
AndroidIOStream::AndroidIOStream(const std::string& path) : m_IO(nullptr), m_Size(0) {
    // Android 平台：移除 assets/ 前缀
    std::string androidPath = path;
    
    // 移除 assets/ 前缀（SDL3 在 Android 上会自动从 assets 目录读取）
    if (androidPath.find("assets/") == 0) {
        androidPath = androidPath.substr(7); // 移除 "assets/" 前缀
    }
    
    // 统一使用正斜杠
    std::replace(androidPath.begin(), androidPath.end(), '\\', '/');
    
    m_IO = SDL_IOFromFile(androidPath.c_str(), "rb");
    if (m_IO) {
        m_Size = (size_t)SDL_GetIOSize(m_IO);
    }
}

AndroidIOStream::~AndroidIOStream() {
    if (m_IO) {
        SDL_CloseIO(m_IO);
    }
}

size_t AndroidIOStream::Read(void* pvBuffer, size_t pSize, size_t pCount) {
    // Assimp may issue a zero-sized read while probing a container.  Do not
    // divide by pSize in that case; the old implementation could crash only
    // for assets whose GLB layout exercised that probe path.
    if (!m_IO || !pvBuffer || pSize == 0 || pCount == 0) return 0;
    return SDL_ReadIO(m_IO, pvBuffer, pSize * pCount) / pSize;
}

size_t AndroidIOStream::Write(const void* pvBuffer, size_t pSize, size_t pCount) {
    return 0;
}

aiReturn AndroidIOStream::Seek(size_t pOffset, aiOrigin pOrigin) {
    if (!m_IO) return aiReturn_FAILURE;
    SDL_IOWhence whence = SDL_IO_SEEK_SET;
    switch (pOrigin) {
        case aiOrigin_SET: whence = SDL_IO_SEEK_SET; break;
        case aiOrigin_CUR: whence = SDL_IO_SEEK_CUR; break;
        case aiOrigin_END: whence = SDL_IO_SEEK_END; break;
    }
    Sint64 result = SDL_SeekIO(m_IO, (Sint64)pOffset, whence);
    return result >= 0 ? aiReturn_SUCCESS : aiReturn_FAILURE;
}

size_t AndroidIOStream::Tell() const {
    if (!m_IO) return 0;
    return (size_t)SDL_TellIO(m_IO);
}

size_t AndroidIOStream::FileSize() const {
    return m_Size;
}

void AndroidIOStream::Flush() {
}

bool AndroidIOSystem::Exists(const char* pFile) const {
    SDL_IOStream* io = SDL_IOFromFile(pFile, "rb");
    if (io) {
        SDL_CloseIO(io);
        return true;
    }
    return false;
}

char AndroidIOSystem::getOsSeparator() const {
    return '/';
}

Assimp::IOStream* AndroidIOSystem::Open(const char* pFile, const char* pMode) {
    AndroidIOStream* stream = new AndroidIOStream(pFile);
    if (stream->FileSize() > 0) {
        return stream;
    }
    delete stream;
    return nullptr;
}

void AndroidIOSystem::Close(Assimp::IOStream* pFile) {
    delete pFile;
}
#endif

void ModelLoader::ClearCache()
{
    std::lock_guard<std::mutex> lock(s_CacheMutex);
    s_ModelCache.clear();
}

MeshData ModelLoader::LoadModel(const std::string& path) {
    return LoadModelWithTextures(path).meshData;
}

ModelLoadResult ModelLoader::LoadModelWithTextures(const std::string& path) {
    // 首先检查缓存（加锁）
    {
        std::lock_guard<std::mutex> lock(s_CacheMutex);
        auto it = s_ModelCache.find(path);
        if (it != s_ModelCache.end()) {
            std::cout << "[ModelLoader] Loading from cache: " << path << std::endl;
            return it->second;
        }
    }

    ModelLoadResult result;

    Assimp::Importer importer;

#ifdef __ANDROID__
    // Android: 使用自定义 IOSystem 从 assets 加载
    importer.SetIOHandler(new AndroidIOSystem());
#endif

    const aiScene* scene = nullptr;
    std::string loadedPath = path;   // 实际加载成功的路径（用于计算 modelDir）
    
    // 尝试多个路径来解决工作目录问题
    std::vector<std::string> pathsToTry;
    // 优先使用 ProjectManager 绝对化路径（按引擎根定位）：assimp 解析 mtl/纹理时基于绝对路径，不依赖进程工作目录
    std::string absPath = ProjectManager::GetInstance().ResolveAssetPath(path);
    pathsToTry.push_back(absPath);
    if (absPath != path) {
        pathsToTry.push_back(path);                           // 原始路径
    }
    
#ifndef __ANDROID__
    // 非 Android 平台尝试额外的路径
    pathsToTry.push_back("../../" + path);                // 从 build/Debug 或 build/Release 返回项目根目录
    pathsToTry.push_back("../../../" + path);             // 从 build/Debug/xxx 返回项目根目录
    pathsToTry.push_back("../../assets/" + path);         // 从 build 目录返回项目根目录，然后进入 assets
    pathsToTry.push_back("../../../assets/" + path);      // 从更深的目录返回
#endif
    
    for (const auto& tryPath : pathsToTry) {
        scene = importer.ReadFile(
            tryPath,
            aiProcess_Triangulate |
            aiProcess_GenNormals |
            aiProcess_FlipUVs |
            aiProcess_CalcTangentSpace |
            aiProcess_LimitBoneWeights
        );
        
        if (scene && !(scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) && scene->mRootNode) {
            std::cout << "[ModelLoader] Successfully loaded model from: " << tryPath << std::endl;
            loadedPath = tryPath;
            break;
        }
    }

    if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) {
        std::cerr << "Assimp error: " << importer.GetErrorString() << std::endl;
        return result;
    }

    MeshData& data = result.meshData;

    std::map<std::string, std::string> boneParentMap;
    std::map<std::string, aiMatrix4x4> nodeTransforms;
    std::unordered_map<std::string, int> boneNameToIndex;

    std::function<void(aiNode*, const std::string&)> buildBoneHierarchy;
    buildBoneHierarchy = [&](aiNode* node, const std::string& parentName) {
        std::string nodeName = node->mName.C_Str();
        boneParentMap[nodeName] = parentName;
        nodeTransforms[nodeName] = node->mTransformation;

        for (unsigned int i = 0; i < node->mNumChildren; i++) {
            buildBoneHierarchy(node->mChildren[i], nodeName);
        }
    };

    buildBoneHierarchy(scene->mRootNode, "");

    auto toGlm = [](const aiMatrix4x4& m) -> glm::mat4 {
        return glm::mat4(m.a1, m.b1, m.c1, m.d1,
                         m.a2, m.b2, m.c2, m.d2,
                         m.a3, m.b3, m.c3, m.d3,
                         m.a4, m.b4, m.c4, m.d4);
    };

    // ---- glTF 静态 mesh node 世界变换烘焙（2026-08-16）----
    // 根因：assimp 的 aiMesh 顶点是 node 局部空间；多 node glTF（2CylinderEngine/Avocado 等）
    // 每个 mesh 挂在带 translation/rotation/scale 的 node 下，此前直接当模型空间使用 →
    // 所有 subMesh 画在实例原点互相重叠错位。单 node 模型（Helmet/Sponza）不受影响——
    // 与"一些模型会错位"的现象吻合。
    // 参考 glTF-Sample-Viewer：node 世界矩阵 = 父链连乘（gltf/scene.js applyTransformHierarchy）。
    // 引擎无 node 动画 → 静态 mesh 在加载时烘焙进顶点（法线用逆转置、镜像修正 tangent 手性）；
    // 蒙皮 mesh 顶点必须保持局部空间（骨骼 globalTransform*offsetMatrix 已含 node 树），不烘焙。
    // .gltf/.glb/.fbx 启用（2026-08-17：FBX 多 node 静态场景——Bistro 灯带等挂在带变换
    // node 下的 mesh 此前不烘焙 → 顶点留在 node 局部空间，画在实例原点附近/陷入地面）。
    // obj 无 node 树（单 node identity）不受影响；蒙皮 mesh 顶点保持局部空间（骨骼链路已含 node 树）。
    std::string bakeExt = std::filesystem::path(loadedPath).extension().string();
    for (auto& ch : bakeExt) ch = (char)std::tolower((unsigned char)ch);
    const bool bakeNodeTransforms = (bakeExt == ".gltf" || bakeExt == ".glb" || bakeExt == ".fbx");
    // mesh -> 引用它的 node 实例列表（world 矩阵 + node 名）。一个 mesh 可被多个 node 引用
    // （CesiumMilkTruck 的 Wheels 轮子对：2 个 node 各带 rotation 引用同一 wheel mesh）——
    // 每个实例必须生成独立 subMesh 副本，否则只有一个实例的位置正确。
    using NodeInstance = std::pair<glm::mat4, std::string>;
    std::vector<std::vector<NodeInstance>> meshWorldTransforms(scene->mNumMeshes);
    std::function<void(aiNode*, const glm::mat4&)> computeMeshWorld;
    computeMeshWorld = [&](aiNode* node, const glm::mat4& parentWorld) {
        glm::mat4 world = parentWorld * toGlm(node->mTransformation);
        for (unsigned int i = 0; i < node->mNumMeshes; i++) {
            unsigned int idx = node->mMeshes[i];
            if (idx < scene->mNumMeshes) {
                meshWorldTransforms[idx].emplace_back(world, node->mName.C_Str());
            }
        }
        for (unsigned int i = 0; i < node->mNumChildren; i++) {
            computeMeshWorld(node->mChildren[i], world);
        }
    };
    computeMeshWorld(scene->mRootNode, glm::mat4(1.0f));

    // ---- glTF 材质索引预解析（必须在 subMesh 循环前）----    // assimp 5.0 加载 gltf 时会重排材质（aiMesh::mMaterialIndex ≠ gltf materials 下标），
    // 必须用 gltf primitive->material 索引对齐（mesh/primitive 顺序与 assimp mesh 顺序一致）。
    std::vector<int> gltfMeshMaterial;   // mesh 顺序 -> gltf material index（-1 无）
    std::map<int, int> assimpToGltfMat;  // assimp material index -> gltf material index
    std::vector<float> gltfMatMetallic, gltfMatRoughness;   // 2026-08-11 glTF metallic/roughness factor（声明前置——subMesh 循环要用）
    // 2026-08-16 glTF alphaMode/alphaCutoff/doubleSided（声明前置——subMesh 循环要用；-1=未知）
    std::vector<int> gltfMatAlphaMode;
    std::vector<float> gltfMatAlphaCutoff;
    std::vector<bool> gltfMatDoubleSided;
    // 2026-08-16 glTF mesh 名字 → material 索引（assimp mesh 顺序 ≠ gltf mesh 顺序，名字映射修正材质对齐）
    std::map<std::string, int> gltfMeshNameToMat;
    {
        std::filesystem::path earlyPath = ProjectManager::GetInstance().ResolveAssetPath(path);
        std::string earlyExt = earlyPath.extension().string();
        for (auto& ch : earlyExt) ch = (char)std::tolower((unsigned char)ch);
        if (earlyExt == ".gltf") {
            std::ifstream gfs(earlyPath);
            if (gfs) {
                std::string text((std::istreambuf_iterator<char>(gfs)), std::istreambuf_iterator<char>());
                JsonLite::Value root;
                if (JsonLite::Parse(text, root)) {
                    // 2026-08-17：glTF 1.0 拦截——1.0（2017 废弃）材质/纹理/technique 用
                    // 字符串 id 引用，引擎材质链路（primitive->material 下标、pbrMetallicRoughness、
                    // alphaMode）全按 2.0 语义；assimp 5.0 对 1.0 勉强映射，加载后 GPU 驱动层崩溃
                    // （nvoglv64.dll 0xC0000005，BarramundiFish 复现）。拒绝加载并提示转 2.0。
                    if (const JsonLite::Value* asset = root.Get("asset")) {
                        if (const JsonLite::Value* ver = asset->Get("version")) {
                            float v = 0.0f;
                            if (ver->type == JsonLite::Value::Type::Number) v = (float)ver->num;
                            else if (ver->type == JsonLite::Value::Type::String && !ver->str.empty())
                                v = (float)atof(ver->str.c_str());
                            if (v > 0.0f && v < 2.0f) {
                                std::cerr << "[ModelLoader] glTF " << ver->str
                                          << " is deprecated (glTF 1.0) and unsupported - convert to glTF 2.0: "
                                          << path << std::endl;
                                return result;   // 空 result = 加载失败，调用方按失败处理（不崩）
                            }
                        }
                    }
                    if (const JsonLite::Value* meshes = root.Get("meshes")) {
                        for (size_t k = 0; k < meshes->Size(); ++k) {
                            const JsonLite::Value* msh = meshes->At(k);
                            const JsonLite::Value* prims = msh ? msh->Get("primitives") : nullptr;
                            if (!prims) continue;
                            for (size_t p = 0; p < prims->Size(); ++p) {
                                int mat = -1;
                                const JsonLite::Value* mm = prims->At(p)->Get("material");
                                if (mm && mm->type == JsonLite::Value::Type::Number) mat = (int)mm->num;
                                gltfMeshMaterial.push_back(mat);
                            }
                            // 2026-08-16：名字映射（assimp mesh 顺序 ≠ gltf mesh 顺序——AlphaBlendModeTest
                            // 实测顺序打乱，仅靠顺序对齐会错位；gltf mesh name 与 aiMesh.mName 一致（唯一名）
                            // 时用名字直查；重名 assimp 会加 -N 后缀 → 名字失败回退顺序兜底）
                            if (msh) {
                                if (const JsonLite::Value* nm = msh->Get("name")) {
                                    if (nm->type == JsonLite::Value::Type::String && !nm->str.empty()) {
                                        int firstMat = -1;
                                        if (prims->Size() > 0) {
                                            const JsonLite::Value* mm = prims->At(0)->Get("material");
                                            if (mm && mm->type == JsonLite::Value::Type::Number) firstMat = (int)mm->num;
                                        }
                                        gltfMeshNameToMat[nm->str] = firstMat;
                                    }
                                }
                            }
                        }
                    }
                }
            }
            // gltf primitive->material 已解析到 gltfMeshMaterial（按 gltf mesh 顺序）+ gltfMeshNameToMat（按名字）
        }
    }

    for (unsigned int m = 0; m < scene->mNumMeshes; m++) {
        aiMesh* mesh = scene->mMeshes[m];
        if (!mesh) {
            std::cerr << "[ModelLoader] Skipping null aiMesh at index " << m << std::endl;
            continue;
        }
        const bool meshHasBones = mesh->HasBones();

        // 多 node 引用同一静态 mesh（如 CesiumMilkTruck 轮子对：2 node 引用同一 wheel mesh）：
        // 每个引用 node 生成一个 subMesh 副本并各自烘焙 node 变换；蒙皮 mesh 顶点必须保持
        // 局部空间（骨骼 globalTransform*offsetMatrix 已含 node 树），不复制。
        const auto& instList = meshWorldTransforms[m];
        const size_t instanceCount =
            (bakeNodeTransforms && !meshHasBones && !instList.empty()) ? instList.size() : 1;

        for (size_t inst = 0; inst < instanceCount; inst++) {
        SubMesh subMesh;

        // 2026-08-09：subMesh 标识（per-subMesh 材质选择用）——优先 mesh 名，空则索引兜底
        const char* meshName = mesh->mName.C_Str();
        subMesh.name = (meshName && meshName[0] != '\0') ? meshName : ("SubMesh_" + std::to_string(m));
        // 多实例副本：名称加 node 名区分（材质面板 per-subMesh 选择）
        if (instanceCount > 1) subMesh.name += "#" + instList[inst].second;

        // 2026-08-17：静态路径已清理——统一蒙皮 Vertex 32B 渲染所有模型

        aiMaterial* material = nullptr;
        if (mesh->mMaterialIndex < scene->mNumMaterials) {
            material = scene->mMaterials[mesh->mMaterialIndex];
        }
        if (!material) {
            std::cerr << "[ModelLoader] Skipping mesh '" << subMesh.name
                      << "' with invalid material index " << mesh->mMaterialIndex << std::endl;
            continue;
        }
        aiString materialName;
        subMesh.materialIndex = (int)mesh->mMaterialIndex;   // 2026-08-17：assimp 材质索引（hasMRTexture 按索引回填）
        subMesh.srcMesh = (int)m;   // 2026-08-17：来源 mesh 序号（assimp=gltf mesh 顺序）
        // gltf 材质索引对齐：assimp 5.0 会重排 gltf 材质（mMaterialIndex ≠ gltf materials 下标），
        // 必须用 gltf primitive->material 索引命名，才能与 materialTextures（按 gltf 索引注入）匹配。
        // 2026-08-16：gltf 材质索引对齐——优先名字映射（assimp mesh 顺序 ≠ gltf mesh 顺序），
        // 名字失败（重名带 -N 后缀/无名字）回退顺序兜底（gltfMeshMaterial 按 gltf mesh 顺序）
        const char* meshNameC = mesh->mName.C_Str();
        int gltfMatIdx = -1;
        if (meshNameC && meshNameC[0] != '\0') {
            auto nameIt = gltfMeshNameToMat.find(meshNameC);
            if (nameIt != gltfMeshNameToMat.end()) gltfMatIdx = nameIt->second;
        }
        if (gltfMatIdx < 0 && m < gltfMeshMaterial.size()) gltfMatIdx = gltfMeshMaterial[m];
        if (gltfMatIdx >= 0) {
            subMesh.materialName = "Material_" + std::to_string(gltfMatIdx);
            subMesh.gltfMatIndex = gltfMatIdx;   // 2026-08-17：记录 gltf 材质索引（doubleSided 回填用）
            // 2026-08-11 glTF metallicFactor/roughnessFactor 写入 subMesh（MetalRoughSpheres 纯 factor 材质）
            if (gltfMatIdx < (int)gltfMatMetallic.size()) subMesh.metallic = gltfMatMetallic[gltfMatIdx];
            if (gltfMatIdx < (int)gltfMatRoughness.size()) subMesh.roughness = gltfMatRoughness[gltfMatIdx];
            // 2026-08-16 glTF alphaMode/alphaCutoff/doubleSided 写入 subMesh
            // ⚠️ 注意：materials 数组解析在 subMesh 循环之后（:800+），此处数组恒为空——
            // 实际生效通道 = MaterialTextureInfo 注入（:1050）+ CreateModelBuffers 匹配兜底；
            // 此注入保留供解析顺序调整后自然生效。
            if (gltfMatIdx < (int)gltfMatAlphaMode.size()) subMesh.alphaMode = gltfMatAlphaMode[gltfMatIdx];
            if (gltfMatIdx < (int)gltfMatAlphaCutoff.size()) subMesh.alphaCutoff = gltfMatAlphaCutoff[gltfMatIdx];
            if (gltfMatIdx < (int)gltfMatDoubleSided.size()) subMesh.doubleSided = gltfMatDoubleSided[gltfMatIdx];
            std::cout << "[ModelLoader] subMesh '" << subMesh.name << "' gltfMat=" << gltfMatIdx
                      << " metallic=" << subMesh.metallic << " roughness=" << subMesh.roughness
                      << " alphaMode=" << subMesh.alphaMode << " cutoff=" << subMesh.alphaCutoff
                      << " doubleSided=" << (subMesh.doubleSided ? 1 : 0) << std::endl;
            if (assimpToGltfMat.find(mesh->mMaterialIndex) == assimpToGltfMat.end()) {
                assimpToGltfMat[mesh->mMaterialIndex] = gltfMatIdx;
            }
        } else if (material->Get(AI_MATKEY_NAME, materialName) == AI_SUCCESS) {
            subMesh.materialName = materialName.C_Str();
        } else {
            // 无名称材质（gltfpack 生成的 gltf 材质常无 name）：用材质索引命名，与 materialTextures 的 fallback 一致
            subMesh.materialName = "Material_" + std::to_string(mesh->mMaterialIndex);
        }
        // [diag 已删] mesh 材质索引对齐诊断

        // glTF node 变换烘焙（仅静态 mesh）：位置 = world * 局部空间；
        // 法线/切线用逆转置（非等比缩放正确），镜像（det<0）翻转 tangent 手性。
        // 多实例：每个实例用自己的 nodeWorld（identity 变换也生成副本，仅顶点不变）。
        const glm::mat4 nodeWorld = (instanceCount > 1) ? instList[inst].first
                                    : (!instList.empty() ? instList[0].first : glm::mat4(1.0f));
        const bool bakeNode = bakeNodeTransforms && !meshHasBones && nodeWorld != glm::mat4(1.0f);
        glm::mat3 nodeNormalMat(1.0f);
        float nodeHandSign = 1.0f;
        if (bakeNode) {
            nodeNormalMat = glm::transpose(glm::inverse(glm::mat3(nodeWorld)));
            nodeHandSign = (glm::determinant(glm::mat3(nodeWorld)) < 0.0f) ? -1.0f : 1.0f;
        }

        for (unsigned int i = 0; i < mesh->mNumVertices; i++) {
            Vertex vertex;

            glm::vec3 pos = glm::vec3(
                mesh->mVertices[i].x,
                mesh->mVertices[i].y,
                mesh->mVertices[i].z
            );
            if (bakeNode) pos = glm::vec3(nodeWorld * glm::vec4(pos, 1.0f));
            vertex.Position = pos;

            glm::vec3 normal(0.0f, 1.0f, 0.0f);
            if (mesh->HasNormals()) {
                normal = glm::vec3(
                    mesh->mNormals[i].x,
                    mesh->mNormals[i].y,
                    mesh->mNormals[i].z
                );
                if (bakeNode) normal = glm::normalize(nodeNormalMat * normal);
            }
            vertex.Normal = PackSnorm3(normal);

            if (mesh->HasTextureCoords(0)) {
                vertex.TexCoords = PackHalf2(glm::vec2(
                    mesh->mTextureCoords[0][i].x,
                    1.0f - mesh->mTextureCoords[0][i].y
                ));
            } else {
                vertex.TexCoords = glm::u16vec2(0, 0);
            }

            if (mesh->HasTangentsAndBitangents()) {
                glm::vec3 tangent(
                    mesh->mTangents[i].x,
                    mesh->mTangents[i].y,
                    mesh->mTangents[i].z
                );
                if (bakeNode) tangent = glm::normalize(nodeNormalMat * tangent);
                glm::vec3 bitangent(
                    mesh->mBitangents[i].x,
                    mesh->mBitangents[i].y,
                    mesh->mBitangents[i].z
                );
                // 手性：bitangent 方向 = cross(normal, tangent) × ±1（shader 用 tangent.w 重建）
                float hand = glm::dot(glm::cross(normal, tangent), bitangent) >= 0.0f ? 1.0f : -1.0f;
                if (bakeNode) hand *= nodeHandSign;
                vertex.Tangent = PackSnorm3(tangent, hand);
            } else {
                vertex.Tangent = PackSnorm3(glm::vec3(1.0f, 0.0f, 0.0f), 1.0f);
            }

            // 骨骼字段始终初始化（蒙皮布局；静态布局无此字段）
            for (unsigned int j = 0; j < 4; j++) {
                vertex.BoneIDs[j] = 0xFF;
                vertex.BoneWeights[j] = 0;
            }

            if (meshHasBones) {
                subMesh.vertices.push_back(vertex);
            } else {
                // （统一布局：无骨骼也走蒙皮布局，weight 全 0 shader 直通；静态路径已清理）
                subMesh.vertices.push_back(vertex);
            }
        }

        for (unsigned int i = 0; i < mesh->mNumFaces; i++) {
            aiFace face = mesh->mFaces[i];
            for (unsigned int j = 0; j < face.mNumIndices; j++) {
                subMesh.indices.push_back(face.mIndices[j]);
            }
        }

        if (mesh->HasBones() && mesh->mBones) {
            for (unsigned int i = 0; i < mesh->mNumBones; i++) {
                aiBone* bone = mesh->mBones[i];
                // Some Assimp/glTF combinations can leave an empty slot in
                // the bone array (the Quaternius animation library exposes
                // this on Android).  Treat it as an absent influence instead
                // of dereferencing it during import.
                if (!bone) {
                    std::cerr << "[ModelLoader] Skipping null bone " << i
                              << " in mesh '" << subMesh.name << "'" << std::endl;
                    continue;
                }
                if (bone->mNumWeights > 0 && !bone->mWeights) {
                    std::cerr << "[ModelLoader] Skipping bone '" << bone->mName.C_Str()
                              << "' with missing weights" << std::endl;
                    continue;
                }
                std::string boneName = bone->mName.C_Str();

                int boneIndex = -1;
                for (size_t j = 0; j < data.bones.size(); j++) {
                    if (data.bones[j].name == boneName) {
                        boneIndex = static_cast<int>(j);
                        break;
                    }
                }

                if (boneIndex == -1) {
                    Bone newBone;
                    newBone.name = boneName;
                    newBone.parentIndex = -1;

                    auto it = boneParentMap.find(boneName);
                    if (it != boneParentMap.end() && !it->second.empty()) {
                        for (size_t j = 0; j < data.bones.size(); j++) {
                            if (data.bones[j].name == it->second) {
                                newBone.parentIndex = static_cast<int>(j);
                                break;
                            }
                        }
                    }

                    aiMatrix4x4 offsetMatrix = bone->mOffsetMatrix;
                    newBone.offsetMatrix = glm::mat4(
                        offsetMatrix.a1, offsetMatrix.b1, offsetMatrix.c1, offsetMatrix.d1,
                        offsetMatrix.a2, offsetMatrix.b2, offsetMatrix.c2, offsetMatrix.d2,
                        offsetMatrix.a3, offsetMatrix.b3, offsetMatrix.c3, offsetMatrix.d3,
                        offsetMatrix.a4, offsetMatrix.b4, offsetMatrix.c4, offsetMatrix.d4
                    );

                    boneIndex = static_cast<int>(data.bones.size());
                    data.bones.push_back(newBone);
                    boneNameToIndex[boneName] = boneIndex;
                }

                for (unsigned int j = 0; j < bone->mNumWeights; j++) {
                    unsigned int vertexID = bone->mWeights[j].mVertexId;
                    float weight = bone->mWeights[j].mWeight;

                    if (vertexID < subMesh.vertices.size()) {
                        Vertex& vertex = subMesh.vertices[vertexID];

                        for (int k = 0; k < 4; k++) {
                            if (vertex.BoneWeights[k] == 0) {
                                vertex.BoneIDs[k] = (uint8_t)glm::clamp(boneIndex, 0, MAX_BONES - 1);
                                vertex.BoneWeights[k] = (uint8_t)(glm::clamp(weight, 0.0f, 1.0f) * 255.0f);
                                break;
                            }
                        }
                    }
                }
            }
        }

        data.subMeshes.push_back(subMesh);
        } // for inst（多 node 实例副本）
    }

    // 用场景节点树变换计算骨骼绑定姿势（含根节点 Z_UP/Armature 等轴修正，glTF/FBX 坐标正确）。
    // 不能从 offsetMatrix 反推——那会丢失场景根节点（如 glTF 的 Z_UP）的轴变换，导致模型朝向错误。
    for (auto& bone : data.bones) {
        bone.localTransform = glm::inverse(bone.offsetMatrix); // 兜底（骨骼节点不在节点树时）
        auto it = nodeTransforms.find(bone.name);
        if (it != nodeTransforms.end()) {
            bone.localTransform = toGlm(it->second); // 场景节点局部变换（含根节点 TRS）
        }
        bone.bindLocalTransform = bone.localTransform; // 缓存绑定姿势（动画采样重置用）

        // 非骨骼祖先累计（Z_UP/Armature 等场景根轴修正；骨骼父由 parentIndex 链处理）
        glm::mat4 acc(1.0f);
        std::string cur = bone.name;
        while (!cur.empty()) {
            auto pit = boneParentMap.find(cur);
            if (pit == boneParentMap.end() || pit->second.empty()) break;
            const std::string& pname = pit->second;
            bool pIsBone = false;
            for (const auto& b : data.bones) {
                if (b.name == pname) { pIsBone = true; break; }
            }
            if (pIsBone) break; // 骨骼父交给 parentIndex 链
            auto nt = nodeTransforms.find(pname);
            if (nt != nodeTransforms.end()) acc = toGlm(nt->second) * acc;
            cur = pname;
        }
        bone.ancestorTransform = acc;
    }

    std::vector<bool> computed(data.bones.size(), false);
    bool progress = true;
    while (progress) {
        progress = false;
        for (size_t index = 0; index < data.bones.size(); index++) {
            if (computed[index]) continue;

            Bone& bone = data.bones[index];
            if (bone.parentIndex == -1 || computed[bone.parentIndex]) {
                if (bone.parentIndex == -1) {
                    bone.globalTransform = bone.ancestorTransform * bone.localTransform;
                } else {
                    bone.globalTransform = data.bones[bone.parentIndex].globalTransform * bone.localTransform;
                }

                bone.position = glm::vec3(bone.globalTransform[3]);
                bone.rotation = glm::quat_cast(bone.globalTransform);

                computed[index] = true;
                progress = true;
            }
        }
    }

    // ===== 骨骼动画解析（aiAnimation -> AnimationClip；assimp 归一化，glTF/FBX 通用）=====
    if (scene->mNumAnimations > 0 && !data.bones.empty()) {
        for (unsigned int a = 0; a < scene->mNumAnimations; a++) {
            const aiAnimation* aiAnim = scene->mAnimations[a];
            AnimationClip clip;
            clip.name = aiAnim->mName.C_Str();
            if (clip.name.empty()) clip.name = "anim_" + std::to_string(a);

            const double ticksPerSec = aiAnim->mTicksPerSecond > 0 ? aiAnim->mTicksPerSecond : 25.0;
            // glTF 动画时间轴修正：assimp 把 glTF 的秒（如 CesiumMan 0-2s）放大 1000 存成"毫秒"tick。
            double tickToSec = 1.0 / ticksPerSec;
            const double rawDurationSec = aiAnim->mDuration * tickToSec;
            if (rawDurationSec > 10.0) {
                tickToSec = 1.0 / 1000.0;
                printf("[ModelLoader] animation '%s': assimp tick mis-scaled (%fs) -> ms->s fix\n",
                    clip.name.c_str(), rawDurationSec);
            }
            clip.duration = (float)(aiAnim->mDuration * tickToSec);

            for (unsigned int c = 0; c < aiAnim->mNumChannels; c++) {
                const aiNodeAnim* channel = aiAnim->mChannels[c];
                auto nameIt = boneNameToIndex.find(channel->mNodeName.C_Str());
                if (nameIt == boneNameToIndex.end()) continue; // 非骨骼节点（相机等）跳过

                BoneChannel boneChannel;
                boneChannel.boneIndex = nameIt->second;

                // 统一时间轴：取三通道最大帧数；时间优先 rotation（骨骼动画通常 rotation 主导、position 可能单帧）
                unsigned int maxKeys = channel->mNumPositionKeys;
                maxKeys = std::max(maxKeys, channel->mNumRotationKeys);
                maxKeys = std::max(maxKeys, channel->mNumScalingKeys);

                for (unsigned int k = 0; k < maxKeys; k++) {
                    BoneKeyframe kf;
                    double t = 0.0;
                    if (channel->mNumRotationKeys > 0) {
                        t = channel->mRotationKeys[std::min(k, channel->mNumRotationKeys - 1)].mTime;
                    } else if (channel->mNumPositionKeys > 0) {
                        t = channel->mPositionKeys[std::min(k, channel->mNumPositionKeys - 1)].mTime;
                    } else if (channel->mNumScalingKeys > 0) {
                        t = channel->mScalingKeys[std::min(k, channel->mNumScalingKeys - 1)].mTime;
                    }
                    kf.time = (float)(t * tickToSec);

                    if (channel->mNumPositionKeys > 0) {
                        const auto& p = channel->mPositionKeys[std::min(k, channel->mNumPositionKeys - 1)].mValue;
                        kf.position = glm::vec3(p.x, p.y, p.z);
                    }
                    if (channel->mNumRotationKeys > 0) {
                        const auto& r = channel->mRotationKeys[std::min(k, channel->mNumRotationKeys - 1)].mValue;
                        kf.rotation = glm::quat(r.w, r.x, r.y, r.z);
                    }
                    if (channel->mNumScalingKeys > 0) {
                        const auto& s = channel->mScalingKeys[std::min(k, channel->mNumScalingKeys - 1)].mValue;
                        kf.scale = glm::vec3(s.x, s.y, s.z);
                    }
                    boneChannel.keyframes.push_back(kf);
                }
                clip.channels.push_back(boneChannel);
            }
            data.animations.push_back(clip);
        }
        std::cout << "[ModelLoader] Loaded " << data.animations.size() << " animation clip(s):";
        for (const auto& anim : data.animations) {
            std::cout << " '" << anim.name << "' (" << anim.duration << "s, " << anim.channels.size() << " channels)";
        }
        std::cout << std::endl;
    }



    // 基于实际加载成功的路径并绝对化（ProjectManager 按引擎根解析），
    // 避免场景序列化出的相对路径导致 modelDir/纹理解析依赖进程工作目录而失败（表现为模型变白）
    std::filesystem::path modelFilePath(ProjectManager::GetInstance().ResolveAssetPath(loadedPath));
    std::string modelDir = modelFilePath.parent_path().string();
    if (!modelDir.empty() && modelDir.back() != '/' && modelDir.back() != '\\') {
        modelDir += "/";
    }

#ifdef __ANDROID__
    printf("[ModelLoader] modelDir = '%s'", modelDir.c_str());
#endif

    auto resolveTexturePath = [&](const std::string& texturePath) -> std::string {
        std::string resolvedPath;
        if (texturePath.find("textures/") == 0 || texturePath.find("textures\\") == 0) {
            resolvedPath = modelDir + texturePath;
        } else if (texturePath.find("/") == 0 || texturePath.find("\\") == 0 || 
                  (texturePath.length() > 1 && texturePath[1] == ':')) {
            resolvedPath = texturePath;
        } else {
            resolvedPath = modelDir + texturePath;
#ifndef __ANDROID__
            std::filesystem::path checkPath(resolvedPath);
            if (!std::filesystem::exists(checkPath)) {
                resolvedPath = modelDir + "textures/" + texturePath;
            }
            // 2026-08 项目化（Unity 式）：mtl 纹理常见放在项目资源区根 textures/（相对项目根）
            if (!std::filesystem::exists(resolvedPath)) {
                const std::string projTextures = ProjectManager::GetInstance().GetAssetsDir() + "textures/" + texturePath;
                if (std::filesystem::exists(projTextures)) {
                    resolvedPath = projTextures;
                }
            }
#endif
        }
        
        // 统一使用正斜杠
        std::replace(resolvedPath.begin(), resolvedPath.end(), '\\', '/');
        
#ifdef __ANDROID__
        // Android: 确保路径以 assets/ 开头或相对于 assets 目录
        // 移除开头的 ./ 或 assets/ 前缀（如果存在）
        if (resolvedPath.find("./") == 0) {
            resolvedPath = resolvedPath.substr(2);
        }
        if (resolvedPath.find("assets/") == 0) {
            resolvedPath = resolvedPath.substr(7);
        }
        printf("[ModelLoader] Resolved texture path: '%s' (raw: '%s')", resolvedPath.c_str(), texturePath.c_str());
        return resolvedPath;
#else
        std::filesystem::path finalPath(resolvedPath);
        if (!std::filesystem::exists(finalPath)) {
            std::cerr << "[ModelLoader] Texture not found: " << resolvedPath << " (raw: " << texturePath << ")" << std::endl;
            return "";
        }
        std::cout << "[ModelLoader] Resolved texture: " << texturePath << " -> " << resolvedPath << std::endl;
        return resolvedPath;
#endif
    };

    // glTF/glb 内嵌纹理（assimp: 材质纹理路径为 "*N"，数据在 scene->mTextures[N]）：
    // 提取到 modelDir/textures/embedded_N.<ext> 文件后按普通纹理处理。
    auto resolveEmbeddedTexture = [&](const std::string& rawPath) -> std::string {
        if (rawPath.empty() || rawPath[0] != '*') {
            return resolveTexturePath(rawPath);
        }
        const int idx = atoi(rawPath.c_str() + 1);
        if (idx < 0 || idx >= (int)scene->mNumTextures) {
            std::cerr << "[ModelLoader] Embedded texture index out of range: " << rawPath << std::endl;
            return "";
        }
        const aiTexture* tex = scene->mTextures[idx];
        if (!tex || !tex->pcData) return "";

        // mHeight == 0: 压缩数据（mWidth 字节，achFormatHint 为扩展名）；mHeight > 0: 原始 RGBA
        const std::string ext = (tex->mHeight == 0) ? std::string(tex->achFormatHint) : "tga";
        const std::string outPath = modelDir + "textures/embedded_" + std::to_string(idx) + "." + ext;
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(outPath).parent_path(), ec);
        std::ofstream ofs(outPath, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) {
            std::cerr << "[ModelLoader] FAILED to write embedded texture: " << outPath << std::endl;
            return "";
        }
        if (tex->mHeight == 0) {
            ofs.write((const char*)tex->pcData, tex->mWidth);
        } else {
            // 原始 RGBA → TGA 头 + 数据（uncompressed true-color, 32bpp, top-left）
            unsigned char header[18] = {};
            header[2] = 2;
            header[12] = (unsigned char)(tex->mWidth & 0xFF);
            header[13] = (unsigned char)((tex->mWidth >> 8) & 0xFF);
            header[14] = (unsigned char)(tex->mHeight & 0xFF);
            header[15] = (unsigned char)((tex->mHeight >> 8) & 0xFF);
            header[16] = 32;
            header[17] = 0x20;
            ofs.write((const char*)header, 18);
            ofs.write((const char*)tex->pcData, (std::streamsize)tex->mWidth * tex->mHeight * 4);
        }
        ofs.close();
        std::cout << "[ModelLoader] Embedded texture " << rawPath << " -> " << outPath
                  << " (" << tex->mWidth << "x" << tex->mHeight << ")" << std::endl;
        return outPath;
    };

    // ---- glTF 手动纹理解析（assimp 不认 KHR_texture_basisu / .ktx2 引用）----
    // 解析 gltf json 的 images/textures/materials，按材质索引注入纹理路径（覆盖 assimp 的 png 引用），
    // 使 .ktx2 压缩纹理无需同目录 png 即可被引擎加载（真机同样适用）。
    bool isGltf = false;
    std::vector<std::string> gltfImageUris;      // image index -> resolved uri / "@bufferView"（glb 内嵌按 bufferView 直读）
    std::vector<std::string> gltfImageMime;      // image index -> mimeType（glb 内嵌提取扩展名用，可空）
    std::vector<std::pair<size_t, size_t>> glbBufferViews;   // bufferView index -> (byteOffset, byteLength)，相对 BIN chunk 数据起点
    std::vector<int> gltfTexSource;              // texture index -> image index（-1 无）

    // 2026-08-17：glb 内嵌纹理按 bufferView 从 BIN chunk 直读（不依赖 assimp mTextures 顺序——
    // 顺序 ≠ gltf images 顺序，DamagedHelmet 实测 normal/emissive 互换）。
    auto extractGlbBufferView = [&](int bvIdx, const std::string& mime) -> std::string {
        if (bvIdx < 0 || bvIdx >= (int)glbBufferViews.size()) {
            std::cerr << "[ModelLoader] glb bufferView out of range: " << bvIdx << std::endl;
            return "";
        }
        std::ifstream gfs(modelFilePath, std::ios::binary);
        if (!gfs) return "";
        char header[12]; gfs.read(header, 12);
        size_t binOff = 0, binLen = 0;
        while (gfs) {
            uint32_t clen = 0; char ctype[4] = {};
            gfs.read((char*)&clen, 4); gfs.read(ctype, 4);
            if (!gfs) break;
            if (memcmp(ctype, "BIN", 4) == 0) { binOff = (size_t)gfs.tellg(); binLen = clen; break; }
            gfs.seekg((std::streamoff)clen, std::ios::cur);
        }
        if (binLen == 0) {
            std::cerr << "[ModelLoader] glb BIN chunk not found: " << modelFilePath << std::endl;
            return "";
        }
        const size_t off = binOff + glbBufferViews[bvIdx].first;
        const size_t len = glbBufferViews[bvIdx].second;
        if (off + len > binOff + binLen) {
            std::cerr << "[ModelLoader] glb bufferView " << bvIdx << " out of BIN range" << std::endl;
            return "";
        }
        gfs.seekg((std::streamoff)off);
        std::string data(len, '\0');
        if (!gfs.read(&data[0], (std::streamsize)len)) return "";
        // 扩展名：mimeType 优先，其次魔数嗅探
        std::string ext;
        if (mime.find("png") != std::string::npos) ext = "png";
        else if (mime.find("jpeg") != std::string::npos) ext = "jpg";
        if (ext.empty()) {
            if (len > 8 && (unsigned char)data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') ext = "png";
            else if (len > 3 && (unsigned char)data[0] == 0xFF && (unsigned char)data[1] == 0xD8 && (unsigned char)data[2] == 0xFF) ext = "jpg";
            else ext = "bin";
        }
        const std::string outPath = modelDir + "textures/embedded_bv" + std::to_string(bvIdx) + "." + ext;
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(outPath).parent_path(), ec);
        std::ofstream ofs(outPath, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) {
            std::cerr << "[ModelLoader] FAILED to write glb bufferView texture: " << outPath << std::endl;
            return "";
        }
        ofs.write(data.data(), (std::streamsize)data.size());
        ofs.close();
        std::cout << "[ModelLoader] glb bufferView " << bvIdx << " -> " << outPath
                  << " (" << len << " bytes)" << std::endl;
        return outPath;
    };
    std::vector<int> gltfTexWrap;                // texture index -> wrapMode（VkSamplerAddressMode；默认 REPEAT）
    std::vector<int> gltfMatBaseColor, gltfMatNormal, gltfMatMR;  // material index -> texture index（-1 无）
    {
        std::string modelFileExt = modelFilePath.extension().string();
        for (auto& ch : modelFileExt) ch = (char)std::tolower((unsigned char)ch);
        std::string gltfJsonText;
        if (modelFileExt == ".gltf") {
            isGltf = true;
            std::ifstream gfs(modelFilePath);
            if (gfs) gltfJsonText.assign(std::istreambuf_iterator<char>(gfs), std::istreambuf_iterator<char>());
        } else if (modelFileExt == ".glb") {
            isGltf = true;
            // 2026-08-11 glb 二进制：12 字节头 + chunk0 = JSON（4 字节长度 + "JSON" 类型）——提取 JSON 文本
            std::ifstream gfs(modelFilePath, std::ios::binary);
            if (gfs) {
                char header[12]; gfs.read(header, 12);
                uint32_t chunkLen = 0; char chunkType[4] = {};
                gfs.read((char*)&chunkLen, 4); gfs.read(chunkType, 4);
                if (memcmp(chunkType, "JSON", 4) == 0 && chunkLen > 0 && chunkLen < (1u << 26)) {
                    gltfJsonText.resize(chunkLen);
                    gfs.read(&gltfJsonText[0], chunkLen);
                }
            }
        }
        if (isGltf && !gltfJsonText.empty()) {
            JsonLite::Value root;
            if (JsonLite::Parse(gltfJsonText, root)) {
                    // glb 内嵌纹理直读需要的 bufferViews 表（byteOffset 相对 buffer 即 BIN chunk 起点）
                    if (modelFileExt == ".glb") {
                        if (const JsonLite::Value* bvs = root.Get("bufferViews")) {
                            for (size_t k = 0; k < bvs->Size(); ++k) {
                                size_t boff = 0, blen = 0;
                                const JsonLite::Value* bv = bvs->At(k);
                                if (const JsonLite::Value* o = bv ? bv->Get("byteOffset") : nullptr)
                                    if (o->type == JsonLite::Value::Type::Number && o->num > 0) boff = (size_t)o->num;
                                if (const JsonLite::Value* l = bv ? bv->Get("byteLength") : nullptr)
                                    if (l->type == JsonLite::Value::Type::Number && l->num > 0) blen = (size_t)l->num;
                                glbBufferViews.emplace_back(boff, blen);
                            }
                        }
                    }
                    // 2026-08-17 修复：gltfMeshMaterial/gltfMeshNameToMat（mesh→材质）只在 .gltf 路径解析
                    // （253 行 if earlyExt==".gltf"）。.glb 未填充 → doubleSided/alphaMode 回填断/串位。
                    // 此处统一补：mesh 名→首 primitive 材质索引（gltfMeshNameToMat，subMesh 循环按名查→不串位）
                    // + 扁平 primitive 顺序（gltfMeshMaterial，仅兜底——assimp mesh 顺序 ≠ gltf 顺序会串位）。
                    if (gltfMeshMaterial.empty()) {
                        if (const JsonLite::Value* meshesA = root.Get("meshes")) {
                            for (size_t k = 0; k < meshesA->Size(); ++k) {
                                const JsonLite::Value* mshA = meshesA->At(k);
                                const JsonLite::Value* prims = mshA ? mshA->Get("primitives") : nullptr;
                                if (!prims) continue;
                                int firstMat = -1;
                                if (prims->Size() > 0)
                                    if (const JsonLite::Value* mm0 = prims->At(0)->Get("material"))
                                        if (mm0->type == JsonLite::Value::Type::Number) firstMat = (int)mm0->num;
                                if (mshA)
                                    if (const JsonLite::Value* nm2 = mshA->Get("name"))
                                        if (nm2->type == JsonLite::Value::Type::String && !nm2->str.empty())
                                            gltfMeshNameToMat[nm2->str] = firstMat;   // 名字映射（subMesh 循环 mesh->mName 查）
                                for (size_t p = 0; p < prims->Size(); ++p) {
                                    int mat = -1;
                                    if (const JsonLite::Value* mm = prims->At(p)->Get("material")) {
                                        if (mm->type == JsonLite::Value::Type::Number) mat = (int)mm->num;
                                    }
                                    gltfMeshMaterial.push_back(mat);
                                }
                            }
                        }
                        std::cout << "[ModelLoader] glb meshes->materials: " << gltfMeshMaterial.size()
                                  << " entries, nameMap=" << gltfMeshNameToMat.size() << std::endl;
                    }
                    if (const JsonLite::Value* images = root.Get("images")) {
                        for (size_t k = 0; k < images->Size(); ++k) {
                            const JsonLite::Value* img = images->At(k);
                            const JsonLite::Value* uri = img ? img->Get("uri") : nullptr;
                            std::string mime;
                            if (const JsonLite::Value* mt = img ? img->Get("mimeType") : nullptr) {
                                if (mt->type == JsonLite::Value::Type::String) mime = mt->str;
                            }
                            gltfImageMime.push_back(mime);
                            if (uri && uri->type == JsonLite::Value::Type::String) {
                                gltfImageUris.push_back(resolveTexturePath(uri->str));
                            } else if (img && img->Get("bufferView")) {
                                // 2026-08-17 修复：glb 内嵌纹理不能按 images 下标索引 assimp mTextures——
                                // assimp 顺序 ≠ images 顺序（DamagedHelmet 实测 normal/emissive 互换）。
                                // 记录 bufferView 索引，提取时从 glb 的 BIN chunk 按 bufferViews 直读。
                                const JsonLite::Value* bv = img->Get("bufferView");
                                if (bv && bv->type == JsonLite::Value::Type::Number && bv->num >= 0)
                                    gltfImageUris.push_back("@" + std::to_string((int)bv->num));
                                else
                                    gltfImageUris.push_back(std::string());
                            } else {
                                gltfImageUris.push_back(std::string());
                            }
                        }
                    }
                    // glTF samplers: wrapS/wrapT（10497=REPEAT, 33071=CLAMP_TO_EDGE, 33648=MIRRORED_REPEAT）
                    std::vector<int> gltfSamplerWrap;   // sampler index -> wrapMode（取 wrapS，默认 REPEAT）
                    if (const JsonLite::Value* samplers = root.Get("samplers")) {
                        for (size_t k = 0; k < samplers->Size(); ++k) {
                            int wrap = 10497;   // REPEAT
                            if (const JsonLite::Value* s = samplers->At(k)->Get("wrapS")) {
                                if (s->type == JsonLite::Value::Type::Number) wrap = (int)s->num;
                            }
                            gltfSamplerWrap.push_back(wrap);
                        }
                    }
                    if (const JsonLite::Value* textures = root.Get("textures")) {
                        for (size_t k = 0; k < textures->Size(); ++k) {
                            int src = -1;
                            int wrap = 10497;   // REPEAT
                            const JsonLite::Value* tex = textures->At(k);
                            if (tex) {
                                if (const JsonLite::Value* s = tex->Get("source")) {
                                    if (s->type == JsonLite::Value::Type::Number) src = (int)s->num;
                                }
                                // 纹理自身的 sampler（wrapS/wrapT → 环绕方式）
                                if (const JsonLite::Value* sm = tex->Get("sampler")) {
                                    if (sm->type == JsonLite::Value::Type::Number) {
                                        const int si = (int)sm->num;
                                        if (si >= 0 && si < (int)gltfSamplerWrap.size()) wrap = gltfSamplerWrap[si];
                                    }
                                }
                                // KHR_texture_basisu 扩展（.ktx2 纹理引用）
                                if (src < 0) {
                                    if (const JsonLite::Value* ext = tex->Get("extensions")) {
                                        if (const JsonLite::Value* basisu = ext->Get("KHR_texture_basisu")) {
                                            if (const JsonLite::Value* s = basisu->Get("source")) {
                                                if (s->type == JsonLite::Value::Type::Number) src = (int)s->num;
                                            }
                                        }
                                    }
                                }
                            }
                            gltfTexSource.push_back(src);
                            gltfTexWrap.push_back(wrap);
                        }
                    }
                    auto readTexIndex = [](const JsonLite::Value* mat, const char* slot) -> int {
                        if (!mat) return -1;
                        if (const JsonLite::Value* s = mat->Get(slot)) {
                            if (const JsonLite::Value* idx = s->Get("index")) {
                                if (idx->type == JsonLite::Value::Type::Number) return (int)idx->num;
                            }
                        }
                        return -1;
                    };
                    // 2026-08-11 glTF metallicFactor/roughnessFactor（pbrMetallicRoughness 数值——MetalRoughSpheres 纯 factor 材质）
                    auto readFactor = [](const JsonLite::Value* pbr, const char* key) -> float {
                        if (!pbr) return -1.0f;
                        if (const JsonLite::Value* f = pbr->Get(key)) {
                            if (f->type == JsonLite::Value::Type::Number) return (float)f->num;
                        }
                        return 1.0f;   // 有 pbr 块但无 factor 字段 → glTF 规范默认 1
                    };
                    if (const JsonLite::Value* materials = root.Get("materials")) {
                        for (size_t k = 0; k < materials->Size(); ++k) {
                            const JsonLite::Value* mat = materials->At(k);
                            const JsonLite::Value* pbr = mat ? mat->Get("pbrMetallicRoughness") : nullptr;
                            gltfMatBaseColor.push_back(readTexIndex(pbr, "baseColorTexture"));
                            gltfMatMR.push_back(readTexIndex(pbr, "metallicRoughnessTexture"));
                            gltfMatNormal.push_back(readTexIndex(mat, "normalTexture"));
                            gltfMatMetallic.push_back(readFactor(pbr, "metallicFactor"));   // 2026-08-11
                            gltfMatRoughness.push_back(readFactor(pbr, "roughnessFactor")); // 2026-08-11
                            // 2026-08-16 alphaMode/alphaCutoff/doubleSided（glTF 2.0 规范；缺失=OPAQUE/0.5/false）
                            int alphaMode = 0;       // 默认 OPAQUE
                            if (const JsonLite::Value* am = mat ? mat->Get("alphaMode") : nullptr) {
                                if (am->type == JsonLite::Value::Type::String) {
                                    if (am->str == "MASK") alphaMode = 1;
                                    else if (am->str == "BLEND") alphaMode = 2;
                                }
                            }
                            gltfMatAlphaMode.push_back(alphaMode);
                            float alphaCutoff = 0.5f;
                            if (const JsonLite::Value* ac = mat ? mat->Get("alphaCutoff") : nullptr) {
                                if (ac->type == JsonLite::Value::Type::Number) alphaCutoff = (float)ac->num;
                            }
                            gltfMatAlphaCutoff.push_back(alphaCutoff);
                            bool doubleSided = false;
                            if (const JsonLite::Value* ds = mat ? mat->Get("doubleSided") : nullptr) {
                                if (ds->type == JsonLite::Value::Type::Bool) doubleSided = ds->b;
                            }
                            gltfMatDoubleSided.push_back(doubleSided);
                        }
                    }
                }
            std::cout << "[ModelLoader] glTF textures: " << gltfImageUris.size() << " images, "
                      << gltfTexSource.size() << " textures, " << gltfMatBaseColor.size() << " materials" << std::endl;
            if (gltfMatBaseColor.size() != scene->mNumMaterials) {
                printf("[ModelLoader][diag] assimp materials=%u vs gltf materials=%zu（已用 gltf primitive->material 索引对齐）\n",
                       scene->mNumMaterials, gltfMatBaseColor.size());
            }
        }
    }
    // gltf 纹理按材质索引覆盖（assimp 的 png 引用不再需要；材质索引与 gltf materials 顺序对齐）
    auto applyGltfTex = [&](int matIdx, const std::vector<int>& matSlot, std::string& outPath, bool& outHas, int* outWrap = nullptr) {
        if (!isGltf || matIdx < 0 || matIdx >= (int)matSlot.size() || matSlot[matIdx] < 0) return;
        const int ti = matSlot[matIdx];
        if (ti < 0 || ti >= (int)gltfTexSource.size() || gltfTexSource[ti] < 0) return;
        const int ii = gltfTexSource[ti];
        if (ii >= 0 && ii < (int)gltfImageUris.size() && !gltfImageUris[ii].empty()) {
            // 2026-08-17 glb 内嵌（"@bufferView"）→ 从 BIN chunk 按 bufferView 直读；"*N" 为 assimp 语义（老路径兼容）
            const std::string& imgRef = gltfImageUris[ii];
            if (imgRef[0] == '@') {
                outPath = extractGlbBufferView(atoi(imgRef.c_str() + 1), ii < (int)gltfImageMime.size() ? gltfImageMime[ii] : std::string());
            } else if (imgRef[0] == '*') {
                outPath = resolveEmbeddedTexture(imgRef);
            } else {
                outPath = imgRef;
            }
            std::replace(outPath.begin(), outPath.end(), '\\', '/');
            outHas = true;
            if (outWrap && ti < (int)gltfTexWrap.size()) *outWrap = gltfTexWrap[ti];   // 纹理自身环绕（gltf sampler，per-texture）
        }
    };

    for (unsigned int i = 0; i < scene->mNumMaterials; i++) {
        aiMaterial* material = scene->mMaterials[i];
        MaterialTextureInfo info;

        aiString materialName;
        auto a2gIt = assimpToGltfMat.find(i);
        if (a2gIt != assimpToGltfMat.end()) {
            // 用 gltf 材质索引命名（与 subMesh.materialName 对齐，assimp 材质索引已重排）
            info.materialName = "Material_" + std::to_string(a2gIt->second);
        } else if (material->Get(AI_MATKEY_NAME, materialName) == AI_SUCCESS) {
            info.materialName = materialName.C_Str();
        } else {
            info.materialName = "Material_" + std::to_string(i);   // 无名称材质：材质索引命名
        }

        aiString texturePath;
        if (material->GetTexture(aiTextureType_DIFFUSE, 0, &texturePath) == AI_SUCCESS) {
            std::string fullPath = texturePath.C_Str();
            std::cout << "[ModelLoader] Material " << i << " diffuse texture (raw): " << fullPath << std::endl;
            info.diffuseTexturePath = resolveEmbeddedTexture(fullPath);
            std::replace(info.diffuseTexturePath.begin(), info.diffuseTexturePath.end(), '\\', '/');
            info.hasTexture = !info.diffuseTexturePath.empty();
            applyGltfTex(a2gIt != assimpToGltfMat.end() ? a2gIt->second : i, gltfMatBaseColor, info.diffuseTexturePath, info.hasTexture, &info.wrapMode);
#ifdef __ANDROID__
            printf("[ModelLoader] Material %d '%s': diffuse texture path = '%s' (raw: '%s')", 
                   i, info.materialName.c_str(), info.diffuseTexturePath.c_str(), fullPath.c_str());
#endif
        } else {
            std::cout << "[ModelLoader] Material " << i << " has no diffuse texture" << std::endl;
            info.hasTexture = false;
        }

        aiString normalTexturePath;
        bool foundNormalTexture = false;
        if (material->GetTexture(aiTextureType_NORMALS, 0, &normalTexturePath) == AI_SUCCESS) {
            std::string fullPath = normalTexturePath.C_Str();
            info.normalTexturePath = resolveEmbeddedTexture(fullPath);
            std::replace(info.normalTexturePath.begin(), info.normalTexturePath.end(), '\\', '/');
            info.hasNormalTexture = true;
            foundNormalTexture = true;
        }
        
        if (!foundNormalTexture && material->GetTexture(aiTextureType_HEIGHT, 0, &normalTexturePath) == AI_SUCCESS) {
            std::string fullPath = normalTexturePath.C_Str();
            info.normalTexturePath = resolveEmbeddedTexture(fullPath);
            std::replace(info.normalTexturePath.begin(), info.normalTexturePath.end(), '\\', '/');
            info.hasNormalTexture = true;
            foundNormalTexture = true;
        }
        
        // Sponza等模型使用map_Disp作为法线纹理
        if (!foundNormalTexture && material->GetTexture(aiTextureType_DISPLACEMENT, 0, &normalTexturePath) == AI_SUCCESS) {
            std::string fullPath = normalTexturePath.C_Str();
            info.normalTexturePath = resolveEmbeddedTexture(fullPath);
            std::replace(info.normalTexturePath.begin(), info.normalTexturePath.end(), '\\', '/');
            info.hasNormalTexture = true;
            foundNormalTexture = true;
        }
        
        if (!foundNormalTexture) {
            info.hasNormalTexture = false;
            info.normalTexturePath = "";
        }
        applyGltfTex(a2gIt != assimpToGltfMat.end() ? a2gIt->second : i, gltfMatNormal, info.normalTexturePath, info.hasNormalTexture);

        aiString specularTexturePath;
        bool foundSpecularTexture = false;
        if (material->GetTexture(aiTextureType_SPECULAR, 0, &specularTexturePath) == AI_SUCCESS) {
            std::string fullPath = specularTexturePath.C_Str();
            info.specularTexturePath = resolveEmbeddedTexture(fullPath);
            std::replace(info.specularTexturePath.begin(), info.specularTexturePath.end(), '\\', '/');
            info.hasSpecularTexture = true;
            foundSpecularTexture = true;
        }
        
        if (!foundSpecularTexture) {
            info.hasSpecularTexture = false;
            info.specularTexturePath = "";
        }
        
        // 加载粗糙度纹理
        aiString roughnessTexturePath;
        bool foundRoughnessTexture = false;
        if (material->GetTexture(aiTextureType_DIFFUSE_ROUGHNESS, 0, &roughnessTexturePath) == AI_SUCCESS) {
            std::string fullPath = roughnessTexturePath.C_Str();
            info.roughnessTexturePath = resolveEmbeddedTexture(fullPath);
            std::replace(info.roughnessTexturePath.begin(), info.roughnessTexturePath.end(), '\\', '/');
            info.hasRoughnessTexture = true;
            foundRoughnessTexture = true;
        }
        
        // 某些模型使用 SHININESS 作为粗糙度纹理
        if (!foundRoughnessTexture && material->GetTexture(aiTextureType_SHININESS, 0, &roughnessTexturePath) == AI_SUCCESS) {
            std::string fullPath = roughnessTexturePath.C_Str();
            info.roughnessTexturePath = resolveEmbeddedTexture(fullPath);
            std::replace(info.roughnessTexturePath.begin(), info.roughnessTexturePath.end(), '\\', '/');
            info.hasRoughnessTexture = true;
            foundRoughnessTexture = true;
        }
        
        if (!foundRoughnessTexture) {
            info.hasRoughnessTexture = false;
            info.roughnessTexturePath = "";
        }
        applyGltfTex(a2gIt != assimpToGltfMat.end() ? a2gIt->second : i, gltfMatMR, info.roughnessTexturePath, info.hasRoughnessTexture);
        
        // 加载金属度纹理
        aiString metallicTexturePath;
        bool foundMetallicTexture = false;
        if (material->GetTexture(aiTextureType_METALNESS, 0, &metallicTexturePath) == AI_SUCCESS) {
            std::string fullPath = metallicTexturePath.C_Str();
            info.metallicTexturePath = resolveEmbeddedTexture(fullPath);
            std::replace(info.metallicTexturePath.begin(), info.metallicTexturePath.end(), '\\', '/');
            info.hasMetallicTexture = true;
            foundMetallicTexture = true;
        }
        
        // 某些模型使用 AMBIENT_OCCLUSION 作为金属度纹理
        if (!foundMetallicTexture && material->GetTexture(aiTextureType_AMBIENT_OCCLUSION, 0, &metallicTexturePath) == AI_SUCCESS) {
            std::string fullPath = metallicTexturePath.C_Str();
            info.metallicTexturePath = resolveEmbeddedTexture(fullPath);
            std::replace(info.metallicTexturePath.begin(), info.metallicTexturePath.end(), '\\', '/');
            info.hasMetallicTexture = true;
            foundMetallicTexture = true;
        }
        
        if (!foundMetallicTexture) {
            info.hasMetallicTexture = false;
            info.metallicTexturePath = "";
        }
        applyGltfTex(a2gIt != assimpToGltfMat.end() ? a2gIt->second : i, gltfMatMR, info.metallicTexturePath, info.hasMetallicTexture);

        // 加载自发光纹理（Bistro 发光体材质：BaseColor 黑 + *_Emissive.dds 提供发光色）
        aiString emissiveTexPath;
        bool foundEmissiveTexture = false;
        if (material->GetTexture(aiTextureType_EMISSIVE, 0, &emissiveTexPath) == AI_SUCCESS) {
            std::string fullPath = emissiveTexPath.C_Str();
            info.emissiveTexturePath = resolveEmbeddedTexture(fullPath);
            std::replace(info.emissiveTexturePath.begin(), info.emissiveTexturePath.end(), '\\', '/');
            info.hasEmissiveTexture = true;
            foundEmissiveTexture = true;
        }
        if (!foundEmissiveTexture) {
            info.hasEmissiveTexture = false;
            info.emissiveTexturePath = "";
        };

        aiColor4D diffuse;
        if (material->Get(AI_MATKEY_COLOR_DIFFUSE, diffuse) == AI_SUCCESS) {
            info.diffuse = glm::vec4(diffuse.r, diffuse.g, diffuse.b, diffuse.a);
        } else {
            info.diffuse = glm::vec4(1.0f);
        }

        aiColor4D specular;
        if (material->Get(AI_MATKEY_COLOR_SPECULAR, specular) == AI_SUCCESS) {
            info.specular = glm::vec3(specular.r, specular.g, specular.b);
        } else {
            info.specular = glm::vec3(1.0f);
        }

        float shininess;
        if (material->Get(AI_MATKEY_SHININESS, shininess) == AI_SUCCESS) {
            info.specularPower = shininess;
        } else {
            info.specularPower = 32.0f;
        }

        aiColor4D ambient;
        if (material->Get(AI_MATKEY_COLOR_AMBIENT, ambient) == AI_SUCCESS) {
            info.ambient = glm::vec3(ambient.r, ambient.g, ambient.b);
        } else {
            info.ambient = glm::vec3(0.1f);
        }

        // 2026-08-16 alphaMode/alphaCutoff/doubleSided：gltf 材质索引对齐注入（assimp 材质索引已重排，
        // 必须走 assimpToGltfMat 映射；FBX/obj 无 gltf 元数据 → 保持 -1 未知（shader 回退旧 alpha 行为））
        int a2g = (a2gIt != assimpToGltfMat.end()) ? a2gIt->second : -1;
        if (a2g >= 0 && a2g < (int)gltfMatAlphaMode.size()) {
            info.alphaMode = gltfMatAlphaMode[a2g];
            info.alphaCutoff = gltfMatAlphaCutoff[a2g];
            info.doubleSided = gltfMatDoubleSided[a2g];
        }
        // 2026-08-16：glTF factor 同样在此注入（materials 解析在 subMesh 循环之后，subMesh 直接
        // 注入恒为空——实际走 materialTextures 匹配通道，见 CreateModelBuffers）
        if (a2g >= 0 && a2g < (int)gltfMatMetallic.size()) {
            info.metallic = gltfMatMetallic[a2g];
            info.roughness = gltfMatRoughness[a2g];
        }

        result.materialTextures.push_back(info);
        result.meshData.materialTextures.push_back(info);
    }

    // 2026-08-17 重实现：加载阶段标记 MR 纹理缺失——按 assimp 材质索引直接映射（不靠 materialName
    // 字符串匹配：MetalRoughSpheres 等多材质共享纹理/名字重排时名字匹配会错，导致有 MR 纹理的材质
    // 被误判成"无"全部回退成普通材质）。materialTextures 与 scene->mMaterials 1:1（同一索引空间）。
    std::vector<int> assimpHasMR(scene->mNumMaterials, 0);
    for (unsigned int mi = 0; mi < scene->mNumMaterials && mi < (unsigned int)result.materialTextures.size(); mi++) {
        const auto& mt = result.materialTextures[mi];
        if (!mt.roughnessTexturePath.empty() || !mt.metallicTexturePath.empty()) assimpHasMR[mi] = 1;
    }
    for (auto& sm : data.subMeshes) {
        sm.hasMRTexture = (sm.materialIndex >= 0 && sm.materialIndex < (int)assimpHasMR.size())
                              ? assimpHasMR[sm.materialIndex] : 0;
    }
    // 2026-08-17：doubleSided 也按 assimp 索引可靠回填（不靠 materialName 匹配 / alphaMode 条件——
    // ModelRenderer 652 的通道依赖名字匹配且被 alphaMode>=0 闸住，会漏）
    std::vector<int> assimpDS(scene->mNumMaterials, 0);
    for (unsigned mi = 0; mi < scene->mNumMaterials && mi < (unsigned int)result.materialTextures.size(); mi++)
        if (result.materialTextures[mi].doubleSided) assimpDS[mi] = 1;
    // 2026-08-17 主通道（按名，不串位）：subMesh.name = assimp mesh 名 = gltf mesh 名（本模型 9 mesh 均一致）
    // → gltfMeshNameToMat（名字→gltf 材质索引，已在 .gltf/.glb 双路径解析）→ gltfMatDoubleSided/AlphaMode/...
    // （assimp mesh 顺序 ≠ gltf 顺序，用顺序索引会串位——必须按名）
    for (auto& sm : data.subMeshes) {
        auto git = gltfMeshNameToMat.find(sm.name);
        if (git != gltfMeshNameToMat.end()) {
            int gi = git->second;
            if (gi >= 0 && gi < (int)gltfMatDoubleSided.size()) {
                sm.gltfMatIndex = gi;
                sm.doubleSided = gltfMatDoubleSided[gi];
                sm.alphaMode = gltfMatAlphaMode[gi];
                sm.alphaCutoff = gltfMatAlphaCutoff[gi];
                if (sm.metallic < 0) sm.metallic = gltfMatMetallic[gi];
                if (sm.roughness < 0) sm.roughness = gltfMatRoughness[gi];
                continue;
            }
        }
        if (sm.materialIndex >= 0 && sm.materialIndex < (int)assimpDS.size())
            sm.doubleSided = assimpDS[sm.materialIndex] != 0;   // 兜底（非 gltf 或名字未命中）
    }
    // 2026-08-17 临时诊断：加载阶段 doubleSided 回填值
    for (auto& sm : data.subMeshes)
        printf("[LoadDS] subMesh '%s' matIdx=%d doubleSided=%d\n", sm.name.c_str(), sm.materialIndex, (int)sm.doubleSided);
    for (unsigned mi = 0; mi < /*scene->mNumMaterials*/ (unsigned int)result.materialTextures.size(); mi++)
        printf("[LoadDS] mtl[%u] name='%s' doubleSided=%d\n", mi, result.materialTextures[mi].materialName.c_str(), (int)result.materialTextures[mi].doubleSided);

    // 添加到缓存（加锁）
    {
        std::lock_guard<std::mutex> lock(s_CacheMutex);
        s_ModelCache[path] = result;
    }
    std::cout << "[ModelLoader] Cached model: " << path << std::endl;

    return result;
}

// ===== 骨骼动画采样 =====
bool ModelLoader::SampleAnimation(const AnimationClip& clip, float time, std::vector<Bone>& bones) {
    if (bones.empty()) return false;

    // 每帧重置为绑定姿势（未在通道中的骨骼保持 bind pose；换 clip 不残留）
    for (auto& bone : bones) {
        bone.localTransform = bone.bindLocalTransform;
    }

    for (const auto& ch : clip.channels) {
        if (ch.boneIndex < 0 || ch.boneIndex >= (int)bones.size()) continue;
        const auto& kfs = ch.keyframes;
        if (kfs.empty()) continue;

        Bone& bone = bones[ch.boneIndex];

        // 定位插值段（关键帧按时间升序）
        size_t i = 0;
        while (i + 1 < kfs.size() && kfs[i + 1].time <= time) i++;

        glm::vec3 pos;
        glm::quat rot;
        glm::vec3 scl;
        if (i + 1 < kfs.size() && kfs[i + 1].time > kfs[i].time) {
            const BoneKeyframe& a = kfs[i];
            const BoneKeyframe& b = kfs[i + 1];
            const float span = b.time - a.time;
            const float f = (span > 0.0001f) ? glm::clamp((time - a.time) / span, 0.0f, 1.0f) : 1.0f;
            pos = glm::mix(a.position, b.position, f);
            rot = glm::slerp(a.rotation, b.rotation, f);
            scl = glm::mix(a.scale, b.scale, f);
        } else {
            pos = kfs.back().position;
            rot = kfs.back().rotation;
            scl = kfs.back().scale;
        }
        bone.localTransform = glm::translate(glm::mat4(1.0f), pos) * glm::mat4_cast(rot) * glm::scale(glm::mat4(1.0f), scl);
        bone.position = pos;
        bone.rotation = rot;
    }

    // 层级重算 globalTransform（含非骨骼祖先变换；依赖父先算）
    std::vector<bool> computed(bones.size(), false);
    for (int pass = 0; pass < (int)bones.size(); pass++) {
        bool any = false;
        for (size_t i = 0; i < bones.size(); i++) {
            if (computed[i]) continue;
            Bone& b = bones[i];
            if (b.parentIndex == -1 || computed[b.parentIndex]) {
                b.globalTransform = (b.parentIndex == -1)
                    ? b.ancestorTransform * b.localTransform
                    : bones[b.parentIndex].globalTransform * b.localTransform;
                computed[i] = true;
                any = true;
            }
        }
        if (!any) break;
    }
    return true;
}
