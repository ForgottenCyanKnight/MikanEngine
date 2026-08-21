#include "VoxLoader.h"
#include "Core/ProjectManager.h"
#include "Core/EngineConfig.h"
#include <fstream>
#include <iostream>
#include <cstring>
#include <unordered_map>
#include <set>
#ifdef __ANDROID__
#include <SDL3/SDL_iostream.h>


#endif

namespace VoxFormat {

// 默认调色板 (MagicaVoxel默认调色板)
static const uint32_t DEFAULT_PALETTE[256] = {
    0x00000000, 0xffffffff, 0xffccffff, 0xff99ffff, 0xff66ffff, 0xff33ffff, 0xff00ffff, 0xffffccff,
    0xffccccff, 0xff99ccff, 0xff66ccff, 0xff33ccff, 0xff00ccff, 0xffff99ff, 0xffcc99ff, 0xff9999ff,
    0xff6699ff, 0xff3399ff, 0xff0099ff, 0xffff66ff, 0xffcc66ff, 0xff9966ff, 0xff6666ff, 0xff3366ff,
    0xff0066ff, 0xffff33ff, 0xffcc33ff, 0xff9933ff, 0xff6633ff, 0xff3333ff, 0xff0033ff, 0xffff00ff,
    0xffcc00ff, 0xff9900ff, 0xff6600ff, 0xff3300ff, 0xff0000ff, 0xffffffcc, 0xffccffcc, 0xff99ffcc,
    0xff66ffcc, 0xff33ffcc, 0xff00ffcc, 0xffffcccc, 0xffcccccc, 0xff99cccc, 0xff66cccc, 0xff33cccc,
    0xff00cccc, 0xffff99cc, 0xffcc99cc, 0xff9999cc, 0xff6699cc, 0xff3399cc, 0xff0099cc, 0xffff66cc,
    0xffcc66cc, 0xff9966cc, 0xff6666cc, 0xff3366cc, 0xff0066cc, 0xffff33cc, 0xffcc33cc, 0xff9933cc,
    0xff6633cc, 0xff3333cc, 0xff0033cc, 0xffff00cc, 0xffcc00cc, 0xff9900cc, 0xff6600cc, 0xff3300cc,
    0xff0000cc, 0xffffff99, 0xffccff99, 0xff99ff99, 0xff66ff99, 0xff33ff99, 0xff00ff99, 0xffffcc99,
    0xffcccc99, 0xff99cc99, 0xff66cc99, 0xff33cc99, 0xff00cc99, 0xffff9999, 0xffcc9999, 0xff999999,
    0xff669999, 0xff339999, 0xff009999, 0xffff6699, 0xffcc6699, 0xff996699, 0xff666699, 0xff336699,
    0xff006699, 0xffff3399, 0xffcc3399, 0xff993399, 0xff663399, 0xff333399, 0xff003399, 0xffff0099,
    0xffcc0099, 0xff990099, 0xff660099, 0xff330099, 0xff000099, 0xffffff66, 0xffccff66, 0xff99ff66,
    0xff66ff66, 0xff33ff66, 0xff00ff66, 0xffffcc66, 0xffcccc66, 0xff99cc66, 0xff66cc66, 0xff33cc66,
    0xff00cc66, 0xffff9966, 0xffcc9966, 0xff999966, 0xff669966, 0xff339966, 0xff009966, 0xffff6666,
    0xffcc6666, 0xff996666, 0xff666666, 0xff336666, 0xff006666, 0xffff3366, 0xffcc3366, 0xff993366,
    0xff663366, 0xff333366, 0xff003366, 0xffff0066, 0xffcc0066, 0xff990066, 0xff660066, 0xff330066,
    0xff000066, 0xffffff33, 0xffccff33, 0xff99ff33, 0xff66ff33, 0xff33ff33, 0xff00ff33, 0xffffcc33,
    0xffcccc33, 0xff99cc33, 0xff66cc33, 0xff33cc33, 0xff00cc33, 0xffff9933, 0xffcc9933, 0xff999933,
    0xff669933, 0xff339933, 0xff009933, 0xffff6633, 0xffcc6633, 0xff996633, 0xff666633, 0xff336633,
    0xff006633, 0xffff3333, 0xffcc3333, 0xff993333, 0xff663333, 0xff333333, 0xff003333, 0xffff0033,
    0xffcc0033, 0xff990033, 0xff660033, 0xff330033, 0xff000033, 0xffffff00, 0xffccff00, 0xff99ff00,
    0xff66ff00, 0xff33ff00, 0xff00ff00, 0xffffcc00, 0xffcccc00, 0xff99cc00, 0xff66cc00, 0xff33cc00,
    0xff00cc00, 0xffff9900, 0xffcc9900, 0xff999900, 0xff669900, 0xff339900, 0xff009900, 0xffff6600,
    0xffcc6600, 0xff996600, 0xff666600, 0xff336600, 0xff006600, 0xffff3300, 0xffcc3300, 0xff993300,
    0xff663300, 0xff333300, 0xff003300, 0xffff0000, 0xffcc0000, 0xff990000, 0xff660000, 0xff330000,
    0xff0000ee, 0xff0000dd, 0xff0000bb, 0xff0000aa, 0xff000088, 0xff000077, 0xff000055, 0xff000044,
    0xff000022, 0xff000011, 0xff00ee00, 0xff00dd00, 0xff00bb00, 0xff00aa00, 0xff008800, 0xff007700,
    0xff005500, 0xff004400, 0xff002200, 0xff001100, 0xffee0000, 0xffdd0000, 0xffbb0000, 0xffaa0000,
    0xff880000, 0xff770000, 0xff550000, 0xff440000, 0xff220000, 0xff110000, 0xffeeeeee, 0xffdddddd,
    0xffbbbbbb, 0xffaaaaaa, 0xff888888, 0xff777777, 0xff555555, 0xff444444, 0xff222222, 0xff111111
};

VoxData::VoxData() {
    // 初始化默认调色板
    for (int i = 0; i < 256; i++) {
        uint32_t color = DEFAULT_PALETTE[i];
        palette[i].r = (color >> 24) & 0xFF;
        palette[i].g = (color >> 16) & 0xFF;
        palette[i].b = (color >> 8) & 0xFF;
        palette[i].a = color & 0xFF;
    }
}

// 确保结构体按1字节对齐，避免平台差异导致的大小变化
#pragma pack(push, 1)
struct ChunkHeader {
    uint32_t id;
    uint32_t contentSize;
    uint32_t childrenSize;
};
#pragma pack(pop)

static bool ReadChunkHeader(std::ifstream& file, ChunkHeader& header) {
    file.read(reinterpret_cast<char*>(&header.id), sizeof(header.id));
    file.read(reinterpret_cast<char*>(&header.contentSize), sizeof(header.contentSize));
    file.read(reinterpret_cast<char*>(&header.childrenSize), sizeof(header.childrenSize));
    return file.good();
}

// 自定义字节交换函数，用于小端序转换
static uint32_t SwapLE32(uint32_t value) {
    return ((value & 0x000000FF) << 24) |
           ((value & 0x0000FF00) << 8) |
           ((value & 0x00FF0000) >> 8) |
           ((value & 0xFF000000) >> 24);
}

bool LoadVoxFile(const std::string& path, VoxData& outData) {
    // 打印原始相对路径
    std::cout << "[VoxLoader] ========================================" << std::endl;
    std::cout << "[VoxLoader] Attempting to open: " << path << std::endl;
    
#ifdef __ANDROID__
    // Android 平台：清理路径，移除 assets/ 前缀（SDL3 在 Android 上会自动从 assets 目录读取）
    std::string androidPath = EngineConfig::StripAssetPrefix(path);
    
    // 统一使用正斜杠
    std::replace(androidPath.begin(), androidPath.end(), '\\', '/');
    
    // Android 平台使用 SDL_IOFromFile，直接使用相对于 assets 目录的路径
    printf("[VoxLoader] ========================================");
    printf("[VoxLoader] Attempting to open: %s", path.c_str());
    printf("[VoxLoader] Cleaned Android path: %s", androidPath.c_str());
    printf("[VoxLoader] Loading from Android assets: %s", androidPath.c_str());
    
    // 直接使用路径（SDL3 在 Android 上会自动从 assets 读取）
    SDL_IOStream* io = SDL_IOFromFile(androidPath.c_str(), "rb");
    if (io == nullptr) {
        printf("[VoxLoader] SDL_IOFromFile failed for path: %s", androidPath.c_str());
        printf("[VoxLoader] SDL Error: %s", SDL_GetError());
        
        // 尝试使用 android: 前缀
        std::string androidPrefixPath = "android:" + androidPath;
        printf("[VoxLoader] Trying with android: prefix: %s", androidPrefixPath.c_str());
        io = SDL_IOFromFile(androidPrefixPath.c_str(), "rb");
        if (io == nullptr) {
            printf("[VoxLoader] Failed with android: prefix as well");
            printf("[VoxLoader] SDL Error: %s", SDL_GetError());
            printf("[VoxLoader] ========================================");
            return false;
        }
    }
    
    printf("[VoxLoader] File opened successfully via SDL_IOFromFile");
    
    // 获取文件大小
    Sint64 fileSize = SDL_GetIOSize(io);
    printf("[VoxLoader] File size: %lld bytes", fileSize);
    
    if (fileSize < 12) {
        printf("[VoxLoader] File too small to be a valid VOX file");
        SDL_CloseIO(io);
        printf("[VoxLoader] ========================================");
        return false;
    }
    
    // 尝试将整个文件读入内存，然后再解析
    printf("[VoxLoader] Reading entire file into memory");
    std::vector<unsigned char> fileData(fileSize);
    size_t bytesRead = SDL_ReadIO(io, fileData.data(), fileSize);
    printf("[VoxLoader] Read %zu bytes into memory", bytesRead);
    
    if (bytesRead != fileSize) {
        printf("[VoxLoader] Failed to read entire file");
        SDL_CloseIO(io);
        printf("[VoxLoader] ========================================");
        return false;
    }
    
    SDL_CloseIO(io);
    
    // 解析文件头
    printf("[VoxLoader] Parsing file header from memory");
    
    // 解析 magic number (4 字节) - VOX 文件固定为小端序
    uint32_t magic = 0;
    memcpy(&magic, fileData.data(), 4);
    
    // VOX 文件使用小端序存储，直接使用读取的值
    // 不需要字节交换，因为我们是在小端序系统上运行
    
    char magicStr[16];
    snprintf(magicStr, sizeof(magicStr), "0x%08X", magic);
    printf("[VoxLoader] Magic (raw): %s", magicStr);
    printf("[VoxLoader] Expected VOX magic: 0x20584F56");
    
    // 打印文件头的十六进制数据，以便调试
    printf("[VoxLoader] File header hex: ");
    std::string hexString;
    for (int i = 0; i < 12; i++) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02X ", fileData[i]);
        hexString += hex;
    }
    printf("[VoxLoader] %s", hexString.c_str());
    
