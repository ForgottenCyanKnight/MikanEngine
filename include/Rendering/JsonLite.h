// JsonLite.h —— 轻量 JSON 解析（DOM 模型），零依赖（仅标准库）。
// 用途：ModelLoader 手动解析 glTF 纹理引用（assimp 不认 KHR_texture_basisu/.ktx2）。
// 仅支持标准 JSON（对象/数组/字符串/数字/布尔/null），不做任何校验外的容错。
#pragma once

#include <string>
#include <vector>
#include <utility>
#include <cctype>
#include <cstdlib>

namespace JsonLite {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    bool IsValid() const { return type != Type::Null; }
    size_t Size() const { return type == Type::Array ? arr.size() : (type == Type::Object ? obj.size() : 0); }
    const Value* Get(const char* key) const {
        for (const auto& kv : obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    const Value* At(size_t i) const { return i < arr.size() ? &arr[i] : nullptr; }
};

class Parser {
public:
    explicit Parser(const std::string& text) : m_s(text), m_i(0) {}

    bool Parse(Value& out) {
        if (!ParseValue(out)) return false;
        SkipWs();
        return m_i >= m_s.size();
    }

private:
    const std::string& m_s;
    size_t m_i = 0;

    void SkipWs() {
        while (m_i < m_s.size() && (m_s[m_i] == ' ' || m_s[m_i] == '\t' || m_s[m_i] == '\n' || m_s[m_i] == '\r'))
            ++m_i;
    }

    bool ParseValue(Value& v) {
        SkipWs();
        if (m_i >= m_s.size()) return false;
        const char c = m_s[m_i];
        if (c == '{') return ParseObject(v);
        if (c == '[') return ParseArray(v);
        if (c == '"') return ParseString(v);
        if (c == 't' || c == 'f') return ParseBool(v);
        if (c == 'n') { v.type = Value::Type::Null; m_i += 4; return true; }
        return ParseNumber(v);
    }

    bool ParseObject(Value& v) {
        v.type = Value::Type::Object;
        ++m_i; // '{'
        SkipWs();
        if (m_i < m_s.size() && m_s[m_i] == '}') { ++m_i; return true; }
        while (m_i < m_s.size()) {
            SkipWs();
            if (m_i >= m_s.size() || m_s[m_i] != '"') return false;
            std::string key;
            if (!ParseStringRaw(key)) return false;
            SkipWs();
            if (m_i >= m_s.size() || m_s[m_i] != ':') return false;
            ++m_i; // ':'
            Value val;
            if (!ParseValue(val)) return false;
            v.obj.emplace_back(std::move(key), std::move(val));
            SkipWs();
            if (m_i >= m_s.size()) return false;
            const char c = m_s[m_i];
            if (c == ',') { ++m_i; continue; }
            if (c == '}') { ++m_i; return true; }
            return false;
        }
        return false;
    }

    bool ParseArray(Value& v) {
        v.type = Value::Type::Array;
        ++m_i; // '['
        SkipWs();
        if (m_i < m_s.size() && m_s[m_i] == ']') { ++m_i; return true; }
        while (m_i < m_s.size()) {
            Value val;
            if (!ParseValue(val)) return false;
            v.arr.push_back(std::move(val));
            SkipWs();
            if (m_i >= m_s.size()) return false;
            const char c = m_s[m_i];
            if (c == ',') { ++m_i; continue; }
            if (c == ']') { ++m_i; return true; }
            return false;
        }
        return false;
    }

    bool ParseString(Value& v) {
        v.type = Value::Type::String;
        return ParseStringRaw(v.str);
    }

    bool ParseStringRaw(std::string& out) {
        if (m_i >= m_s.size() || m_s[m_i] != '"') return false;
        ++m_i;
        out.clear();
        while (m_i < m_s.size()) {
            const char c = m_s[m_i];
            if (c == '"') { ++m_i; return true; }
            if (c == '\\') {
                ++m_i;
                if (m_i >= m_s.size()) return false;
                const char e = m_s[m_i++];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        if (m_i + 4 > m_s.size()) return false;
                        unsigned code = 0;
                        for (int k = 0; k < 4; ++k) {
                            const char h = m_s[m_i + k];
                            code <<= 4;
                            if (h >= '0' && h <= '9') code |= (unsigned)(h - '0');
                            else if (h >= 'a' && h <= 'f') code |= (unsigned)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') code |= (unsigned)(h - 'A' + 10);
                            else return false;
                        }
                        m_i += 4;
                        // 只支持 BMP 字符（\uXXXX）；代理对原样双字节追加（glTF 纹理路径通常无非 ASCII）
                        out += (char)((code >> 8) & 0xFF);
                        out += (char)(code & 0xFF);
                        break;
                    }
                    default: return false;
                }
            } else {
                out += c;
                ++m_i;
            }
        }
        return false;
    }

    bool ParseBool(Value& v) {
        v.type = Value::Type::Bool;
        if (m_s.compare(m_i, 4, "true") == 0) { v.b = true; m_i += 4; return true; }
        if (m_s.compare(m_i, 5, "false") == 0) { v.b = false; m_i += 5; return true; }
        return false;
    }

    bool ParseNumber(Value& v) {
        v.type = Value::Type::Number;
        const size_t start = m_i;
        if (m_i < m_s.size() && (m_s[m_i] == '-' || m_s[m_i] == '+')) ++m_i;
        bool hasDigit = false;
        while (m_i < m_s.size() && std::isdigit((unsigned char)m_s[m_i])) { ++m_i; hasDigit = true; }
        if (m_i < m_s.size() && m_s[m_i] == '.') {
            ++m_i;
            while (m_i < m_s.size() && std::isdigit((unsigned char)m_s[m_i])) { ++m_i; hasDigit = true; }
        }
        if (m_i < m_s.size() && (m_s[m_i] == 'e' || m_s[m_i] == 'E')) {
            ++m_i;
            if (m_i < m_s.size() && (m_s[m_i] == '-' || m_s[m_i] == '+')) ++m_i;
            while (m_i < m_s.size() && std::isdigit((unsigned char)m_s[m_i])) ++m_i;
        }
        if (!hasDigit) return false;
        v.num = std::strtod(m_s.c_str() + start, nullptr);
        return true;
    }
};

inline bool Parse(const std::string& text, Value& out) {
    Parser p(text);
    return p.Parse(out);
}

} // namespace JsonLite
