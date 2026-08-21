// TmxLoader.cpp - Tiled TMX 地图解析(正交): CSV/base64(zlib) 图层 + 内嵌/外部 tileset + 碰撞属性
#include "Core/TmxLoader.h"
#include <tinyxml2/tinyxml2.h>
#include <zlib/zlib.h>
#include <json.hpp>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace Tmx {

const Tileset* Map::FindTileset(int gid) const {
    if (gid <= 0) return nullptr;
    const Tileset* found = nullptr;
    for (const auto& ts : tilesets) {
        if (gid >= ts.firstGid && (!found || ts.firstGid > found->firstGid)) {
            found = &ts; // 取 firstGid 最大且 <= gid 的 tileset
        }
    }
    return found;
}

std::vector<unsigned char> DecodeBase64(const std::string& b64) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int lut[256];
    std::memset(lut, -1, sizeof(lut));
    for (int i = 0; i < 64; ++i) lut[(unsigned char)T[i]] = i;

    std::vector<unsigned char> out;
    int val = 0, bits = 0;
    for (char c : b64) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        int d = lut[(unsigned char)c];
        if (d < 0) continue;
        val = (val << 6) | d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((unsigned char)((val >> bits) & 0xFF));
        }
    }
    return out;
}

namespace {
// zlib 解压(base64 解码后的字节; compression="zlib", 带 zlib header 的流)
bool Inflate(const std::vector<unsigned char>& src, std::vector<unsigned char>& out) {
    if (src.empty()) return false;
    z_stream stream;
    std::memset(&stream, 0, sizeof(stream));
    if (inflateInit2(&stream, 15) != Z_OK) return false; // windowBits=15: zlib 封装(2 字节头)

    stream.next_in = const_cast<Bytef*>(src.data());
    stream.avail_in = (uInt)src.size();

    unsigned char buf[65536];
    int ret = Z_OK;
    do {
        stream.next_out = buf;
        stream.avail_out = sizeof(buf);
        ret = inflate(&stream, Z_NO_FLUSH);
        const size_t produced = sizeof(buf) - stream.avail_out;
        out.insert(out.end(), buf, buf + produced);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            inflateEnd(&stream);
            return false;
        }
    } while (ret != Z_STREAM_END);

    inflateEnd(&stream);
    return true;
}

// 解析瓦片本地 id(0-based) → collision 集合
void ParseTileCollisions(tinyxml2::XMLElement* tilesetElem, Tileset& ts) {
    for (tinyxml2::XMLElement* tile = tilesetElem->FirstChildElement("tile"); tile;
         tile = tile->NextSiblingElement("tile")) {
        int id = 0;
        if (tile->QueryIntAttribute("id", &id) != tinyxml2::XML_SUCCESS) continue;
        tinyxml2::XMLElement* props = tile->FirstChildElement("properties");
        if (!props) continue;
        for (tinyxml2::XMLElement* p = props->FirstChildElement("property"); p;
             p = p->NextSiblingElement("property")) {
            const char* name = p->Attribute("name");
            if (!name || std::strcmp(name, "collision") != 0) continue;
            const char* val = p->Attribute("value");
            bool isCollision = false;
            if (val) {
                isCollision = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
            } else {
                // 无 value 时按 bool 属性(默认 true)
                isCollision = true;
            }
            if (isCollision) ts.collidable.insert(id);
        }
    }
}

bool ParseTilesetElem(tinyxml2::XMLElement* elem, int firstGid, const std::string& baseDir, Tileset& out) {
    out.firstGid = firstGid;
    if (const char* n = elem->Attribute("name")) out.name = n;
    elem->QueryIntAttribute("tilewidth", &out.tileWidth);
    elem->QueryIntAttribute("tileheight", &out.tileHeight);
    elem->QueryIntAttribute("margin", &out.margin);
    elem->QueryIntAttribute("spacing", &out.spacing);
    elem->QueryIntAttribute("columns", &out.columns);
    elem->QueryIntAttribute("tilecount", &out.tileCount);

    tinyxml2::XMLElement* img = elem->FirstChildElement("image");
    if (img) {
        if (const char* src = img->Attribute("source")) {
            // 图片路径相对 tileset 文件所在目录; 归一化到"相对资产根"
            std::filesystem::path p(src);
            if (p.is_absolute()) {
                out.imagePath = src;
            } else {
                // baseDir 相对资产根; 拼接
                std::filesystem::path rel = std::filesystem::path(baseDir) / p;
                out.imagePath = rel.lexically_normal().generic_string();
            }
        }
        img->QueryIntAttribute("width", &out.imageWidth);
        img->QueryIntAttribute("height", &out.imageHeight);
    }
    ParseTileCollisions(elem, out);
    return out.tileWidth > 0 && out.tileHeight > 0;
}