    uint32_t version = 0;
    memcpy(&version, fileData.data() + 4, 4);
    // 同样不需要字节交换
    printf("[VoxLoader] Version: %u", version);
    printf("[VoxLoader] Expected file version: 200");
    
    if (magic != VOX_MAGIC) {
        printf("[VoxLoader] Invalid VOX file magic number");
        printf("[VoxLoader] ========================================");
        return false;
    }

    printf("[VoxLoader] Loading VOX file version: %u", version);

    // 读取 MAIN chunk
    printf("[VoxLoader] Reading MAIN chunk from memory");
    
    // 逐字节读取MAIN chunk header，避免结构体对齐问题
    uint32_t mainChunkId, mainChunkContentSize, mainChunkChildrenSize;
    
    // 读取chunk id - 注意：VOX文件结构中，MAIN chunk id从位置8开始
    memcpy(&mainChunkId, fileData.data() + 8, 4);
    printf("[VoxLoader] Read MAIN chunk id: 0x%08X", mainChunkId);
    
    // 读取content size
    memcpy(&mainChunkContentSize, fileData.data() + 12, 4);
    printf("[VoxLoader] Read MAIN chunk contentSize: %u", mainChunkContentSize);
    
    // 读取children size
    memcpy(&mainChunkChildrenSize, fileData.data() + 16, 4);
    printf("[VoxLoader] Read MAIN chunk childrenSize: %u", mainChunkChildrenSize);
    
