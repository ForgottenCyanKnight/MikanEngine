// Native renderer for Pixel Hop Twins by stm1978, MIT / CC0.
#pragma once

#include "NativeModel.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class Renderer2D;

namespace PixelHop {

class NativeRender {
public:
    bool Initialize();
    void Clear();
    void Render(Renderer2D& renderer, const Model& model, int width, int height, double wallMilliseconds);
    void DrawLabel(Renderer2D& renderer, const std::string& text, glm::vec2 baseline,
                   int fontSize, glm::vec4 color, int layer);
    uint32_t LastQuadCount() const { return m_lastQuadCount; }

private:
    struct ColorMask {
        std::vector<uint8_t> alpha;
        std::vector<uint8_t> premul;
    };
    struct Glyph {
        double x = 0, y = 0, w = 0, h = 0;
        double offsetX = 0, offsetY = 0, advance = 0;
        std::vector<uint8_t> alpha;
        std::unordered_map<std::string, ColorMask> colors;
    };
    struct Font {
        std::unordered_map<std::string, Glyph> glyphs;
        std::unordered_map<std::string, std::vector<Glyph>> phases;
    };
    using Matrix = std::vector<std::vector<int>>;

    void EmitRect(double x, double y, double w, double h, glm::vec4 color, int coverage = 255);
    void Rect(double x, double y, double w, double h, glm::vec4 color);
    void Pixels(const std::string& name, double x, double y, double w = 32,
                double h = 32, bool flip = false);
    void Text(const std::string& text, double x, double baseline, int fontSize,
              glm::vec4 color, const std::string& alignment = "left");
    double TextWidth(const std::string& text, int fontSize) const;
    void Mountains(double offsetX, double baseY, double spacing, double height, glm::vec4 color);
    void Aura(double x, double y, double rx, double ry, glm::vec4 color);
    void World(const Model& model, double wallMilliseconds);
    void Hud(const Model& model);
    void Overlays(const Model& model);
    void EmitCanvas();

    Renderer2D* m_renderer = nullptr;
    std::vector<glm::vec4> m_palette;
    std::unordered_map<std::string, Matrix> m_matrices;
    std::unordered_map<int, Font> m_fonts;
    double m_atlasWidth = 1;
    double m_atlasHeight = 1;
    double m_scale = 1;
    glm::dvec2 m_offset{0};
    int m_layer = 0;
    bool m_ready = false;
    bool m_rasterizing = false;
    std::vector<uint32_t> m_pixels;
    uint32_t m_lastQuadCount = 0;
};

} // namespace PixelHop
