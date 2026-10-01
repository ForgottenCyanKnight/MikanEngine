#include "Animation/MMD/Model/VMDFile.h"
#include <iostream>
#include <fstream>
#include "Core/Utf8Path.h"
#include <cstring>
#include <algorithm>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif
#ifdef __ANDROID__
#include <SDL3/SDL.h>
#include <jni.h>
#endif

namespace mmd
{
    static std::string ShiftJIStoUTF8(const char* sjisStr, size_t maxLen)
    {
        if (!sjisStr || maxLen == 0)
            return "";

        size_t actualLen = strnlen(sjisStr, maxLen);
        if (actualLen == 0)
            return "";

#ifdef _WIN32
        int wideLen = MultiByteToWideChar(932, 0, sjisStr, static_cast<int>(actualLen), nullptr, 0);
        if (wideLen <= 0)
            return std::string(sjisStr, actualLen);

        std::vector<wchar_t> wideStr(wideLen);
        MultiByteToWideChar(932, 0, sjisStr, static_cast<int>(actualLen), wideStr.data(), wideLen);

        int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wideStr.data(), wideLen, nullptr, 0, nullptr, nullptr);
        if (utf8Len <= 0)
            return std::string(sjisStr, actualLen);

        std::vector<char> utf8Str(utf8Len);
        WideCharToMultiByte(CP_UTF8, 0, wideStr.data(), wideLen, utf8Str.data(), utf8Len, nullptr, nullptr);