    // VOX文件使用小端序存储，直接使用读取的值
    // 不需要字节交换，因为我们是在小端序系统上运行
    
    char chunkName[5] = {0};
    chunkName[0] = (mainChunkId >> 24) & 0xFF;
    chunkName[1] = (mainChunkId >> 16) & 0xFF;
    chunkName[2] = (mainChunkId >> 8) & 0xFF;
    chunkName[3] = mainChunkId & 0xFF;
    
    char idStr[16];
    snprintf(idStr, sizeof(idStr), "0x%08X", mainChunkId);
    printf("[VoxLoader] MAIN chunk: id=%s (%s), contentSize=%u, childrenSize=%u", idStr, chunkName, mainChunkContentSize, mainChunkChildrenSize);

    if (mainChunkId != CHUNK_MAIN) {
        char expectedStr[16];
        snprintf(expectedStr, sizeof(expectedStr), "0x%08X", CHUNK_MAIN);
        printf("[VoxLoader] Expected MAIN chunk (%s), got %s", expectedStr, idStr);
        
        // 手动解析MAIN chunk id
        uint32_t manualMainChunkId = (fileData[11] << 24) | (fileData[10] << 16) | (fileData[9] << 8) | fileData[8];
        printf("[VoxLoader] Manual MAIN chunk id: 0x%08X", manualMainChunkId);
        printf("[VoxLoader] Expected MAIN chunk id: 0x%08X", CHUNK_MAIN);
        
        // 打印文件的前32字节，查看文件结构
        printf("[VoxLoader] File first 32 bytes hex: ");
        std::string hexString2;
        for (int i = 0; i < 32; i++) {
            char hex[3];
            snprintf(hex, sizeof(hex), "%02X ", fileData[i]);
            hexString2 += hex;
        }
        printf("[VoxLoader] %s", hexString2.c_str());
        
        printf("[VoxLoader] ========================================");
        return false;
    }

