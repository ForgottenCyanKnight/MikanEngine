#include "World/BlockManager.h"
#include "World/WorldTypes.h"
#include "Core/ProjectManager.h"
#include <stdexcept>
#include <algorithm>
#include <iostream>

BlockManager* BlockManager::instance = nullptr;

BlockManager::BlockManager() {
    try {
        // 优先使用 WorldConfig 指定的资源路径；桌面端未指定时从当前项目解析。
        std::string csvPath = GetWorldConfig().AssetPath;
        if (csvPath.empty()) {
#ifdef __ANDROID__
            csvPath = "assets/data/blocks.csv";
#else
            csvPath = ProjectManager::GetInstance().ResolveAssetPath("data/blocks.csv");
#endif
        } else {
            csvPath += "/data/blocks.csv";
        }
        LoadFromCSV(csvPath);
        // std::cout << "方块数据加载成功" << std::endl;
    }
    catch (const std::exception& e) {
        std::cerr << "方块数据加载失败: " << e.what() << std::endl;
    }
}
void BlockManager::LoadFromCSV(const std::string& filePath) {
    std::ifstream file(filePath);
    if (!file.is_open()) {
        throw std::runtime_error("无法打开方块数据文件: " + filePath);
    }

    std::string line;
    std::getline(file, line);

    while (std::getline(file, line)) {
        std::vector<std::string> row;
        std::stringstream ss(line);
        std::string cell;

        while (std::getline(ss, cell, ',')) {
            row.push_back(cell);
        }

        if (row.size() >= 14) {
            BlockType block = ParseCSVRow(row);
            int id = std::stoi(row[0]);
            RegisterBlock(id, block);
            transparentBlocks.set(id, block.isTransparent);
        }
    }
}

BlockType BlockManager::ParseCSVRow(const std::vector<std::string>& row) {
    BlockType block;

    block.name = row[1];
    block.isTransparent = std::stoi(row[2]) != 0;
    block.isSolid = std::stoi(row[3]) != 0;

    block.frontTexIndex = std::stoi(row[4]);    // front_tex
    block.backTexIndex = std::stoi(row[5]);     // back_tex
    block.leftTexIndex = std::stoi(row[6]);      // left_tex
    block.rightTexIndex = std::stoi(row[7]);     // right_tex
    block.topTexIndex = std::stoi(row[8]);       // top_tex
    block.bottomTexIndex = std::stoi(row[9]);    // bottom_tex

    block.hardness = row.size() > 10 ? std::stof(row[10]) : 0.0f;       // hardness
    block.dropId = row.size() > 11 ? std::stoi(row[11]) : std::stoi(row[0]); // drop_id
    block.emitsLight = row.size() > 12 ? std::stoi(row[12]) != 0 : false; // emits_light
    block.lightLevel = row.size() > 13 ? std::stoi(row[13]) : 0;        // light_level
    block.blockType = row.size() > 14 ? std::stoi(row[14]) : 0;         // block_type
    return block;
}


BlockManager& BlockManager::GetInstance() {
    // 线程安全 Meyers 单例：
    // 旧的 "if (!instance) instance = new ..." 在多线程下（多个 World worker 同时首次调用）
    // 存在竞态，会导致悬垂对象并触发 VCRUNTIME 访问冲突崩溃。
    static BlockManager instance;
    return instance;
}

void BlockManager::RegisterBlock(int id, const BlockType& type) {
    if (id >= 0 && id < 256) {
        fastblockTypes[id] = type;
        transparentBlocks.set(id, type.isTransparent);
    }
    blockTypes[id] = type;
}

const BlockType& BlockManager::GetBlockType(int id) const {
    return fastblockTypes[id];
}

const std::string& BlockManager::GetBlockName(int id) const {
    static const std::string unknownName = "unknown";

    auto it = blockTypes.find(id);
    return it != blockTypes.end() ? it->second.name : unknownName;
}
