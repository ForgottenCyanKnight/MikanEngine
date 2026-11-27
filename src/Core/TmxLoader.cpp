// TmxLoader.cpp - Tiled TMX 地图解析(正交): CSV/base64(zlib) 图层 + 内嵌/外部 tileset + 碰撞属性
#include "Core/TmxLoader.h"
#include "Core/AtomicFile.h"
#include "Core/Utf8Path.h"
#include <tinyxml2/tinyxml2.h>
#include <zlib/zlib.h>
#include <json.hpp>
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string_view>
#ifdef __ANDROID__
#include <SDL3/SDL.h>
#endif

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
bool ReadFile(const std::string& path, std::string& contents, std::string& err) {
    std::ifstream input(Utf8Path(path), std::ios::binary);
    if (!input) {
        err = "cannot open " + path;
        return false;
    }
    std::ostringstream read;
    read << input.rdbuf();
    if (!read || input.bad()) {
        err = "cannot read " + path;
        return false;
    }
    contents = std::move(read).str();
    return true;
}

bool ReadXml(tinyxml2::XMLDocument& document, const std::string& path, std::string& err) {
#ifdef __ANDROID__
    // SDL resolves relative paths inside the APK before trying the filesystem.
    size_t size = 0;
    void* bytes = SDL_LoadFile(path.c_str(), &size);
    if (!bytes) {
        err = "cannot open " + path + ": " + SDL_GetError();
        return false;
    }
    const auto result = document.Parse(static_cast<const char*>(bytes), size);
    SDL_free(bytes);
#else
    std::string contents;
    if (!ReadFile(path, contents, err)) return false;
    const auto result = document.Parse(contents.c_str(), contents.size());
#endif
    if (result == tinyxml2::XML_SUCCESS) return true;
    err = path + ": " + document.ErrorStr();
    return false;
}

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
            auto p = Utf8Path(src);
            if (p.is_absolute()) {
                out.imagePath = src;
            } else {
                // baseDir 相对资产根; 拼接
                const auto rel = Utf8Path(baseDir) / p;
                out.imagePath = GenericUtf8String(rel.lexically_normal());
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
    if (!ReadXml(doc, tsxPath, err)) return false;
    tinyxml2::XMLElement* tsElem = doc.FirstChildElement("tileset");
    if (!tsElem) { err = "no tileset in " + tsxPath; return false; }
    const std::string dir = GenericUtf8String(Utf8Path(tsxPath).parent_path());
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

constexpr unsigned kTmxTileIdMask = 0x0FFFFFFFu;

bool HasDefaultValue(const tinyxml2::XMLElement& element, const char* name, double expected) {
    double value = 0;
    const auto result = element.QueryDoubleAttribute(name, &value);
    return result == tinyxml2::XML_NO_ATTRIBUTE ||
        (result == tinyxml2::XML_SUCCESS && value == expected);
}

bool FitsAtlas(const Map& map, const Tileset& tileset) {
    if (tileset.firstGid <= 0 || tileset.tileCount <= 0 || tileset.columns <= 0 ||
        tileset.imagePath.empty() || tileset.margin < 0 || tileset.spacing < 0 ||
        tileset.tileWidth != map.tileWidth || tileset.tileHeight != map.tileHeight ||
        static_cast<std::uint64_t>(tileset.firstGid) + tileset.tileCount - 1 > kTmxTileIdMask) {
        return false;
    }
    const int rows = (tileset.tileCount - 1) / tileset.columns + 1;
    const auto extent = [&](int count, int pixels) {
        return 2LL * tileset.margin + 1LL * count * pixels + 1LL * (count - 1) * tileset.spacing;
    };
    return extent(tileset.columns, tileset.tileWidth) <= tileset.imageWidth &&
        extent(rows, tileset.tileHeight) <= tileset.imageHeight;
}

bool IsAtlasGid(unsigned gid, const Tileset& tileset) {
    return gid == 0 || (gid <= kTmxTileIdMask && gid >= static_cast<unsigned>(tileset.firstGid) &&
        gid - static_cast<unsigned>(tileset.firstGid) < static_cast<unsigned>(tileset.tileCount));
}

bool ReadEditableCsv(const char* text, const Tileset& tileset, Layer& layer) {
    if (!text) return false;
    constexpr std::string_view whitespace = " \t\r\n";
    const auto count = static_cast<std::uint64_t>(layer.width) * layer.height;
    std::string_view values(text);
    while (true) {
        const auto start = values.find_first_not_of(whitespace);
        if (start == std::string_view::npos) break;
        values.remove_prefix(start);

        unsigned gid = 0;
        const char* end = values.data() + values.size();
        const auto result = std::from_chars(values.data(), end, gid);
        if (result.ec != std::errc{} || !IsAtlasGid(gid, tileset) || layer.gids.size() >= count) {
            return false;
        }
        layer.gids.push_back(static_cast<int>(gid));
        values.remove_prefix(result.ptr - values.data());
        const auto separator = values.find_first_not_of(whitespace);
        if (separator == std::string_view::npos) break;
        if (values[separator] != ',') return false;
        values.remove_prefix(separator + 1);
    }
    return layer.gids.size() == count;
}

bool ReadEditableMap(tinyxml2::XMLDocument& document, const std::string& path,
                     Map& map, std::string& err) {
    auto* root = document.RootElement();
    if (!root || std::strcmp(root->Name(), "map") != 0 || !root->Attribute("orientation", "orthogonal") ||
        !HasDefaultValue(*root, "infinite", 0) ||
        (root->Attribute("renderorder") && !root->Attribute("renderorder", "right-down")) ||
        root->QueryIntAttribute("width", &map.width) != tinyxml2::XML_SUCCESS ||
        root->QueryIntAttribute("height", &map.height) != tinyxml2::XML_SUCCESS ||
        root->QueryIntAttribute("tilewidth", &map.tileWidth) != tinyxml2::XML_SUCCESS ||
        root->QueryIntAttribute("tileheight", &map.tileHeight) != tinyxml2::XML_SUCCESS ||
        map.width <= 0 || map.height <= 0 || map.tileWidth <= 0 || map.tileHeight <= 0) {
        err = "TMX editing requires a finite orthogonal right-down map";
        return false;
    }

    auto* reference = root->FirstChildElement("tileset");
    Tileset tileset;
    if (!reference || reference->NextSiblingElement("tileset") ||
        reference->QueryIntAttribute("firstgid", &tileset.firstGid) != tinyxml2::XML_SUCCESS) {
        err = "TMX editing requires exactly one atlas tileset with firstgid";
        return false;
    }
    auto tilesetDir = Utf8Path(path).parent_path();
    auto* definition = reference;
    tinyxml2::XMLDocument external;
    if (const char* source = reference->Attribute("source")) {
        const auto tsxPath = (tilesetDir / Utf8Path(source)).lexically_normal();
        if (!ReadXml(external, GenericUtf8String(tsxPath), err)) return false;
        definition = external.FirstChildElement("tileset");
        tilesetDir = tsxPath.parent_path();
    }
    if (!definition || definition->FirstChildElement("tileoffset") ||
        !definition->FirstChildElement("image") ||
        !ParseTilesetElem(definition, tileset.firstGid, GenericUtf8String(tilesetDir), tileset) ||
        !FitsAtlas(map, tileset)) {
        err = "TMX editing requires a single-image atlas matching the map grid";
        return false;
    }
    for (auto* tile = definition->FirstChildElement("tile"); tile; tile = tile->NextSiblingElement("tile")) {
        if (tile->FirstChildElement("animation") || tile->FirstChildElement("image")) {
            err = "TMX editing does not support animated tiles or per-tile images";
            return false;
        }
    }

    for (auto* element = root->FirstChildElement(); element; element = element->NextSiblingElement()) {
        const std::string_view kind = element->Name();
        if (kind == "tileset" || kind == "properties" || kind == "editorsettings") continue;
        if (kind != "layer") {
            err = "unsupported TMX map element: " + std::string(kind);
            return false;
        }
        Layer layer;
        if (const char* name = element->Attribute("name")) layer.name = name;
        for (const auto* child = element->FirstChildElement(); child; child = child->NextSiblingElement()) {
            const std::string_view childKind = child->Name();
            if (childKind == "data" || childKind == "properties") continue;
            err = "unsupported TMX layer element: " + std::string(childKind);
            return false;
        }
        auto* data = element->FirstChildElement("data");
        const auto* text = data && data->FirstChild() ? data->FirstChild()->ToText() : nullptr;
        int visible = 1;
        const auto visibility = element->QueryIntAttribute("visible", &visible);
        if ((visibility != tinyxml2::XML_NO_ATTRIBUTE && visibility != tinyxml2::XML_SUCCESS) ||
            (visible != 0 && visible != 1) ||
            !HasDefaultValue(*element, "x", 0) || !HasDefaultValue(*element, "y", 0) ||
            !HasDefaultValue(*element, "offsetx", 0) || !HasDefaultValue(*element, "offsety", 0) ||
            !HasDefaultValue(*element, "opacity", 1) ||
            !HasDefaultValue(*element, "parallaxx", 1) || !HasDefaultValue(*element, "parallaxy", 1) ||
            element->Attribute("tintcolor") || element->Attribute("mode") ||
            element->QueryIntAttribute("width", &layer.width) != tinyxml2::XML_SUCCESS ||
            element->QueryIntAttribute("height", &layer.height) != tinyxml2::XML_SUCCESS ||
            layer.width != map.width || layer.height != map.height ||
            !data || !data->Attribute("encoding", "csv") || data->Attribute("compression") ||
            data->NextSiblingElement("data") || !text || text->NextSibling() ||
            !ReadEditableCsv(text->Value(), tileset, layer)) {
            err = "TMX layer '" + layer.name + "' must be an untransformed full-size CSV tile layer";
            return false;
        }
        layer.visible = visible != 0;
        map.layers.push_back(std::move(layer));
    }
    if (map.layers.empty()) {
        err = "TMX map has no editable tile layers";
        return false;
    }
    map.tilesets.push_back(std::move(tileset));
    return true;
}

std::string FormatEditableCsv(const Layer& layer) {
    std::string csv = "\n";
    for (size_t index = 0; index < layer.gids.size(); ++index) {
        if (index != 0) csv += index % static_cast<size_t>(layer.width) == 0 ? ",\n" : ",";
        csv += std::to_string(layer.gids[index]);
    }
    csv += '\n';
    return csv;
}
} // namespace