    printf("[VoxLoader] MAIN chunk found, childrenSize: %u", mainChunkChildrenSize);

    // MAIN chunk 没有内容，只有子 chunk
    size_t endPos = 20 + mainChunkChildrenSize;  // 20 = 8 (文件头) + 12 (MAIN chunk header)
    
    // 确保 endPos 不超过文件大小
    if (endPos > fileData.size()) {
        printf("[VoxLoader] WARNING: endPos exceeds file size, clamping");
        endPos = fileData.size();
    }
    
    printf("[VoxLoader] Will read chunks until position: %zu (file size: %zu)", 
        endPos, fileData.size());

    Model currentModel;
    bool hasCurrentModel = false;
    int chunkCount = 0;

    size_t currentPos = 20;  // 跳过文件头和 MAIN chunk header
    
    // 检查起始位置是否有效
    if (currentPos >= fileData.size()) {
        printf("[VoxLoader] ERROR: Starting position exceeds file size");
        return false;
    }
    
    while (currentPos < endPos) {
        // 检查是否有足够的数据读取 chunk header (12 字节)
        if (currentPos + 12 > fileData.size()) {
            printf("[VoxLoader] ERROR: Not enough data to read chunk header at pos %zu", currentPos);
            break;
        }
    
        printf("[VoxLoader] Reading chunk at position: %zu", currentPos);
        
        // 逐字节读取 chunk header，避免结构体对齐问题
        uint32_t chunkId, chunkContentSize, chunkChildrenSize;
        
        // 读取 chunk id
        memcpy(&chunkId, fileData.data() + currentPos, 4);
        currentPos += 4;
        
        // 读取 content size
        memcpy(&chunkContentSize, fileData.data() + currentPos, 4);
        currentPos += 4;
        
        // 读取 children size
        memcpy(&chunkChildrenSize, fileData.data() + currentPos, 4);
        currentPos += 4;
        
        // VOX 文件使用小端序存储，直接使用读取的值
        // 不需要字节交换，因为我们是在小端序系统上运行

        auto chunkStart = currentPos;
        chunkCount++;

        char chunkIdStr[16];
        snprintf(chunkIdStr, sizeof(chunkIdStr), "0x%08X", chunkId);
        char name[5] = {0};
        name[0] = (chunkId >> 24) & 0xFF;
        name[1] = (chunkId >> 16) & 0xFF;
        name[2] = (chunkId >> 8) & 0xFF;
        name[3] = chunkId & 0xFF;
        
        printf("[VoxLoader] Chunk #%d ID: %s (%s), contentSize: %u, childrenSize: %u", 
            chunkCount, chunkIdStr, name, chunkContentSize, chunkChildrenSize);

        if (chunkId == CHUNK_SIZE) {
            // SIZE chunk - 模型尺寸
            printf("[VoxLoader] Reading SIZE chunk data");
            uint32_t sizeX, sizeY, sizeZ;
            
            // 检查是否有足够的数据读取 3 个 uint32_t
            if (currentPos + 12 > fileData.size()) {
                printf("[VoxLoader] ERROR: SIZE chunk - not enough data");
                return false;
            }
            
            // 读取 sizeX
            memcpy(&sizeX, fileData.data() + currentPos, 4);
            currentPos += 4;
            
            // 读取 sizeY
            memcpy(&sizeY, fileData.data() + currentPos, 4);
            currentPos += 4;
            
            // 读取 sizeZ
            memcpy(&sizeZ, fileData.data() + currentPos, 4);
            currentPos += 4;
            
            // VOX 文件使用小端序存储，直接使用读取的值
            // 不需要字节交换，因为我们是在小端序系统上运行
            
            // 检查模型尺寸是否合理
            if (sizeX == 0 || sizeY == 0 || sizeZ == 0) {
                printf("[VoxLoader] ERROR: Invalid model size: %ux%ux%u", 
                    sizeX, sizeY, sizeZ);
                return false;
            }
            
            if (sizeX > 10000 || sizeY > 10000 || sizeZ > 10000) {
                printf("[VoxLoader] ERROR: Model size too large: %ux%ux%u", 
                    sizeX, sizeY, sizeZ);
                return false;
            }

            if (hasCurrentModel) {
                outData.models.push_back(currentModel);
            }
            currentModel = Model();
            currentModel.sizeX = sizeX;
            currentModel.sizeY = sizeY;
            currentModel.sizeZ = sizeZ;
            hasCurrentModel = true;

            printf("[VoxLoader] Model size: %ux%ux%u", sizeX, sizeY, sizeZ);
        }
        else if (chunkId == CHUNK_XYZI) {
            // XYZI chunk - 体素数据
            printf("[VoxLoader] Reading XYZI chunk data");
            uint32_t numVoxels;
            
            // 读取 numVoxels
            if (currentPos + 4 > fileData.size()) {
                printf("[VoxLoader] ERROR: XYZI chunk - not enough data to read numVoxels");
                return false;
            }
            memcpy(&numVoxels, fileData.data() + currentPos, 4);
            currentPos += 4;
            
            // VOX 文件使用小端序存储，直接使用读取的值
            // 不需要字节交换，因为我们是在小端序系统上运行

            printf("[VoxLoader] Reading %u voxels", numVoxels);
            
            // 检查体素数量是否合理（防止恶意文件）
            if (numVoxels == 0) {
                printf("[VoxLoader] WARNING: numVoxels is 0, skipping");
                continue;
            }
            
            if (numVoxels > 1000000) {
                printf("[VoxLoader] ERROR: numVoxels %u is too large, possible corrupted file", numVoxels);
                return false;
            }
            
            // 检查是否有足够的数据
            if (currentPos + sizeof(Voxel) * numVoxels > fileData.size()) {
                printf("[VoxLoader] ERROR: XYZI chunk - not enough data for voxel data");
                return false;
            }
            
            try {
                currentModel.voxels.resize(numVoxels);
            } catch (const std::bad_alloc& e) {
                printf("[VoxLoader] ERROR: Failed to allocate memory for voxels: %s", e.what());
                return false;
            }
            
            // 检查指针有效性
            void* srcPtr = fileData.data() + currentPos;
            void* dstPtr = currentModel.voxels.data();
            
            if (srcPtr == nullptr || dstPtr == nullptr) {
                printf("[VoxLoader] ERROR: Invalid pointer for memcpy");
                return false;
            }
            
            printf("[VoxLoader] Copying voxel data from offset %zu, size %zu", 
                currentPos, sizeof(Voxel) * numVoxels);
            
            memcpy(dstPtr, srcPtr, sizeof(Voxel) * numVoxels);
            currentPos += sizeof(Voxel) * numVoxels;

            printf("[VoxLoader] Voxel count: %u", numVoxels);
            
            // 记录第一个体素的数据用于调试
            if (numVoxels > 0) {
                const Voxel& firstVoxel = currentModel.voxels[0];
                printf("[VoxLoader] First voxel: (%u,%u,%u, color=%u)", 
                    firstVoxel.x, firstVoxel.y, firstVoxel.z, firstVoxel.colorIndex);
            }
        }
        else if (chunkId == CHUNK_RGBA) {
            // RGBA chunk - 调色板
            printf("[VoxLoader] Reading RGBA chunk (256 colors)");
            
            // 检查是否有足够的数据读取 256 个颜色（1024 字节）+ 1 字节填充
            if (currentPos + sizeof(Color) * 256 > fileData.size()) {
                printf("[VoxLoader] ERROR: RGBA chunk - not enough data for palette");
                return false;
            }
            
            for (int i = 1; i < 256; i++) {  // 索引 0 保留
                memcpy(&outData.palette[i], fileData.data() + currentPos, sizeof(Color));
                currentPos += sizeof(Color);
            }
            // 跳过一个字节 (填充)
            currentPos += 1;
            outData.hasCustomPalette = true;
            printf("[VoxLoader] Custom palette loaded");
        }
        else if (chunkId == CHUNK_IMAP) {
            // IMAP chunk - 索引映射，跳过
            printf("[VoxLoader] Skipping IMAP chunk");
            currentPos += chunkContentSize + chunkChildrenSize;
        }
        else if (chunkId == CHUNK_MATT) {
            // MATT chunk - 材质属性，跳过
            printf("[VoxLoader] Skipping MATT chunk");
            currentPos += chunkContentSize + chunkChildrenSize;
        }
        else {
            // 未知 chunk，跳过
            printf("[VoxLoader] Skipping unknown chunk");
            currentPos += chunkContentSize + chunkChildrenSize;
        }

        // 移动到下一个 chunk
        // 已经在上面处理了
    }