bool ParseTilesetFile(const std::string& tsxPath, Tileset& out, std::string& err) {
    tinyxml2::XMLDocument doc;
    if (doc.LoadFile(tsxPath.c_str()) != tinyxml2::XML_SUCCESS) {
        err = "cannot open tsx: " + tsxPath;
        return false;
    }
    tinyxml2::XMLElement* tsElem = doc.FirstChildElement("tileset");
    if (!tsElem) { err = "no tileset in " + tsxPath; return false; }
    const std::string dir = std::filesystem::path(tsxPath).parent_path().generic_string();
    return ParseTilesetElem(tsElem, 1, dir, out); // tsx 内 firstGid 由引用方决定, 这里临时 1
}

// 解析图层 data(CSV 或 base64+zlib) → gids
bool ParseLayerData(tinyxml2::XMLElement* dataElem, Layer& layer) {
    const char* encoding = dataElem->Attribute("encoding");
    if (encoding && std::strcmp(encoding, "csv") == 0) {
        const char* text = dataElem->GetText();
        if (!text) return false;
        std::istringstream ss(text);
        std::string cell;
        int gid = 0;
        while (std::getline(ss, cell, ',')) {
            while (!cell.empty() && (cell.front() == ' ' || cell.front() == '\n' || cell.front() == '\r')) cell.erase(cell.begin());
            while (!cell.empty() && (cell.back() == ' ' || cell.back() == '\n' || cell.back() == '\r')) cell.pop_back();
            if (cell.empty()) continue;
            try { gid = std::stoi(cell); } catch (...) { gid = 0; }
            layer.gids.push_back(gid);
        }
        return !layer.gids.empty();
    }
    if (encoding && std::strcmp(encoding, "base64") == 0) {
        const char* text = dataElem->GetText();
        if (!text) return false;
        std::vector<unsigned char> bytes = DecodeBase64(text);
        const char* compression = dataElem->Attribute("compression");
        if (compression && std::strcmp(compression, "zlib") == 0) {
            std::vector<unsigned char> inflated;
            if (!Inflate(bytes, inflated)) return false;
            bytes = std::move(inflated);
        }
        // 每瓦片 4 字节小端 uint32
        layer.gids.reserve(bytes.size() / 4);
        for (size_t i = 0; i + 3 < bytes.size(); i += 4) {
            unsigned gid = (unsigned)bytes[i] | ((unsigned)bytes[i + 1] << 8) |
                           ((unsigned)bytes[i + 2] << 16) | ((unsigned)bytes[i + 3] << 24);
            layer.gids.push_back((int)(gid & 0x1FFFFFFFu)); // 清除翻转/旋转位
        }
        return !layer.gids.empty();
    }
    // 无 encoding: 空格分隔? 少见, 支持逗号/空格
    const char* text = dataElem->GetText();
    if (text) {
        std::istringstream ss(text);
        std::string cell;
        while (ss >> cell) {
            try { layer.gids.push_back(std::stoi(cell)); } catch (...) {}
        }
        return !layer.gids.empty();
    }
    return false;
}
} // namespace

