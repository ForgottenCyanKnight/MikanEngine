#include "Platform/SystemFont.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb/stb_truetype.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>

static void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    try {
        Check(!std::filesystem::exists("engine/fonts/simhei.ttf"), "Run this test outside the engine asset directory");
        const auto* font = SystemFonts::GetChineseFont();
        Check(font && !font->bytes.empty(), "No system Chinese font available");
        Check(SystemFonts::GetChineseFont() == font, "Font data must remain stable for ImGui");
        const int offset = stbtt_GetFontOffsetForIndex(font->bytes.data(), font->faceIndex);
        Check(offset >= 0, "Invalid TTC face index");
        stbtt_fontinfo info{};
        Check(stbtt_InitFont(&info, font->bytes.data(), offset) != 0, "System font rejected by runtime parser");
        for (const int cp : {0x41, 0x4e2d, 0x6587, 0x6c49}) {
            Check(stbtt_FindGlyphIndex(&info, cp) != 0, "Missing required Latin/Chinese glyph");
            const float scale = stbtt_ScaleForPixelHeight(&info, 24);
            int width = 0, height = 0;
            auto* bitmap = stbtt_GetCodepointBitmap(&info, 0, scale, cp, &width, &height, nullptr, nullptr);
            Check(bitmap && width > 0 && height > 0, "Glyph rasterization failed");
            stbtt_FreeBitmap(bitmap, nullptr);
            stbtt_vertex* vertices = nullptr;
            Check(stbtt_GetCodepointShape(&info, cp, &vertices) > 0, "MSDF source outline missing");
            stbtt_FreeShape(&info, vertices);
            auto* sdf = stbtt_GetCodepointSDF(&info, scale, cp, 5, 128, 16, &width, &height, nullptr, nullptr);
            Check(sdf && width > 0 && height > 0, "SDF generation failed");
            stbtt_FreeSDF(sdf, nullptr);
        }
        std::cout << "PASS: system family=" << font->family << " face=" << font->faceIndex
                  << " bytes=" << font->bytes.size() << "; Chinese bitmap/SDF/outline without engine font assets\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