    if (hasCurrentModel) {
        outData.models.push_back(currentModel);
    }

    // 计算总体素数量
    size_t totalVoxels = 0;
    for (const auto& model : outData.models) {
        totalVoxels += model.voxels.size();
    }

    printf("[VoxLoader] Loaded %zu models", outData.models.size());
    printf("[VoxLoader] Total voxel count: %zu", totalVoxels);
    printf("[VoxLoader] ========================================");

    return true;    
#else
    // 非 Android 平台使用标准文件流
    // 优先使用相对路径（参考 ModelLoader 的方式）
    std::ifstream file;
    
    // 尝试路径的优先级：
    // 1. 直接使用路径（相对路径或绝对路径）
    // 2. 添加 ../../../ 前缀（从 build 目录返回项目根目录）
    // 3. 添加 ../../../assets/ 前缀
    
    std::vector<std::string> pathsToTry = {
        ProjectManager::GetInstance().ResolveAssetPath(path),  // 项目根解析（--project 支持）
        path,                           // 原始路径
        "../../../" + path,             // 从 build 目录返回项目根目录
        "../../../assets/" + path       // 从 build 目录返回项目根目录，然后进入 assets
    };
    
    bool fileOpened = false;
    for (const auto& tryPath : pathsToTry) {
        file.open(tryPath, std::ios::binary);
        if (file.is_open()) {
            // 只打印原始相对路径，不打印完整路径
            std::cout << "[VoxLoader] File opened successfully" << std::endl;
            fileOpened = true;
            break;
        }
    }
    
