#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <iostream>
#include <filesystem>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/mat4x4.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace mmd
{
    template <size_t Size>
    class MMDFileString
    {
    public:
        MMDFileString() { m_str[0] = '\0'; }
        MMDFileString(const char* str) { strncpy(m_str, str, Size); m_str[Size - 1] = '\0'; }

        const char* c_str() const { return m_str; }
        std::string to_string() const { return m_str; }

        void clear() { m_str[0] = '\0'; }

        bool operator==(const MMDFileString& rhs) const { return strcmp(m_str, rhs.m_str) == 0; }
        bool operator!=(const MMDFileString& rhs) const { return strcmp(m_str, rhs.m_str) != 0; }

    private:
        char m_str[Size];
    };

    template <typename T>
    bool Read(std::istream& file, T& data)
    {
        file.read(reinterpret_cast<char*>(&data), sizeof(T));
        return file.good();
    }

    template <typename T>
    bool Read(std::istream& file, T* data, size_t count)
    {
        file.read(reinterpret_cast<char*>(data), sizeof(T) * count);
        return file.good();
    }

    inline bool ReadString(std::istream& file, std::string& str, int size)
    {
        std::vector<char> buffer(size);
        file.read(buffer.data(), size);
        if (!file.good()) return false;
        str.assign(buffer.data(), size);
        return true;
    }

    inline bool ReadPMXString(std::istream& file, std::string& str, uint8_t encode)
    {
        int32_t size;
        if (!Read(file, size)) return false;
        if (size < 0 || size > 16*1024*1024 || (encode==0 && (size&1))) return false;
        if (size == 0) { str.clear(); return true; }

        if (encode == 0) {
            // UTF-16: size is in bytes, need to convert to char16_t count
            int32_t charCount = size / 2;
            std::vector<char16_t> buffer(charCount);
            file.read(reinterpret_cast<char*>(buffer.data()), size);
            if (!file.good()) return false;
            str.clear();
            str.reserve(charCount);
            for (int i = 0; i < charCount; i++) {
                uint32_t c = buffer[i];
                if(c>=0xD800 && c<=0xDBFF) {
                    if(i+1>=charCount || buffer[i+1]<0xDC00 || buffer[i+1]>0xDFFF) return false;
                    c=0x10000+((c-0xD800)<<10)+(buffer[++i]-0xDC00);
                } else if(c>=0xDC00 && c<=0xDFFF) return false;
                if (c < 0x80) {
                    str.push_back(static_cast<char>(c));
                } else if (c < 0x800) {
                    str.push_back(static_cast<char>((c >> 6) | 0xC0));
                    str.push_back(static_cast<char>((c & 0x3F) | 0x80));
                } else if(c<0x10000) {
                    str.push_back(static_cast<char>((c >> 12) | 0xE0));
                    str.push_back(static_cast<char>(((c >> 6) & 0x3F) | 0x80));
                    str.push_back(static_cast<char>((c & 0x3F) | 0x80));
                } else {
                    str.push_back(static_cast<char>((c>>18)|0xF0));
                    str.push_back(static_cast<char>(((c>>12)&0x3F)|0x80));
                    str.push_back(static_cast<char>(((c>>6)&0x3F)|0x80));
                    str.push_back(static_cast<char>((c&0x3F)|0x80));
                }
            }
        } else if (encode == 1) {
            // UTF-8: size is in bytes
            std::vector<char> buffer(size);
            file.read(buffer.data(), size);
            if (!file.good()) return false;
            str.assign(buffer.data(), size);
        } else {
            return false;
        }
        return true;
    }

    template <typename T>
    T ReadPMXValue(std::istream& file, uint8_t indexSize)
    {
        T value = 0;
        if (indexSize == 1) {
            int8_t v=0; Read(file, v); value = static_cast<T>(v);
        } else if (indexSize == 2) {
            int16_t v=0; Read(file, v); value = static_cast<T>(v);
        } else if (indexSize == 4) {
            int32_t v=0; Read(file, v); value = static_cast<T>(v);
        }
        return value;
    }

    template <typename T>
    T ReadPMXValueU(std::istream& file, uint8_t indexSize)
    {
        T value = 0;
        if (indexSize == 1) {
            uint8_t v; Read(file, v); value = static_cast<T>(v);
        } else if (indexSize == 2) {
            uint16_t v; Read(file, v); value = static_cast<T>(v);
        } else if (indexSize == 4) {
            uint32_t v; Read(file, v); value = static_cast<T>(v);
        }
        return value;
    }
}