bool Load(const std::string& tmxPath, Map& destination, std::string& err) {
    tinyxml2::XMLDocument doc;
    if (!ReadXml(doc, tmxPath, err)) return false;
    tinyxml2::XMLElement* mapElem = doc.FirstChildElement("map");
    if (!mapElem) { err = "no <map> in " + tmxPath; return false; }

    Map out;
    mapElem->QueryIntAttribute("width", &out.width);
    mapElem->QueryIntAttribute("height", &out.height);
    mapElem->QueryIntAttribute("tilewidth", &out.tileWidth);
    mapElem->QueryIntAttribute("tileheight", &out.tileHeight);

    const auto tmxDir = Utf8Path(tmxPath).parent_path();

    // tilesets(内嵌 + 外部)
    for (tinyxml2::XMLElement* tsElem = mapElem->FirstChildElement("tileset"); tsElem;
         tsElem = tsElem->NextSiblingElement("tileset")) {
        int firstGid = 1;
        tsElem->QueryIntAttribute("firstgid", &firstGid);
        const char* source = tsElem->Attribute("source");
        Tileset ts;
        if (source) {
            // 外部 .tsx: 路径相对 tmx 目录
            const auto tsxPath = (tmxDir / Utf8Path(source)).lexically_normal();
            if (!ParseTilesetFile(GenericUtf8String(tsxPath), ts, err)) {
                err = "tsx: " + err;
                return false;
            }
            ts.firstGid = firstGid;
        } else {
            if (!ParseTilesetElem(tsElem, firstGid, GenericUtf8String(tmxDir), ts)) {
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

    if (out.layers.empty() && out.width <= 0) {
        err = "map has no tile layers or width: " + tmxPath;
        return false;
    }
    destination = std::move(out);
    return true;
}

bool LoadEditable(const std::string& path, Map& out, std::string& source, std::string& err) {
    std::string bytes;
    if (!ReadFile(path, bytes, err)) return false;
    tinyxml2::XMLDocument document;
    if (document.Parse(bytes.c_str(), bytes.size()) != tinyxml2::XML_SUCCESS) {
        err = path + ": " + document.ErrorStr();
        return false;
    }
    Map map;
    if (!ReadEditableMap(document, path, map, err)) return false;
    out = std::move(map);
    source = std::move(bytes);
    err.clear();
    return true;
}

bool SaveEditable(const std::string& path, const Map& map, std::string& source, std::string& err) {
    // Serialize saves within the editor so two opened copies cannot both pass
    // the source check and then replace the same map.
    static std::mutex saveMutex;
    const std::lock_guard locker(saveMutex);
    std::error_code statusError;
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(Utf8Path(path), statusError))) {
        err = "cannot replace a linked TMX map: " + path;
        return false;
    }
    std::string current;
    if (!ReadFile(path, current, err)) return false;
    if (current != source) {
        err = "TMX map changed on disk. Reopen it before saving.";
        return false;
    }
    tinyxml2::XMLDocument document;
    if (document.Parse(current.c_str(), current.size()) != tinyxml2::XML_SUCCESS) {
        err = path + ": " + document.ErrorStr();
        return false;
    }
    Map original;
    if (!ReadEditableMap(document, path, original, err)) return false;
    if (map.width != original.width || map.height != original.height ||
        map.tileWidth != original.tileWidth || map.tileHeight != original.tileHeight ||
        map.layers.size() != original.layers.size()) {
        err = "TMX editing does not change map dimensions or layer structure";
        return false;
    }
    if (map.tilesets != original.tilesets) {
        err = "TMX tileset changed since the map was opened. Reopen it before saving.";
        return false;
    }

    auto* element = document.RootElement()->FirstChildElement("layer");
    bool changed = false;
    for (size_t index = 0; index < map.layers.size(); ++index, element = element->NextSiblingElement("layer")) {
        const auto& before = original.layers[index];
        const auto& after = map.layers[index];
        if (after.name != before.name || after.width != before.width || after.height != before.height ||
            after.gids.size() != before.gids.size()) {
            err = "TMX editing does not change layer names or dimensions";
            return false;
        }
        if (after.gids != before.gids) {
            for (int gid : after.gids) {
                if (!IsAtlasGid(static_cast<unsigned>(gid), original.tilesets.front())) {
                    err = "invalid tile id in TMX layer: " + after.name;
                    return false;
                }
            }
            element->FirstChildElement("data")->SetText(FormatEditableCsv(after).c_str());
            changed = true;
        }
        if (after.visible != before.visible) {
            element->SetAttribute("visible", after.visible ? 1 : 0);
            changed = true;
        }
    }
    if (!changed) {
        err.clear();
        return true;
    }

    tinyxml2::XMLPrinter printer;
    document.Print(&printer);
    std::string replacement(printer.CStr(), printer.CStrSize() - 1);
    if (!Core::WriteAssetFileAtomically(path, replacement, "tilemap")) {
        err = "cannot save TMX map: " + path;
        return false;
    }
    source = std::move(replacement);
    err.clear();
    return true;
}

// ===== 引擎自产资源(JSON, 编辑器切片器/瓦片绘制生成) =====

bool LoadTilesetJson(const std::string& jsonPath, Tileset& destination, std::string& err) {
    std::ifstream ifs(Utf8Path(jsonPath));
    if (!ifs) { err = "cannot open tileset json: " + jsonPath; return false; }
    Tileset atlas;
    try {
        nlohmann::json j;
        ifs >> j;
        atlas.name = j.value("name", "");
        atlas.tileWidth = j.value("tileWidth", 0);
        atlas.tileHeight = j.value("tileHeight", 0);
        atlas.margin = j.value("margin", 0);
        atlas.spacing = j.value("spacing", 0);
        atlas.columns = j.value("columns", 1);
        atlas.imagePath = j.value("image", "");
        atlas.imageWidth = j.value("imageWidth", 0);
        atlas.imageHeight = j.value("imageHeight", 0);
        if (atlas.tileWidth <= 0 || atlas.tileHeight <= 0 || atlas.columns <= 0 ||
            atlas.margin < 0 || atlas.spacing < 0 || atlas.imagePath.empty()) {
            err = "invalid tileset grid or missing image: " + jsonPath;
            return false;
        }
        if (j.contains("tileCount")) {
            atlas.tileCount = j.at("tileCount").get<int>();
        } else {
            const int64_t stride = static_cast<int64_t>(atlas.tileHeight) + atlas.spacing;
            const int64_t usable = atlas.imageHeight - 2LL * atlas.margin + atlas.spacing;
            const int64_t rows = atlas.imageHeight > 0 ? std::max<int64_t>(0, usable / stride) : 1;
            const int64_t count = rows * atlas.columns;
            atlas.tileCount = static_cast<int>(std::min<int64_t>(count, std::numeric_limits<int>::max()));
        }
        if (j.contains("collidable") && j["collidable"].is_array()) {
            atlas.collidable = j["collidable"].get<std::set<int>>();
        }
    } catch (const nlohmann::json::exception& e) {
        err = "invalid tileset json: " + jsonPath + ": " + e.what();
        return false;
    }
    destination = std::move(atlas);
    err.clear();
    return true;
}

bool LoadTilemapJson(const std::string& jsonPath, Map& destination, std::string& err) {
    std::ifstream ifs(Utf8Path(jsonPath));
    if (!ifs) { err = "cannot open tilemap json: " + jsonPath; return false; }
    Map out;
    try {
        nlohmann::json j;
        ifs >> j;
        out.width = j.value("width", 0);
        out.height = j.value("height", 0);
        out.tileWidth = j.value("tileWidth", 0);
        out.tileHeight = j.value("tileHeight", 0);
        if (out.width <= 0 || out.height <= 0) {
            err = "tilemap json requires positive dimensions: " + jsonPath;
            return false;
        }

        // TilemapSystem resolves the tileset name to its asset when loading the map.
        Tileset tileset;
        tileset.name = j.value("tileset", "");
        if (tileset.name.empty()) {
            err = "tilemap json missing tileset: " + jsonPath;
            return false;
        }
        out.tilesets.push_back(std::move(tileset));
        if (j.contains("layers") && j["layers"].is_array()) {
            for (const auto& value : j["layers"]) {
                Layer layer;
                layer.name = value.value("name", "Layer");
                layer.width = value.value("width", out.width);
                layer.height = value.value("height", out.height);
                layer.visible = value.value("visible", true);
                if (value.contains("gids") && value["gids"].is_array()) {
                    layer.gids = value["gids"].get<std::vector<int>>();
                }
                out.layers.push_back(std::move(layer));
            }
        }
    } catch (const nlohmann::json::exception& e) {
        err = "invalid tilemap json: " + jsonPath + ": " + e.what();
        return false;
    }
    destination = std::move(out);
    err.clear();
    return true;
}

} // namespace Tmx