    if (!fileOpened) {
        std::cerr << "[VoxLoader] Failed to open file: " << path << std::endl;
        return false;
    }

    // 读取文件头
    uint32_t magic, version;
    file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    file.read(reinterpret_cast<char*>(&version), sizeof(version));
    
    std::cout << "[VoxLoader] Read magic: 0x" << std::hex << magic << std::dec 
              << " (expected: 0x" << std::hex << VOX_MAGIC << std::dec << ")" << std::endl;
    std::cout << "[VoxLoader] Read version: " << version << std::endl;

    if (magic != VOX_MAGIC) {
        std::cerr << "[VoxLoader] Invalid VOX file magic number" << std::endl;
        return false;
    }

    std::cout << "[VoxLoader] Loading VOX file version: " << version << std::endl;

    // 读取 MAIN chunk
    ChunkHeader mainChunk;
    if (!ReadChunkHeader(file, mainChunk) || mainChunk.id != CHUNK_MAIN) {
        std::cerr << "[VoxLoader] Expected MAIN chunk" << std::endl;
        return false;
    }

    // MAIN chunk 没有内容，只有子 chunk
    // 读取所有子 chunk
    auto endPos = file.tellg();
    endPos += static_cast<std::streamoff>(mainChunk.childrenSize);

    Model currentModel;
    bool hasCurrentModel = false;