bool Load(const std::string& tmxPath, Map& out, std::string& err) {    tinyxml2::XMLDocument doc;
    if (doc.LoadFile(tmxPath.c_str()) != tinyxml2::XML_SUCCESS) {
        err = "cannot open tmx: " + tmxPath;
        return false;
    }
    tinyxml2::XMLElement* mapElem = doc.FirstChildElement("map");
    if (!mapElem) { err = "no <map> in " + tmxPath; return false; }

    mapElem->QueryIntAttribute("width", &out.width);
    mapElem->QueryIntAttribute("height", &out.height);
    mapElem->QueryIntAttribute("tilewidth", &out.tileWidth);
    mapElem->QueryIntAttribute("tileheight", &out.tileHeight);

    const std::string tmxDir = std::filesystem::path(tmxPath).parent_path().generic_string();

    // tilesets(内嵌 + 外部)
    for (tinyxml2::XMLElement* tsElem = mapElem->FirstChildElement("tileset"); tsElem;
         tsElem = tsElem->NextSiblingElement("tileset")) {
        int firstGid = 1;
        tsElem->QueryIntAttribute("firstgid", &firstGid);
        const char* source = tsElem->Attribute("source");
        Tileset ts;
        if (source) {
            // 外部 .tsx: 路径相对 tmx 目录
            std::filesystem::path tsxRel = std::filesystem::path(tmxDir) / source;
            std::filesystem::path tsxAbs = tsxRel.is_absolute() ? tsxRel : tsxRel;
            if (!ParseTilesetFile(tsxAbs.generic_string(), ts, err)) { err = "tsx: " + err; return false; }
            ts.firstGid = firstGid;
        } else {
            if (!ParseTilesetElem(tsElem, firstGid, tmxDir, ts)) {
                err = "invalid inline tileset in " + tmxPath;
                return false;
            }
        }
        out.tilesets.push_back(std::move(ts));
    }

    // layers
    for (tinyxml2::XMLElement* layerElem = mapElem->FirstChildElement("layer"); layerElem;
         layerElem = layerElem->NextSiblingElement("layer")) {
        Layer layer;
        if (const char* n = layerElem->Attribute("name")) layer.name = n;
        layerElem->QueryIntAttribute("width", &layer.width);
        layerElem->QueryIntAttribute("height", &layer.height);
        int visible = 1;
        layerElem->QueryIntAttribute("visible", &visible);
        layer.visible = visible != 0;
        tinyxml2::XMLElement* dataElem = layerElem->FirstChildElement("data");
        if (dataElem && ParseLayerData(dataElem, layer)) {
            out.layers.push_back(std::move(layer));
        }
    }

    return !out.layers.empty() || out.width > 0;
}

// ===== 引擎自产资源(JSON, 编辑器切片器/瓦片绘制生成) =====

bool LoadTilesetJson(const std::string& jsonPath, Tileset& out, std::string& err) {
    std::ifstream ifs(jsonPath);
    if (!ifs) { err = "cannot open tileset json: " + jsonPath; return false; }
    nlohmann::json j;
    try { ifs >> j; } catch (...) { err = "invalid tileset json: " + jsonPath; return false; }

    out.firstGid = 1;
    out.name = j.value("name", "");
    out.tileWidth = j.value("tileWidth", 0);
    out.tileHeight = j.value("tileHeight", 0);
    out.margin = j.value("margin", 0);
    out.spacing = j.value("spacing", 0);
    out.columns = j.value("columns", 1);
    out.imagePath = j.value("image", "");
    out.imageWidth = j.value("imageWidth", 0);
    out.imageHeight = j.value("imageHeight", 0);
    out.tileCount = j.value("tileCount", out.columns * (out.imageHeight > 0 ? (out.imageHeight - 2 * out.margin) / (out.tileHeight + out.spacing) : 1));
    if (j.contains("collidable") && j["collidable"].is_array()) {
        for (auto& v : j["collidable"]) out.collidable.insert(v.get<int>());
    }
    if (out.tileWidth <= 0 || out.tileHeight <= 0 || out.imagePath.empty()) {
        err = "tileset json missing tileWidth/tileHeight/image: " + jsonPath;
        return false;
    }
    return true;
}

bool LoadTilemapJson(const std::string& jsonPath, Map& out, std::string& err) {
    std::ifstream ifs(jsonPath);
    if (!ifs) { err = "cannot open tilemap json: " + jsonPath; return false; }
    nlohmann::json j;
    try { ifs >> j; } catch (...) { err = "invalid tilemap json: " + jsonPath; return false; }

    out.width = j.value("width", 0);
    out.height = j.value("height", 0);
    out.tileWidth = j.value("tileWidth", 0);
    out.tileHeight = j.value("tileHeight", 0);

    // tileset 资源引用: 记录一个占位 Tileset, 由调用方(TilemapSystem)按名称加载实际资源
    const std::string tsName = j.value("tileset", "");
    if (tsName.empty()) { err = "tilemap json missing tileset: " + jsonPath; return false; }
    Tileset ts;
    ts.firstGid = 1;
    ts.name = tsName;
    out.tilesets.push_back(std::move(ts));

    if (j.contains("layers") && j["layers"].is_array()) {
        for (auto& lj : j["layers"]) {
            Layer layer;
            layer.name = lj.value("name", "Layer");
            layer.width = lj.value("width", out.width);
            layer.height = lj.value("height", out.height);
            if (lj.contains("gids") && lj["gids"].is_array()) {
                for (auto& g : lj["gids"]) layer.gids.push_back(g.get<int>());
            }
            out.layers.push_back(std::move(layer));
        }
    }
    return out.width > 0 && out.height > 0;
}

} // namespace Tmx
