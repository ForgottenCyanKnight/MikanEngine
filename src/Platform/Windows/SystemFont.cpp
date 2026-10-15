#include "Platform/SystemFont.h"
#ifdef _WIN32
#include <windows.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <limits>

namespace SystemFonts {
namespace {
using Microsoft::WRL::ComPtr;

std::string FamilyName(IDWriteFontFamily* family) {
    ComPtr<IDWriteLocalizedStrings> names;
    if (FAILED(family->GetFamilyNames(names.GetAddressOf()))) return {};
    UINT32 index = 0, length = 0;
    BOOL exists = FALSE;
    if (FAILED(names->FindLocaleName(L"en-us", &index, &exists)) || !exists) index = 0;
    if (FAILED(names->GetStringLength(index, &length))) return {};
    std::wstring name(length + 1, L'\0');
    if (FAILED(names->GetString(index, name.data(), length + 1))) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, name.data(), int(length), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, name.data(), int(length), result.data(), size, nullptr, nullptr);
    return result;
}

bool LoadFamily(IDWriteFontCollection* collection, UINT32 index, FontData& output) {
    ComPtr<IDWriteFontFamily> family;
    ComPtr<IDWriteFont> font;
    ComPtr<IDWriteFontFace> face;
    if (FAILED(collection->GetFontFamily(index, family.GetAddressOf())) ||
        FAILED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                           DWRITE_FONT_STYLE_NORMAL, font.GetAddressOf())) ||
        FAILED(font->CreateFontFace(face.GetAddressOf()))) return false;

    // Require Latin and representative Chinese glyphs, rather than trusting a family name.
    const UINT32 characters[] = {0x41, 0x4e2d, 0x6587, 0x6c49};
    UINT16 glyphs[4]{};
    if (FAILED(face->GetGlyphIndices(characters, 4, glyphs))) return false;
    for (auto glyph : glyphs) if (!glyph) return false;
    const auto type = face->GetType();
    if (type != DWRITE_FONT_FACE_TYPE_TRUETYPE && type != DWRITE_FONT_FACE_TYPE_TRUETYPE_COLLECTION &&
        type != DWRITE_FONT_FACE_TYPE_CFF) return false;

    UINT32 count = 0;
    ComPtr<IDWriteFontFile> file;
    if (FAILED(face->GetFiles(&count, nullptr)) || count != 1 ||
        FAILED(face->GetFiles(&count, file.GetAddressOf()))) return false;
    const void* key = nullptr;
    UINT32 keySize = 0;
    ComPtr<IDWriteFontFileLoader> loader;
    ComPtr<IDWriteFontFileStream> stream;
    if (FAILED(file->GetReferenceKey(&key, &keySize)) ||
        FAILED(file->GetLoader(loader.GetAddressOf())) ||
        FAILED(loader->CreateStreamFromKey(key, keySize, stream.GetAddressOf()))) return false;
    UINT64 size = 0;
    if (FAILED(stream->GetFileSize(&size)) || size == 0 || size > UINT64((std::numeric_limits<int>::max)()))
        return false;
    const void* fragment = nullptr;
    void* context = nullptr;
    if (FAILED(stream->ReadFileFragment(&fragment, 0, size, &context))) return false;
    try {
        const auto* bytes = static_cast<const std::uint8_t*>(fragment);
        output.bytes.assign(bytes, bytes + size);
    } catch (...) {
        stream->ReleaseFileFragment(context);
        throw;
    }
    stream->ReleaseFileFragment(context);
    output.faceIndex = int(face->GetIndex());
    output.family = FamilyName(family.Get());
    return true;
}

FontData FindChineseFont() {
    ComPtr<IDWriteFactory> factory;
    ComPtr<IDWriteFontCollection> collection;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                  reinterpret_cast<IUnknown**>(factory.GetAddressOf()))) ||
        FAILED(factory->GetSystemFontCollection(collection.GetAddressOf(), FALSE))) return {};
    FontData result;
    const wchar_t* preferred[] = {L"Microsoft YaHei UI", L"Microsoft YaHei", L"DengXian",
        L"SimHei", L"SimSun", L"Noto Sans CJK SC", L"Source Han Sans SC"};
    for (const auto* name : preferred) {
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (SUCCEEDED(collection->FindFamilyName(name, &index, &exists)) && exists &&
            LoadFamily(collection.Get(), index, result)) return result;
    }
    for (UINT32 index = 0; index < collection->GetFontFamilyCount(); ++index)
        if (LoadFamily(collection.Get(), index, result)) return result;
    return {};
}
}

const FontData* GetChineseFont() {
    static const FontData data = FindChineseFont();
    return data.bytes.empty() ? nullptr : &data;
}
}
#endif