    while (file.tellg() < endPos && file.good()) {
        ChunkHeader chunk;
        if (!ReadChunkHeader(file, chunk)) {
            break;
        }

        auto chunkStart = file.tellg();

        if (chunk.id == CHUNK_SIZE) {
            // SIZE chunk - 模型尺寸
            uint32_t sizeX, sizeY, sizeZ;
            file.read(reinterpret_cast<char*>(&sizeX), sizeof(sizeX));
            file.read(reinterpret_cast<char*>(&sizeY), sizeof(sizeY));
            file.read(reinterpret_cast<char*>(&sizeZ), sizeof(sizeZ));

            if (hasCurrentModel) {
                outData.models.push_back(currentModel);
            }
            currentModel = Model();
            currentModel.sizeX = sizeX;
            currentModel.sizeY = sizeY;
            currentModel.sizeZ = sizeZ;
            hasCurrentModel = true;

            std::cout << "[VoxLoader] Model size: " << sizeX << "x" << sizeY << "x" << sizeZ << std::endl;
        }
        else if (chunk.id == CHUNK_XYZI) {
            // XYZI chunk - 体素数据
            uint32_t numVoxels;
            file.read(reinterpret_cast<char*>(&numVoxels), sizeof(numVoxels));

            currentModel.voxels.resize(numVoxels);
            for (uint32_t i = 0; i < numVoxels; i++) {
                file.read(reinterpret_cast<char*>(&currentModel.voxels[i]), sizeof(Voxel));
            }

            std::cout << "[VoxLoader] Voxel count: " << numVoxels << std::endl;
        }
        else if (chunk.id == CHUNK_RGBA) {
            // RGBA chunk - 调色板
            for (int i = 1; i < 256; i++) {  // 索引 0 保留
                file.read(reinterpret_cast<char*>(&outData.palette[i]), sizeof(Color));
            }
            // 跳过一个字节 (填充)
            file.seekg(1, std::ios::cur);
            outData.hasCustomPalette = true;
            std::cout << "[VoxLoader] Custom palette loaded" << std::endl;
        }
        else {
            // 跳过未知的 chunk (扩展chunk如NRTn、PRGn、PHSn、RYAL等不影响基本渲染)
            std::string chunkName(4, '\0');
            chunkName[0] = (chunk.id >> 24) & 0xFF;
            chunkName[1] = (chunk.id >> 16) & 0xFF;
            chunkName[2] = (chunk.id >> 8) & 0xFF;
            chunkName[3] = chunk.id & 0xFF;
            // std::cout << "[VoxLoader] Skipping chunk: " << chunkName << std::endl;
        }

        // 跳到下一个 chunk
        file.seekg(chunkStart + static_cast<std::streamoff>(chunk.contentSize + chunk.childrenSize));
    }

    // 添加最后一个模型
    if (hasCurrentModel) {
        outData.models.push_back(currentModel);
    }

    file.close();
#endif
    
    // 计算总体素数量（只在非Android平台计算，Android平台已经计算过）
#ifndef __ANDROID__
    size_t totalVoxels = 0;
    for (const auto& model : outData.models) {
        totalVoxels += model.voxels.size();
    }
#endif

    std::cout << "[VoxLoader] ========================================" << std::endl;
    std::cout << "[VoxLoader] Successfully loaded " << outData.models.size() << " models" << std::endl;
    std::cout << "[VoxLoader] Total voxel count: " << totalVoxels << std::endl;
    std::cout << "[VoxLoader] ========================================" << std::endl;
    return !outData.models.empty();
}

} // namespace VoxFormat