        return std::string(utf8Str.data(), utf8Len);
#elif defined(__ANDROID__)
        auto* env=static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
        if(!env) return {};
        jclass cls=env->FindClass("java/lang/String");
        if(!cls) { env->ExceptionClear();return {}; }
        auto ctor=env->GetMethodID(cls,"<init>","([BLjava/lang/String;)V");
        auto getBytes=env->GetMethodID(cls,"getBytes","(Ljava/lang/String;)[B");
        jbyteArray bytes=env->NewByteArray(static_cast<jsize>(actualLen));
        env->SetByteArrayRegion(bytes,0,static_cast<jsize>(actualLen),reinterpret_cast<const jbyte*>(sjisStr));
        jstring cp=env->NewStringUTF("windows-31j"),utf=env->NewStringUTF("UTF-8");
        jobject string=env->NewObject(cls,ctor,bytes,cp);
        auto output=static_cast<jbyteArray>(string?env->CallObjectMethod(string,getBytes,utf):nullptr);
        std::string result;
        if(env->ExceptionCheck()) env->ExceptionClear();
        else if(output) { result.resize(env->GetArrayLength(output));env->GetByteArrayRegion(output,0,static_cast<jsize>(result.size()),reinterpret_cast<jbyte*>(result.data())); }
        if(output) env->DeleteLocalRef(output);
        if(string) env->DeleteLocalRef(string);
        env->DeleteLocalRef(bytes);env->DeleteLocalRef(cp);env->DeleteLocalRef(utf);env->DeleteLocalRef(cls);
        return result;
#else
        return std::string(sjisStr, actualLen);
#endif
    }

    VMDFile::VMDFile() : m_version(VMDVersion::V2_0) {}
    VMDFile::~VMDFile() {}

    void VMDFile::Destroy()
    {
        m_modelName.clear();
        m_motions.clear();
        m_morphs.clear();
        m_cameras.clear();
        m_lights.clear();
        m_iks.clear();
    }

    uint32_t VMDFile::GetMaxFrame() const
    {
        uint32_t maxFrame = 0;
        for (const auto& motion : m_motions)
            maxFrame = std::max(maxFrame, motion.frameNo);
        for (const auto& morph : m_morphs)
            maxFrame = std::max(maxFrame, morph.frameNo);
        for (const auto& camera : m_cameras)
            maxFrame = std::max(maxFrame, camera.frameNo);
        for (const auto& light : m_lights)
            maxFrame = std::max(maxFrame, light.frameNo);
        for (const auto& ik : m_iks)
            maxFrame = std::max(maxFrame, ik.frameNo);
        return maxFrame;
    }

    bool VMDFile::Load(const std::string& filename)
    {
        Destroy();
#ifdef __ANDROID__
        size_t byteCount=0;void* data=SDL_LoadFile(filename.c_str(),&byteCount);
        if(!data) return false;
        std::string payload(static_cast<const char*>(data),byteCount);SDL_free(data);
        std::istringstream file(payload,std::ios::binary);
#else
        std::ifstream file(Utf8Path(filename), std::ios::binary);
#endif
        if (!file) return false;
        file.seekg(0,std::ios::end);
        const auto size=file.tellg();
        file.seekg(0);
        auto available=[&](uint64_t bytes) {
            const auto pos=file.tellg();
            return file.good() && pos>=0 && pos<=size && bytes<=static_cast<uint64_t>(size-pos);
        };
        auto read=[&](auto& value) { return available(sizeof(value)) && Read(file,value); };
        auto name=[&](size_t n,std::string& out) {
            if(!available(n)) return false;
            std::vector<char> raw(n); file.read(raw.data(),n);
            out=ShiftJIStoUTF8(raw.data(),n);return file.good();
        };
        auto count=[&](uint32_t& out,size_t minimumBytes) {
            return read(out) && out<=10000000 && available(uint64_t(out)*minimumBytes);
        };
        char signature[30];
        if(!available(30)) return false; file.read(signature,30);
        if(std::memcmp(signature,"Vocaloid Motion Data 0002",24)==0) m_version=VMDVersion::V2_0;
        else if(std::memcmp(signature,"Vocaloid Motion Data file",24)==0) m_version=VMDVersion::V1_0;
        else return false;
        if(!name(m_version==VMDVersion::V2_0?20:10,m_modelName)) return false;
        uint32_t n;
        if(!count(n,111)) return false; m_motions.resize(n);
        for(auto& v:m_motions) {
            if(!name(15,v.boneName) || !read(v.frameNo) || !read(v.position)) return false;
            float q[4];if(!read(q)) return false;
            v.rotation=glm::quat(q[3],q[0],q[1],q[2]);
            if(!read(v.interpolation)) return false;
        }
        if(file.tellg()==size) return true;
        if(!count(n,23)) return false;m_morphs.resize(n);
        for(auto& v:m_morphs) if(!name(15,v.morphName)||!read(v.frameNo)||!read(v.weight)) return false;
        if(file.tellg()==size) return true;
        if(!count(n,61)) return false;m_cameras.resize(n);
        for(auto& v:m_cameras) {
            uint32_t fov;
            if(!read(v.frameNo)||!read(v.distance)||!read(v.position)||!read(v.rotation)||
               !read(v.interpolation)||!read(fov)||!read(v.isPerspective)) return false;
            v.fov=static_cast<float>(fov);
        }
        if(file.tellg()==size) return true;
        if(!count(n,28)) return false;m_lights.resize(n);
        for(auto& v:m_lights) if(!read(v.frameNo)||!read(v.color)||!read(v.direction)) return false;
        if(file.tellg()==size) return true;
        // Self-shadow frames precede the variable-length IK visibility section.
        if(!count(n,9)) return false;
        for(uint32_t i=0;i<n;++i) { uint32_t frame;uint8_t mode;float distance;
            if(!read(frame)||!read(mode)||!read(distance)) return false;
        }
        if(file.tellg()==size) return true;
        if(!count(n,9)) return false;
        for(uint32_t i=0;i<n;++i) {
            uint32_t frame,entries;uint8_t visible;
            if(!read(frame)||!read(visible)||!count(entries,21)) return false;
            for(uint32_t j=0;j<entries;++j) {
                VMDIK v;v.frameNo=frame;
                if(!name(20,v.ikName)||!read(v.enable)) return false;
                m_iks.push_back(std::move(v));
            }
        }
        return file.good();
    }
}
