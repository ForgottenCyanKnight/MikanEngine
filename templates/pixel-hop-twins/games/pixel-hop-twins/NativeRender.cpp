// Port of the original Canvas drawing commands. See licenses/MIT_LICENSE.txt and licenses/UPSTREAM.md.
#include "NativeRender.h"

#include "Core/Log.h"
#include "Core/ProjectManager.h"
#include "Rendering/Renderer2D.h"
#include "Rendering/TexturePool.h"
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <iomanip>
#include <limits>
#include <utility>
#include <sstream>
#include <stdexcept>

// The source rasterizer evaluates each float operation separately. Keep the
// same rounding boundaries on compilers that can fuse multiply-add operations.
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

// Trapezoid coverage compatible with Skia SkScan_AAAPath.cpp.
// Original portions Copyright 2016 The Android Open Source Project,
// under the BSD-style license retained in licenses/SKIA_LICENSE.txt.
// https://github.com/google/skia/blob/main/LICENSE

namespace PixelHop::Coverage {
struct Pixel {
    uint8_t alpha = 0;
    bool fastInterpolation = false;
    bool additive = false;
};

class Raster {
public:
    using Blitter = std::function<void(int y, int x, int alpha, bool fast)>;
    explicit Raster(bool smallMask = false, Blitter blitter = {})
        : m_smallMask(smallMask), m_blitter(std::move(blitter)) {}
    std::vector<Pixel> pixels = std::vector<Pixel>(640 * 400);

    void AddPixel(int y, int x, int alpha) { Put(y, x, alpha, true, false); }

    void FinishRow(int y) {
        if (m_smallMask) return;
        if (y < 0 || y >= 400) return;
        for (int x = 0; x < 640; ++x) {
            auto& pixel = pixels[y * 640 + x];
            if (pixel.additive) {
                if (pixel.alpha > 247) pixel.alpha = 255;
                else if (pixel.alpha < 8) pixel.alpha = 0;
                if (m_blitter && pixel.alpha) m_blitter(y, x, pixel.alpha, false);
            }
            if (m_blitter) pixel = {};
        }
    }

    void Trapezoid(int y, int64_t ul, int64_t ur, int64_t ll, int64_t lr,
                   int64_t lDY, int64_t rDY, int fullAlpha, bool forceRle) {
        if (ul > ur) return;
        if (ll > lr) {
            const int64_t intersection = (std::max(std::min(ul, ll), std::min(ur, lr)) +
                                          std::min(std::max(ul, ll), std::max(ur, lr))) / 2;
            ll = lr = intersection;
        }
        if (ul == ur && ll == lr) return;
        if (ul > ll) std::swap(ul, ll);
        if (ur > lr) std::swap(ur, lr);
        const int64_t joinLeft = Ceil(ll) * One;
        const int64_t joinRight = Floor(ur) * One;
        if (joinLeft > joinRight) {
            Aaa(y, ul, ur, ll, lr, lDY, rDY, fullAlpha, forceRle);
            return;
        }
        if (ul < joinLeft) {
            const int length = Ceil(joinLeft - ul);
            if (length == 1) {
                Single(y, Floor(ul), TrapezoidAlpha(joinLeft - ul, joinLeft - ll), fullAlpha, forceRle);
            } else if (length == 2) {
                const int64_t first = joinLeft - One - ul;
                const int64_t second = ll - ul - first;
                Two(y, Floor(ul), TriangleAlpha(first, lDY), fullAlpha - TriangleAlpha(second, lDY),
                    fullAlpha, forceRle);
            } else {
                Aaa(y, ul, joinLeft, ll, joinLeft, lDY, INT32_MAX, fullAlpha, forceRle);
            }
        }
        if (joinLeft < joinRight) {
            for (int x = Floor(joinLeft); x < Floor(joinRight); ++x) {
                Put(y, x, fullAlpha, fullAlpha != 255 || forceRle, false);
            }
        }
        if (lr > joinRight) {
            const int length = Ceil(lr - joinRight);
            if (length == 1) {
                Single(y, Floor(joinRight), TrapezoidAlpha(ur - joinRight, lr - joinRight), fullAlpha, forceRle);
            } else if (length == 2) {
                const int64_t first = joinRight + One - ur;
                const int64_t second = lr - ur - first;
                Two(y, Floor(joinRight), fullAlpha - TriangleAlpha(first, rDY), TriangleAlpha(second, rDY),
                    fullAlpha, forceRle);
            } else {
                Aaa(y, joinRight, ur, joinRight, lr, INT32_MAX, rDY, fullAlpha, forceRle);
            }
        }
    }

private:
    bool m_smallMask = false;
    Blitter m_blitter;
    static constexpr int64_t One = int64_t{1} << 16;
    static int Floor(int64_t value) { return static_cast<int>(value >> 16); }
    static int Ceil(int64_t value) { return static_cast<int>((value + One - 1) >> 16); }
    static int64_t Mul(int64_t a, int64_t b) { return (a * b) >> 16; }
    static int TrapezoidAlpha(int64_t a, int64_t b) { return static_cast<uint8_t>(((a + b) / 2) >> 8); }
    static int TriangleAlpha(int64_t a, int64_t b) {
        return static_cast<int>((((a >> 11) * (a >> 11) * (b >> 11)) >> 8) & 255);
    }
    static int Partial(int alpha, int fullAlpha) { return (alpha * fullAlpha) >> 8; }

    void Put(int y, int x, int alpha, bool additive, bool fast) {
        if (x < 0 || x >= 640 || y < 0 || y >= 400) return;
        auto& pixel = pixels[y * 640 + x];
        if (additive) {
            pixel.alpha = static_cast<uint8_t>(std::min(255, static_cast<int>(pixel.alpha) + alpha));
            pixel.additive = true;
            pixel.fastInterpolation = false;
        } else {
            if (m_blitter) {
                m_blitter(y, x, alpha, fast);
                return;
            }
            pixel.alpha = static_cast<uint8_t>(alpha);
            pixel.additive = false;
            pixel.fastInterpolation = fast;
        }
    }
    void Single(int y, int x, int alpha, int fullAlpha, bool forceRle) {
        const bool additive = fullAlpha != 255 || forceRle;
        Put(y, x, additive ? Partial(alpha, fullAlpha) : alpha, additive, false);
    }
    void Two(int y, int x, int first, int second, int fullAlpha, bool forceRle) {
        const bool additive = m_smallMask || fullAlpha != 255 || forceRle;
        Put(y, x, static_cast<uint8_t>(first), additive, !additive);
        Put(y, x + 1, static_cast<uint8_t>(second), additive, !additive);
    }
    static void Above(std::vector<int>& result, int64_t left, int64_t right,
                      int64_t dY, int fullAlpha) {
        const int count = Ceil(right);
        result.assign(count, 0);
        if (count == 0) return;
        if (count == 1) {
            result[0] = Partial(static_cast<uint8_t>(((int64_t{count} << 17) - left - right) >> 9), fullAlpha);
            return;
        }
        const int64_t first = One - left;
        const int64_t last = right - (count - 1) * One;
        const int64_t firstHeight = Mul(first, dY);
        result[0] = static_cast<uint8_t>(Mul(first, firstHeight) >> 9);
        int64_t alpha16 = std::min<int64_t>(INT32_MAX, firstHeight + (dY >> 1));
        for (int i = 1; i < count - 1; ++i) {
            result[i] = static_cast<uint8_t>(alpha16 >> 8);
            alpha16 = std::min<int64_t>(INT32_MAX, alpha16 + dY);
        }
        result[count - 1] = static_cast<uint8_t>(fullAlpha - TriangleAlpha(last, dY));
    }
    static void Below(std::vector<int>& result, int64_t left, int64_t right,
                      int64_t dY, int fullAlpha) {
        const int count = Ceil(right);
        result.assign(count, 0);
        if (count == 0) return;
        if (count == 1) {
            result[0] = Partial(TrapezoidAlpha(left, right), fullAlpha);
            return;
        }
        const int64_t first = One - left;
        const int64_t last = right - (count - 1) * One;
        const int64_t lastHeight = Mul(last, dY);
        result[count - 1] = static_cast<uint8_t>(Mul(last, lastHeight) >> 9);
        int64_t alpha16 = std::min<int64_t>(INT32_MAX, lastHeight + (dY >> 1));
        for (int i = count - 2; i > 0; --i) {
            result[i] = static_cast<uint8_t>((alpha16 >> 8) & 255);
            alpha16 = std::min<int64_t>(INT32_MAX, alpha16 + dY);
        }
        result[0] = static_cast<uint8_t>(fullAlpha - TriangleAlpha(first, dY));
    }
    void Aaa(int y, int64_t ul, int64_t ur, int64_t ll, int64_t lr,
             int64_t lDY, int64_t rDY, int fullAlpha, bool forceRle) {
        const int left = Floor(ul), right = Ceil(lr), length = right - left;
        if (length <= 0) return;
        if (length == 1) {
            Single(y, left, TrapezoidAlpha(ur - ul, lr - ll), fullAlpha, forceRle);
            return;
        }
        std::vector<int> alphas(length, fullAlpha), temporary;
        const int upperLeft = Floor(ul), lowerLeft = Ceil(ll);
        if (upperLeft + 2 == lowerLeft) {
            const int64_t first = upperLeft * One + One - ul;
            const int64_t second = ll - ul - first;
            alphas[0] = std::max(0, alphas[0] - (fullAlpha - TriangleAlpha(first, lDY)));
            alphas[1] = std::max(0, alphas[1] - TriangleAlpha(second, lDY));
        } else {
            Below(temporary, ul - upperLeft * One, ll - upperLeft * One, lDY, fullAlpha);
            for (int x = upperLeft; x < lowerLeft; ++x) {
                alphas[x - left] = std::max(0, alphas[x - left] - temporary[x - upperLeft]);
            }
        }
        const int upperRight = Floor(ur), lowerRight = Ceil(lr);
        if (upperRight + 2 == lowerRight) {
            const int64_t first = upperRight * One + One - ur;
            const int64_t second = lr - ur - first;
            alphas[length - 2] = std::max(0, alphas[length - 2] - TriangleAlpha(first, rDY));
            alphas[length - 1] = std::max(0, alphas[length - 1] - (fullAlpha - TriangleAlpha(second, rDY)));
        } else {
            Above(temporary, ur - upperRight * One, lr - upperRight * One, rDY, fullAlpha);
            for (int x = upperRight; x < lowerRight; ++x) {
                alphas[x - left] = std::max(0, alphas[x - left] - temporary[x - upperRight]);
            }
        }
        for (int i = 0; i < length; ++i) {
            Put(y, left + i, alphas[i], m_smallMask || fullAlpha != 255 || forceRle, false);
        }
    }
};
} // namespace PixelHop::Coverage

// Oval/conic and analytic quadratic edges compatible with Skia.
// Original portions Copyright 2006 The Android Open Source Project.
// Used under the Skia BSD license, retained as licenses/SKIA_LICENSE.txt.


namespace PixelHop::CurveCoverage {

struct Point {
    float x = 0;
    float y = 0;
};

using Quad = std::array<Point, 3>;

struct Edge {
    int64_t upperX = 0;
    int64_t upperY = 0;
    int64_t lowerX = 0;
    int64_t lowerY = 0;
    int64_t dx = 0;
    int64_t dy = 0;
    int winding = 1;
};

inline int64_t Inverse(int64_t value) {
    if (value == 0) {
        return INT32_MAX;
    }
    const int64_t inverse = (int64_t{1} << 22) / std::abs(value);
    return value < 0 ? -inverse : inverse;
}

inline int64_t Divide(int64_t numerator, int64_t denominator) {
    if (denominator == 0) {
        return INT32_MAX;
    }
    if (std::abs(denominator) >= 8 && std::abs(denominator) < 1024 &&
        std::abs(numerator) < 4096) {
        return (numerator * Inverse(denominator)) >> 6;
    }
    return (numerator * 65536) / denominator;
}

inline int64_t SnapY(int64_t y) {
    return ((y + 8192) >> 14) << 14;
}

inline int64_t Mul(int64_t first, int64_t second) {
    return (first * second) >> 16;
}

inline std::vector<Quad> EllipseQuads(double centerX, double centerY,
                                    double radiusX, double radiusY) {
    const float cx = static_cast<float>(centerX);
    const float cy = static_cast<float>(centerY);
    const float rx = static_cast<float>(radiusX);
    const float ry = static_cast<float>(radiusY);
    const std::array<Point, 4> ends{{
        {cx + rx, cy},
        {cx, cy + ry},
        {cx - rx, cy},
        {cx, cy - ry}
    }};
    const std::array<Point, 4> controls{{
        {cx + rx, cy + ry},
        {cx - rx, cy + ry},
        {cx - rx, cy - ry},
        {cx + rx, cy - ry}
    }};
    constexpr float weight = 0.7071067811865475244f;
    const float scale = 1.0f / (1.0f + weight);
    std::vector<Quad> output;
    for (int quadrant = 0; quadrant < 4; ++quadrant) {
        const auto first = ends[quadrant];
        const auto control = controls[quadrant];
        const auto last = ends[(quadrant + 1) % 4];
        const Point firstTerm{first.x * scale, first.y * scale};
        const Point controlTerm{
            control.x * (weight * scale),
            control.y * (weight * scale)
        };
        const Point lastTerm{last.x * scale, last.y * scale};
        const Point firstControl{
            firstTerm.x + controlTerm.x,
            firstTerm.y + controlTerm.y
        };
        const Point midpoint{
            0.5f * firstTerm.x + controlTerm.x + 0.5f * lastTerm.x,
            0.5f * firstTerm.y + controlTerm.y + 0.5f * lastTerm.y
        };
        const Point lastControl{
            controlTerm.x + lastTerm.x,
            controlTerm.y + lastTerm.y
        };
        const float errorScale = (weight - 1) / (4 * (weight + 1));
        float error = std::hypot(
            errorScale * (first.x - 2 * control.x + last.x),
            errorScale * (first.y - 2 * control.y + last.y)
        );
        int subdivisions = 0;
        while (error > 0.25f && subdivisions < 5) {
            error *= 0.25f;
            ++subdivisions;
        }
        // The game's 13.8 by 5 ellipse needs one split per conic.
        // Smaller conics can already meet the source's quarter-pixel tolerance.
        if (subdivisions == 0) {
            output.push_back({first, control, last});
        } else {
            output.push_back({first, firstControl, midpoint});
            output.push_back({midpoint, lastControl, last});
        }
    }
    return output;
}

class Quadratic {
public:
    Edge current;
    int count = 0;
    int curveShift = 0;
    int64_t qx = 0;
    int64_t qy = 0;
    int64_t qdx = 0;
    int64_t qdy = 0;
    int64_t qddx = 0;
    int64_t qddy = 0;
    int64_t lastX = 0;
    int64_t lastY = 0;
    int64_t snappedX = 0;
    int64_t snappedY = 0;

    bool Initialize(Quad points) {
        int64_t x0 = static_cast<int>(points[0].x * 256.0f);
        int64_t y0 = static_cast<int>(points[0].y * 256.0f);
        const int64_t x1 = static_cast<int>(points[1].x * 256.0f);
        const int64_t y1 = static_cast<int>(points[1].y * 256.0f);
        int64_t x2 = static_cast<int>(points[2].x * 256.0f);
        int64_t y2 = static_cast<int>(points[2].y * 256.0f);
        current.winding = 1;
        if (y0 > y2) {
            std::swap(x0, x2);
            std::swap(y0, y2);
            current.winding = -1;
        }
        if (((y0 + 32) >> 6) == ((y2 + 32) >> 6)) {
            return false;
        }
        const int64_t dx = (2 * x1 - x0 - x2) >> 2;
        const int64_t dy = (2 * y1 - y0 - y2) >> 2;
        const int64_t absoluteX = std::abs(dx);
        const int64_t absoluteY = std::abs(dy);
        int64_t distance = (
            std::max(absoluteX, absoluteY) +
            (std::min(absoluteX, absoluteY) >> 1) + 16
        ) >> 5;
        int bits = 0;
        while (distance != 0) {
            ++bits;
            distance >>= 1;
        }
        const int shift = std::clamp(bits >> 1, 1, 6);
        curveShift = shift - 1;
        count = 1 << shift;

        // Coefficients are temporarily halved to preserve the source's fixed
        // point range, then rescaled by the forward-difference step below.
        int64_t coefficientA = (x0 - 2 * x1 + x2) * 512;
        int64_t coefficientB = (x1 - x0) * 1024;
        qx = x0 * 1024;
        qdx = coefficientB + (coefficientA >> shift);
        qddx = coefficientA >> (shift - 1);
        coefficientA = (y0 - 2 * y1 + y2) * 512;
        coefficientB = (y1 - y0) * 1024;
        qy = y0 * 1024;
        qdy = coefficientB + (coefficientA >> shift);
        qddy = coefficientA >> (shift - 1);
        lastX = x2 * 1024;
        lastY = y2 * 1024;
        qx >>= 2;
        qy >>= 2;
        qdx >>= 2;
        qdy >>= 2;
        qddx >>= 2;
        qddy >>= 2;
        lastX >>= 2;
        lastY >>= 2;
        qy = SnapY(qy);
        lastY = SnapY(lastY);
        snappedX = qx;
        snappedY = qy;
        return Next();
    }

    bool Next() {
        if (count <= 0) {
            return false;
        }
        int64_t oldX = qx;
        int64_t oldY = qy;
        int64_t dx = qdx;
        int64_t dy = qdy;
        int64_t newX = oldX;
        int64_t newY = oldY;
        int64_t newSnappedX = snappedX;
        int64_t newSnappedY = snappedY;
        bool success = false;
        do {
            int64_t slope = INT32_MAX;
            if (--count > 0) {
                newX = oldX + (dx >> curveShift);
                newY = oldY + (dy >> curveShift);
                if (std::abs(dy >> curveShift) >= 2 * 65536 &&
                    std::abs(dy) * 64 > std::abs(dx)) {
                    const int64_t differenceY = (newY - snappedY) >> 10;
                    if (differenceY != 0) {
                        slope = Divide((newX - snappedX) >> 10, differenceY);
                    }
                    newSnappedY = std::min(lastY, ((newY + 32768) >> 16) << 16);
                    newSnappedX = newX - Mul(slope, newY - newSnappedY);
                } else {
                    newSnappedY = std::min(lastY, SnapY(newY));
                    newSnappedX = newX;
                    const int64_t differenceY = (newSnappedY - snappedY) >> 10;
                    if (differenceY != 0) {
                        slope = Divide((newX - snappedX) >> 10, differenceY);
                    }
                }
                dx += qddx;
                dy += qddy;
            } else {
                newX = lastX;
                newY = lastY;
                newSnappedX = newX;
                newSnappedY = newY;
                const int64_t differenceY = (newY - snappedY) >> 10;
                if (differenceY != 0) {
                    slope = Divide((newX - snappedX) >> 10, differenceY);
                }
            }
            if (slope < INT32_MAX && ((newSnappedY - snappedY) >> 10) != 0) {
                const int64_t absoluteSlope = std::abs(slope >> 10);
                current.upperX = snappedX;
                current.upperY = snappedY;
                current.lowerX = newSnappedX;
                current.lowerY = newSnappedY;
                current.dx = slope;
                if (newSnappedX == snappedX || slope == 0) {
                    current.dy = INT32_MAX;
                } else if (absoluteSlope < 1024) {
                    current.dy = Inverse(absoluteSlope);
                } else {
                    current.dy = std::abs(Divide(
                        (newSnappedY - snappedY) >> 10,
                        (newSnappedX - snappedX) >> 10
                    ));
                }
                success = true;
            }
            oldX = newX;
            oldY = newY;
        } while (count > 0 && !success);
        qx = newX;
        qy = newY;
        qdx = dx;
        qdy = dy;
        snappedX = newSnappedX;
        snappedY = newSnappedY;
        return success;
    }
};

} // namespace PixelHop::CurveCoverage
namespace PixelHop::ConvexCoverage {
using MaskRaster = Coverage::Raster;
inline constexpr int64_t One = int64_t{1} << 16;
inline int FixedFloor(int64_t value) { return static_cast<int>(value >> 16); }
inline int FixedCeil(int64_t value) { return static_cast<int>((value + One - 1) >> 16); }
inline int64_t CeilFixed(int64_t value) { return static_cast<int64_t>(FixedCeil(value)) * One; }
inline int64_t FixedMul(int64_t a, int64_t b) { return (a * b) >> 16; }
inline int64_t Saturate(int64_t value) { return std::clamp<int64_t>(value, INT32_MIN, INT32_MAX); }
inline int PartialFixed(int alpha, int64_t height) { return static_cast<int>((alpha * height + 32768) >> 16); }

struct MovingEdge {
    CurveCoverage::Quadratic quadratic;
    int64_t x = 0;
    int64_t y = 0;
    size_t next = 0;
    bool line = false;

    bool Initialize(const CurveCoverage::Quad& quad) {
        if (!quadratic.Initialize(quad)) return false;
        x = quadratic.current.upperX;
        y = quadratic.current.upperY;
        return true;
    }
    bool InitializeLine(CurveCoverage::Point a, CurveCoverage::Point b) {
        line = true;
        int64_t x0 = static_cast<int>(a.x * 256.0f) * 256;
        int64_t y0 = CurveCoverage::SnapY(static_cast<int>(a.y * 256.0f) * 256);
        int64_t x1 = static_cast<int>(b.x * 256.0f) * 256;
        int64_t y1 = CurveCoverage::SnapY(static_cast<int>(b.y * 256.0f) * 256);
        quadratic.current.winding = 1;
        if (y0 > y1) {
            std::swap(x0, x1);
            std::swap(y0, y1);
            quadratic.current.winding = -1;
        }
        if (y0 == y1) return false;
        auto& edge = quadratic.current;
        edge.upperX = x0;
        edge.upperY = y0;
        edge.lowerX = x1;
        edge.lowerY = y1;
        edge.dx = CurveCoverage::Divide((x1 - x0) >> 10, (y1 - y0) >> 10);
        const auto absoluteSlope = std::abs(edge.dx >> 10);
        edge.dy = x0 == x1 || edge.dx == 0 ? INT32_MAX : absoluteSlope < 1024
            ? CurveCoverage::Inverse(absoluteSlope)
            : std::abs(CurveCoverage::Divide((y1 - y0) >> 10, (x1 - x0) >> 10));
        quadratic.count = 0;
        x = x0;
        y = y0;
        return true;
    }
    bool Update() {
        if (!quadratic.Next()) return false;
        x = quadratic.current.upperX;
        y = quadratic.current.upperY;
        return true;
    }
    void GoY(int64_t target) {
        const auto& edge = quadratic.current;
        if (target == y + One) {
            x = Saturate(x + edge.dx);
            y = target;
        } else if (target != y) {
            x = Saturate(edge.upperX + FixedMul(edge.dx, target - edge.upperY));
            y = target;
        }
    }
};

inline bool UnitDivide(float numerator, float denominator, float& ratio) {
    if (numerator < 0) {
        numerator = -numerator;
        denominator = -denominator;
    }
    if (denominator == 0 || numerator == 0 || numerator >= denominator) return false;
    ratio = numerator / denominator;
    return !std::isnan(ratio) && ratio != 0;
}

inline bool QuadRoot(float a, float b, float c, float& root) {
    if (a == 0) return UnitDivide(-c, b, root);
    const double discriminant = static_cast<double>(b) * b - 4 * static_cast<double>(a) * c;
    if (discriminant < 0) return false;
    const float radical = static_cast<float>(std::sqrt(discriminant));
    if (!std::isfinite(radical)) return false;
    const float q = b < 0 ? -(b - radical) / 2 : -(b + radical) / 2;
    float first = 0, second = 0;
    const bool hasFirst = UnitDivide(q, a, first);
    const bool hasSecond = UnitDivide(c, q, second);
    if (!hasFirst && !hasSecond) return false;
    root = hasFirst && hasSecond ? std::min(first, second) : hasFirst ? first : second;
    return true;
}

inline bool MonoQuadRoot(const CurveCoverage::Quad& points, int axis, float target, float& t) {
    const auto value = [axis](const CurveCoverage::Point& point) { return axis == 0 ? point.x : point.y; };
    const float c0 = value(points[0]), c1 = value(points[1]), c2 = value(points[2]);
    return QuadRoot(c0 - c1 - c1 + c2, 2 * (c1 - c0), c0 - target, t);
}

inline std::array<CurveCoverage::Point, 5> ChopQuad(const CurveCoverage::Quad& points, float t) {
    const auto mix = [t](const auto& a, const auto& b) {
        return CurveCoverage::Point{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
    };
    const auto a = mix(points[0], points[1]);
    const auto b = mix(points[1], points[2]);
    return {points[0], a, mix(a, b), b, points[2]};
}

inline void AppendVertical(std::vector<MovingEdge>& edges, float x, float y0, float y1, bool reverse) {
    if (reverse) std::swap(y0, y1);
    MovingEdge edge;
    if (!edge.InitializeLine({x, y0}, {x, y1})) return;
    if (!edges.empty()) {
        auto& last = edges.back();
        auto& le = last.quadratic.current;
        const auto& ne = edge.quadratic.current;
        if (last.line && le.dx == 0 && edge.x == last.x &&
            le.winding == ne.winding) {
            if (ne.lowerY == le.upperY) {
                le.upperY = ne.upperY;
                last.y = le.upperY;
                return;
            }
            if (std::abs(ne.upperY - le.lowerY) < 0x100) {
                le.lowerY = ne.lowerY;
                return;
            }
        }
    }
    edges.push_back(edge);
}

inline void AppendQuad(std::vector<MovingEdge>& edges, CurveCoverage::Quad points, bool reverse) {
    if (reverse) std::swap(points[0], points[2]);
    MovingEdge edge;
    if (edge.Initialize(points)) edges.push_back(edge);
}

// SkEdgeClipper first chops a monotone quadratic in Y, then X. Portions outside
// horizontal clipping become vertical boundary edges so winding is preserved.
inline void ClipQuad(std::vector<MovingEdge>& edges, CurveCoverage::Quad points) {
    bool reverse = points[0].y > points[2].y;
    if (reverse) std::swap(points[0], points[2]);
    if (points[2].y <= 0 || points[0].y >= 400) return;
    float t;
    if (points[0].y < 0) {
        if (MonoQuadRoot(points, 1, 0, t)) {
            auto chopped = ChopQuad(points, t);
            chopped[2].y = 0;
            chopped[3].y = std::max(chopped[3].y, 0.0f);
            points[0] = chopped[2];
            points[1] = chopped[3];
        } else {
            for (auto& point : points) point.y = std::max(point.y, 0.0f);
        }
    }
    if (points[2].y > 400) {
        if (MonoQuadRoot(points, 1, 400, t)) {
            auto chopped = ChopQuad(points, t);
            chopped[1].y = std::min(chopped[1].y, 400.0f);
            chopped[2].y = 400;
            points[1] = chopped[1];
            points[2] = chopped[2];
        } else {
            for (auto& point : points) point.y = std::min(point.y, 400.0f);
        }
    }
    if (points[0].x > points[2].x) {
        std::swap(points[0], points[2]);
        reverse = !reverse;
    }
    if (points[2].x <= 0) {
        AppendVertical(edges, 0, points[0].y, points[2].y, reverse);
        return;
    }
    if (points[0].x >= 640) {
        AppendVertical(edges, 640, points[0].y, points[2].y, reverse);
        return;
    }
    if (points[0].x < 0) {
        if (MonoQuadRoot(points, 0, 0, t)) {
            auto chopped = ChopQuad(points, t);
            AppendVertical(edges, 0, chopped[0].y, chopped[2].y, reverse);
            chopped[2].x = 0;
            chopped[3].x = std::max(chopped[3].x, 0.0f);
            points[0] = chopped[2];
            points[1] = chopped[3];
        } else {
            AppendVertical(edges, 0, points[0].y, points[2].y, reverse);
            return;
        }
    }
    if (points[2].x > 640) {
        if (MonoQuadRoot(points, 0, 640, t)) {
            auto chopped = ChopQuad(points, t);
            chopped[1].x = std::min(chopped[1].x, 640.0f);
            chopped[2].x = 640;
            AppendQuad(edges, {chopped[0], chopped[1], chopped[2]}, reverse);
            AppendVertical(edges, 640, chopped[2].y, chopped[4].y, reverse);
        } else {
            points[1].x = std::min(points[1].x, 640.0f);
            points[2].x = std::min(points[2].x, 640.0f);
            AppendQuad(edges, points, reverse);
        }
    } else {
        AppendQuad(edges, points, reverse);
    }
}

inline bool Smooth(const MovingEdge& edge, const MovingEdge& next) {
    const auto& q = edge.quadratic;
    if (q.count > 0) {
        return (std::abs(q.qdx) >> 1) >= std::abs(q.qddx) &&
               (std::abs(q.qdy) >> 1) >= std::abs(q.qddy) &&
               ((q.qdy - q.qddy) >> q.curveShift) >= One;
    }
    return std::abs(Saturate(next.quadratic.current.dx - q.current.dx)) <= One &&
           next.quadratic.current.lowerY - next.quadratic.current.upperY >= One;
}

inline bool Smooth(const MovingEdge& left, const MovingEdge& right,
                   size_t current, const std::vector<MovingEdge>& edges, int stop) {
    const auto& next = edges[current];
    if (next.quadratic.current.upperY >= stop * One) return false;
    if (left.quadratic.current.lowerY + One < right.quadratic.current.lowerY)
        return Smooth(left, next);
    if (left.quadratic.current.lowerY > right.quadratic.current.lowerY + One)
        return Smooth(right, next);
    const auto& second = edges[next.next];
    if (second.quadratic.current.upperY >= stop * One) return false;
    if (second.quadratic.current.upperX < next.quadratic.current.upperX)
        return Smooth(left, second) && Smooth(right, next);
    return Smooth(left, next) && Smooth(right, second);
}

// Source-compatible convex analytic walk. A small oval uses Skia's mask
// additive blitter, whose accumulated bytes do not receive RLE row snapping.
inline std::vector<uint8_t> EllipseMask(double centerX, double centerY,
                                       double radiusX, double radiusY) {
    std::vector<uint8_t> result(640 * 400);
    if (!(radiusX > 0 && radiusY > 0)) return result;
    auto quads = CurveCoverage::EllipseQuads(centerX, centerY, radiusX, radiusY);
    std::vector<MovingEdge> edges;
    edges.reserve(quads.size() + 1);
    const float floatX = static_cast<float>(centerX), floatY = static_cast<float>(centerY);
    const float floatRX = static_cast<float>(radiusX), floatRY = static_cast<float>(radiusY);
    const bool contained = floatX - floatRX >= 0 && floatX + floatRX <= 640 &&
                           floatY - floatRY >= 0 && floatY + floatRY <= 400;
    for (const auto& quad : quads) {
        if (contained) AppendQuad(edges, quad, false);
        else ClipQuad(edges, quad);
    }
    if (edges.size() < 2) return result;
    std::sort(edges.begin(), edges.end(), [](const auto& a, const auto& b) {
        const auto& ae = a.quadratic.current;
        const auto& be = b.quadratic.current;
        if (ae.upperY != be.upperY) return ae.upperY < be.upperY;
        if (a.x != b.x) return a.x < b.x;
        return ae.dx < be.dx;
    });
    const size_t sentinel = edges.size();
    MovingEdge tail;
    tail.quadratic.current.upperX = INT32_MAX;
    tail.quadratic.current.upperY = INT32_MAX;
    tail.quadratic.current.lowerY = INT32_MAX;
    tail.quadratic.current.dy = INT32_MAX;
    edges.push_back(tail);
    for (size_t i = 0; i < edges.size(); ++i) edges[i].next = std::min(i + 1, sentinel);

    const int stop = std::min(400, static_cast<int>(std::ceil(floatY + floatRY)));
    const int64_t leftBound = std::max(0, static_cast<int>(std::floor(floatX - floatRX))) * One;
    const int64_t rightBound = std::min(640, static_cast<int>(std::ceil(floatX + floatRX))) * One;
    if (leftBound >= rightBound) return result;
    MaskRaster raster(true);
    size_t leftIndex = 0, rightIndex = 1, current = 2;
    int64_t y = std::max(edges[leftIndex].quadratic.current.upperY,
                         edges[rightIndex].quadratic.current.upperY);
    for (;;) {
        while (edges[leftIndex].quadratic.current.lowerY <= y ||
               edges[rightIndex].quadratic.current.lowerY <= y) {
            while (edges[leftIndex].quadratic.current.lowerY <= y) {
                if (!edges[leftIndex].Update()) {
                    if (FixedFloor(edges[current].quadratic.current.upperY) >= stop) goto finished;
                    leftIndex = current;
                    current = edges[current].next;
                }
            }
            while (edges[rightIndex].quadratic.current.lowerY <= y) {
                if (!edges[rightIndex].Update()) {
                    if (FixedFloor(edges[current].quadratic.current.upperY) >= stop) goto finished;
                    rightIndex = current;
                    current = edges[current].next;
                }
            }
            y = std::max({y, edges[leftIndex].quadratic.current.upperY,
                             edges[rightIndex].quadratic.current.upperY});
        }
        if (FixedFloor(y) >= stop) break;
        edges[leftIndex].GoY(y);
        edges[rightIndex].GoY(y);
        if (edges[leftIndex].x > edges[rightIndex].x ||
            (edges[leftIndex].x == edges[rightIndex].x &&
             edges[leftIndex].quadratic.current.dx > edges[rightIndex].quadratic.current.dx))
            std::swap(leftIndex, rightIndex);
        auto& leftEdge = edges[leftIndex];
        auto& rightEdge = edges[rightIndex];
        auto localBottom = std::min(leftEdge.quadratic.current.lowerY,
                                    rightEdge.quadratic.current.lowerY);
        if (Smooth(leftEdge, rightEdge, current, edges, stop)) localBottom = CeilFixed(localBottom);
        localBottom = std::min(localBottom, stop * One);
        auto left = std::clamp(leftEdge.x, leftBound, rightBound);
        auto right = std::clamp(rightEdge.x, leftBound, rightBound);
        const auto dLeft = leftEdge.quadratic.current.dx;
        const auto dRight = rightEdge.quadratic.current.dx;
        if ((dLeft | dRight) == 0) {
            // A constant-X segment is a rectangle. This branch is unusual for
            // an oval, but preserves the source rectangle coverage rounding.
            const int firstRow = FixedFloor(y);
            const int lastRow = FixedCeil(localBottom);
            for (int row = firstRow; row < lastRow; ++row) {
                const auto top = std::max(y, row * One);
                const auto bottom = std::min(localBottom, (row + 1) * One);
                if (bottom <= top) continue;
                for (int x = FixedFloor(left); x < FixedCeil(right); ++x) {
                    const auto width = std::max<int64_t>(0, std::min(right, (x + 1) * One) -
                                                             std::max(left, x * One));
                    raster.AddPixel(row, x, PartialFixed(255, FixedMul(bottom - top, width)));
                }
            }
            y = localBottom;
        } else {
            constexpr int64_t snapDigit = One >> 4;
            constexpr int64_t snapHalf = snapDigit >> 1;
            constexpr int64_t snapMask = -snapDigit;
            left += snapHalf;
            right += snapHalf;
            const auto minX = leftBound + snapHalf;
            const auto maxX = rightBound + snapHalf;
            int count = FixedCeil(localBottom) - FixedFloor(y);
            auto blit = [&](int64_t nextY, int alpha) {
                const auto height = nextY - y;
                const auto nextLeft = std::clamp(Saturate(left + FixedMul(dLeft, height)), minX, maxX);
                const auto nextRight = std::clamp(Saturate(right + FixedMul(dRight, height)), minX, maxX);
                raster.Trapezoid(FixedFloor(y), left & snapMask, right & snapMask,
                                 nextLeft & snapMask, nextRight & snapMask,
                                 leftEdge.quadratic.current.dy, rightEdge.quadratic.current.dy,
                                 alpha, false);
                left = nextLeft;
                right = nextRight;
                y = nextY;
            };
            if (count > 1) {
                if (y != (y & -One)) {
                    --count;
                    const auto nextY = CeilFixed(y + 1);
                    blit(nextY, PartialFixed(255, nextY - y));
                }
                while (count > 1) {
                    --count;
                    blit(y + One, 255);
                }
            }
            blit(localBottom, PartialFixed(255, localBottom - y));
            left -= snapHalf;
            right -= snapHalf;
        }
        leftEdge.x = left;
        rightEdge.x = right;
        leftEdge.y = rightEdge.y = y;
    }
finished:
    for (size_t i = 0; i < result.size(); ++i) result[i] = raster.pixels[i].alpha;
    return result;
}
} // namespace PixelHop::ConvexCoverage


// Skia-compatible straight-line analytic scan conversion for compound paths.
// Original portions Copyright Google Inc. and The Android Open Source Project.
// Distributed under the Skia BSD license. See the project's licenses/SKIA_LICENSE.txt.

namespace PixelHop::LineCoverage {
inline constexpr int64_t One = 65536;
inline int Floor(int64_t value) { return static_cast<int>(value >> 16); }
inline int Ceil(int64_t value) { return static_cast<int>((value + One - 1) >> 16); }
inline int64_t CeilFixed(int64_t value) { return static_cast<int64_t>(Ceil(value)) * One; }
inline int64_t Pin(int64_t value) { return std::clamp<int64_t>(value, INT32_MIN, INT32_MAX); }
inline int64_t Divide(int64_t a, int64_t b) {
    if (std::abs(b) >= 8 && std::abs(b) < 1024 && std::abs(a) < 4096) {
        const int64_t inverse = ((int64_t{1} << 22) / std::abs(b)) * (b < 0 ? -1 : 1);
        return (a * inverse) >> 6;
    }
    return (a * One) / b;
}
struct Point { float x, y; };
struct Edge {
    Edge* prev = nullptr;
    Edge* next = nullptr;
    int64_t x = 0, y = 0, upperX = 0, upperY = 0, lowerY = 0, dx = 0, dy = 0;
    int winding = 1;
    void GoY(int64_t target) {
        if (target == y + One) {
            x = Pin(x + dx);
            y = target;
        } else if (target != y) {
            x = Pin(upperX + ((dx * (target - upperY)) >> 16));
            y = target;
        }
    }
    void GoY(int64_t target, int shift) {
        y = target;
        x = Pin(x + (dx >> shift));
    }
};

inline float Horizontal(const std::array<Point,2>& source, float y) {
    if (std::abs(source[1].y - source[0].y) <= 1.0f / 4096)
        return source[0].x / 2 + source[1].x / 2;
    const double x0 = source[0].x, x1 = source[1].x;
    const double value = x0 + (static_cast<double>(y) - source[0].y) * (x1 - x0) /
                               (static_cast<double>(source[1].y) - source[0].y);
    return static_cast<float>(std::clamp(value, std::min(x0,x1), std::max(x0,x1)));
}
inline float Vertical(const std::array<Point,2>& source, float x) {
    float y;
    if (std::abs(source[1].x - source[0].x) <= 1.0f / 4096) {
        y = source[0].y / 2 + source[1].y / 2;
    } else {
        const double value = source[0].y + (static_cast<double>(x) - source[0].x) *
            (static_cast<double>(source[1].y) - source[0].y) /
            (static_cast<double>(source[1].x) - source[0].x);
        y = static_cast<float>(value);
    }
    return std::clamp(y, std::min(source[0].y, source[1].y), std::max(source[0].y, source[1].y));
}

inline std::vector<Point> ClipLine(Point a, Point b) {
    const std::array<Point,2> source{a,b};
    int first = a.y < b.y ? 0 : 1;
    int last = 1 - first;
    if (source[last].y <= 0 || source[first].y >= 400) return {};
    auto clipped = source;
    if (source[first].y < 0) clipped[first] = {Horizontal(source,0),0};
    if (source[last].y > 400) clipped[last] = {Horizontal(source,400),400};
    bool reverse = a.x >= b.x;
    first = reverse ? 1 : 0;
    last = 1 - first;
    std::vector<Point> points;
    if (clipped[last].x <= 0) {
        clipped[0].x = clipped[1].x = 0;
        return {clipped[0],clipped[1]};
    }
    if (clipped[first].x >= 640) return {}; // Non-convex paths cull wholly-right segments.
    if (clipped[first].x < 0) {
        points.push_back({0,clipped[first].y});
        points.push_back({0,Vertical(clipped,0)});
    } else points.push_back(clipped[first]);
    if (clipped[last].x > 640) {
        points.push_back({640,Vertical(clipped,640)});
        points.push_back({640,clipped[last].y});
    } else points.push_back(clipped[last]);
    if (reverse) std::reverse(points.begin(),points.end());
    return points;
}

inline void AddLine(std::vector<Edge>& edges, Point a, Point b) {
    int64_t x0 = static_cast<int64_t>(a.x * 256.0f) * 256;
    int64_t y0 = ((static_cast<int64_t>(a.y * 256.0f) + 32) >> 6) * 16384;
    int64_t x1 = static_cast<int64_t>(b.x * 256.0f) * 256;
    int64_t y1 = ((static_cast<int64_t>(b.y * 256.0f) + 32) >> 6) * 16384;
    int winding = 1;
    if (y0 > y1) { std::swap(x0,x1); std::swap(y0,y1); winding = -1; }
    const auto deltaY = (y1-y0) >> 10;
    if (!deltaY) return;
    const auto deltaX = (x1-x0) >> 10;
    const auto dx = Divide(deltaX,deltaY);
    const auto dy = deltaX == 0 || dx == 0 ? INT32_MAX : std::abs(dx) < 1024
        ? (int64_t{1} << 22) / std::abs(dx) : std::abs(Divide(deltaY,deltaX));
    Edge edge;
    edge.x = edge.upperX = x0;
    edge.y = edge.upperY = y0;
    edge.lowerY = y1;
    edge.dx = dx;
    edge.dy = dy;
    edge.winding = winding;
    if (dx == 0 && !edges.empty()) {
        auto& previous = edges.back();
        const auto approximately = [](int64_t p,int64_t q) { return std::abs(p-q) < 0x100; };
        if (previous.dx == 0 && previous.x == edge.x) {
            if (edge.winding == previous.winding) {
                if (edge.lowerY == previous.upperY) {
                    previous.upperY = previous.y = edge.upperY;
                    return;
                }
                if (approximately(edge.upperY,previous.lowerY)) {
                    previous.lowerY = edge.lowerY;
                    return;
                }
            } else if (approximately(edge.upperY,previous.upperY)) {
                if (approximately(edge.lowerY,previous.lowerY)) { edges.pop_back(); return; }
                if (edge.lowerY < previous.lowerY) {
                    previous.upperY = previous.y = edge.lowerY;
                    return;
                }
                previous.upperY = previous.y = previous.lowerY;
                previous.lowerY = edge.lowerY;
                previous.winding = edge.winding;
                return;
            } else if (approximately(edge.lowerY,previous.lowerY)) {
                if (edge.upperY > previous.upperY) { previous.lowerY = edge.upperY; return; }
                previous.lowerY = previous.upperY;
                previous.upperY = previous.y = edge.upperY;
                previous.winding = edge.winding;
                return;
            }
        }
    }
    edges.push_back(edge);
}

inline void Remove(Edge* edge) { edge->prev->next = edge->next; edge->next->prev = edge->prev; }
inline void Insert(Edge* edge, Edge* after) {
    edge->prev = after;
    edge->next = after->next;
    after->next->prev = edge;
    after->next = edge;
}
inline Edge* BackwardStart(Edge* edge, int64_t x) {
    while (edge->prev && edge->x > x) edge = edge->prev;
    return edge;
}
inline void BackwardInsert(Edge* edge) {
    Edge* previous = BackwardStart(edge->prev,edge->x);
    if (previous->next != edge) { Remove(edge); Insert(edge,previous); }
}
inline void UpdateNext(int64_t y, int64_t next, int64_t& following) {
    if (y > next && y < following) following = y;
}
inline void CheckIntersection(const Edge* edge, int64_t next, int64_t& following) {
    if (edge->prev->prev && edge->prev->x + edge->prev->dx > edge->x + edge->dx)
        following = next + One/4;
}
inline void CheckIntersectionForward(const Edge* edge, int64_t next, int64_t& following) {
    if (edge->next->next && edge->x + edge->dx > edge->next->x + edge->next->dx)
        following = next + One/4;
}
inline void InsertNew(Edge* edge, int64_t y, int64_t& following) {
    if (edge->upperY > y) { UpdateNext(edge->upperY,y,following); return; }
    Edge* previous = edge->prev;
    if (previous->x <= edge->x) {
        while (edge->upperY <= y) {
            CheckIntersection(edge,y,following);
            UpdateNext(edge->lowerY,y,following);
            edge = edge->next;
        }
        UpdateNext(edge->upperY,y,following);
        return;
    }
    Edge* start = BackwardStart(previous,edge->x);
    do {
        Edge* next = edge->next;
        for (;;) {
            if (start->next == edge) break;
            Edge* after = start->next;
            if (after->x >= edge->x) { Remove(edge); Insert(edge,start); break; }
            start = after;
        }
        CheckIntersection(edge,y,following);
        CheckIntersectionForward(edge,y,following);
        UpdateNext(edge->lowerY,y,following);
        start = edge;
        edge = next;
    } while (edge->upperY <= y);
    UpdateNext(edge->upperY,y,following);
}
inline bool TooClose(Edge* previous,Edge* next,int64_t lowerY) {
    return next && previous && next->upperY < lowerY &&
           previous->x + One >= next->x - std::abs(next->dx);
}

using Blitter = Coverage::Raster::Blitter;
inline void DrawMountains(double offset,double baseY,double spacing,double height,
                          const Blitter& blitter) {
    // Canvas's clip wrapper routes these draws through its generic anti-H path,
    // including the two-pixel case that otherwise selects fast interpolation.
    Coverage::Raster raster(false, [&](int y,int x,int alpha,bool) { blitter(y,x,alpha,false); });
    const int count = static_cast<int>(std::ceil(640/spacing))+2;
    const int start = static_cast<int>(std::floor(-offset/spacing))-1;
    std::vector<Edge> edges;
    edges.reserve(count*8);
    for (int i=start;i<start+count;++i) {
        const double x = i*spacing+std::fmod(offset,spacing);
        const std::array<Point,3> triangle{{{static_cast<float>(x),static_cast<float>(baseY)},
            {static_cast<float>(x+spacing/2),static_cast<float>(baseY-height)},
            {static_cast<float>(x+spacing),static_cast<float>(baseY)}}};
        for (int j=0;j<3;++j) {
            const auto clipped = ClipLine(triangle[j],triangle[(j+1)%3]);
            for (size_t k=1;k<clipped.size();++k) AddLine(edges,clipped[k-1],clipped[k]);
        }
    }
    if (edges.empty()) return;
    std::sort(edges.begin(),edges.end(),[](const Edge& a,const Edge& b) {
        if (a.upperY != b.upperY) return a.upperY < b.upperY;
        if (a.x != b.x) return a.x < b.x;
        return a.dx < b.dx;
    });
    Edge head,tail;
    head.upperY = head.lowerY = INT32_MIN;
    head.x = head.upperX = 0;
    head.dy = INT32_MAX;
    tail.upperY = tail.lowerY = INT32_MAX;
    tail.x = tail.upperX = 640*One;
    tail.dy = INT32_MAX;
    head.next = &edges.front();
    tail.prev = &edges.back();
    for (size_t i=0;i<edges.size();++i) {
        edges[i].prev = i ? &edges[i-1] : &head;
        edges[i].next = i+1<edges.size() ? &edges[i+1] : &tail;
    }
    const int startY = std::max(0,static_cast<int>(std::floor(baseY-height)));
    const int stopY = std::min(400,static_cast<int>(std::ceil(baseY)));
    int64_t y = std::max(head.next->upperY,startY*One);
    int64_t following = INT32_MAX;
    Edge* edge = head.next;
    for (;edge->upperY<=y;edge=edge->next) { edge->GoY(y); UpdateNext(edge->lowerY,y,following); }
    UpdateNext(edge->upperY,y,following);
    const bool skipIntersections = count*3 > (stopY-startY)*2;
    for (;;) {
        int winding=0;
        bool inside=false;
        int64_t previousX=head.x;
        int64_t nextY=std::min(following,CeilFixed(y+1));
        Edge* current=head.next;
        Edge* leftEdge=&head;
        int64_t left=0,leftDY=0;
        int previousRight=0;
        following=INT32_MAX;
        int shift=0;
        if ((nextY-y)&(One/4)) { shift=2; nextY=y+One/4; }
        else if ((nextY-y)&(One/2)) shift=1;
        const int alpha=static_cast<int>((255*(nextY-y)+One/2)>>16);
        while (current->upperY<=y) {
            winding+=current->winding;
            const bool wasInside=inside;
            inside=winding!=0;
            const bool isLeft=inside&&!wasInside;
            const bool isRight=!inside&&wasInside;
            if (isRight) {
                int64_t right=current->x;
                current->GoY(nextY,shift);
                const int64_t nextLeft=std::max<int64_t>(0,leftEdge->x);
                right=std::min<int64_t>(640*One,right);
                const int64_t nextRight=std::min<int64_t>(640*One,current->x);
                const bool force=alpha==255 && (previousRight>Floor(left)||previousRight>Floor(leftEdge->x)||
                                                 TooClose(current,current->next,nextY));
                raster.Trapezoid(Floor(y),left,right,nextLeft,nextRight,leftDY,current->dy,alpha,force);
                previousRight=Ceil(std::max(right,current->x));
            } else {
                if (isLeft) { left=std::max<int64_t>(current->x,0); leftDY=current->dy; leftEdge=current; }
                current->GoY(nextY,shift);
            }
            Edge* next=current->next;
            if (current->lowerY<=nextY) Remove(current);
            else {
                UpdateNext(current->lowerY,nextY,following);
                const int64_t newX=current->x;
                if (newX<previousX) BackwardInsert(current);
                else previousX=newX;
                if (!skipIntersections) CheckIntersection(current,nextY,following);
            }
            current=next;
        }
        if (inside) {
            const bool force=alpha==255 && TooClose(leftEdge->prev,leftEdge,nextY);
            raster.Trapezoid(Floor(y),left,640*One,std::max<int64_t>(0,leftEdge->x),640*One,
                             leftDY,0,alpha,force);
        }
        if (Floor(y)!=Floor(nextY)) raster.FinishRow(Floor(y));
        y=nextY;
        if (y>=stopY*One) break;
        InsertNew(current,y,following);
    }
}

inline std::vector<Coverage::Pixel> MountainMask(double offset,double baseY,double spacing,double height) {
    std::vector<Coverage::Pixel> pixels(640*400);
    DrawMountains(offset,baseY,spacing,height,[&](int y,int x,int alpha,bool) {
        auto& pixel=pixels[y*640+x];
        pixel.alpha=static_cast<uint8_t>(alpha+((pixel.alpha*(256-alpha))>>8));
    });
    return pixels;
}
} // namespace PixelHop::LineCoverage

namespace PixelHop {
namespace {
constexpr char kFontTexture[] = "pixel-hop-twins-fonts";

nlohmann::json LoadJson(const std::string& relative) {
    const auto path = ProjectManager::GetInstance().ResolveAssetPath(relative);
    size_t length = 0;
    void* data = SDL_LoadFile(path.c_str(), &length);
    if (!data) throw std::runtime_error("Could not load " + relative);
    const auto json = nlohmann::json::parse(static_cast<const char*>(data),
                                          static_cast<const char*>(data) + length, nullptr, false);
    SDL_free(data);
    if (json.is_discarded()) throw std::runtime_error("Invalid JSON " + relative);
    return json;
}

std::vector<uint8_t> LoadBytes(const std::string& relative) {
    const auto path = ProjectManager::GetInstance().ResolveAssetPath(relative);
    size_t length = 0;
    void* data = SDL_LoadFile(path.c_str(), &length);
    if (!data) throw std::runtime_error("Could not load " + relative);
    const auto begin = static_cast<const uint8_t*>(data);
    std::vector<uint8_t> result(begin, begin + length);
    SDL_free(data);
    return result;
}

std::vector<std::string> Characters(const std::string& text) {
    std::vector<std::string> result;
    for (size_t offset = 0; offset < text.size();) {
        const unsigned char byte = static_cast<unsigned char>(text[offset]);
        size_t length = 1;
        if ((byte & 0xe0) == 0xc0) length = 2;
        else if ((byte & 0xf0) == 0xe0) length = 3;
        else if ((byte & 0xf8) == 0xf0) length = 4;
        length = std::min(length, text.size() - offset);
        result.push_back(text.substr(offset, length));
        offset += length;
    }
    return result;
}

std::string Padded(int value, int width) {
    std::ostringstream stream;
    stream << std::setfill('0') << std::setw(width) << value;
    return stream.str();
}


} // namespace

bool NativeRender::Initialize() {
    Clear();
    try {
        const auto sprites = LoadJson("assets/sprites.json");
        for (const auto& entry : sprites.at("palette")) {
            const auto hex = entry.get<std::string>();
            const auto rgb = std::stoul(hex.substr(1), nullptr, 16);
            m_palette.emplace_back(((rgb >> 16) & 255) / 255.0f,
                                   ((rgb >> 8) & 255) / 255.0f,
                                   (rgb & 255) / 255.0f, 1);
        }
        if (m_palette.size() != 16) throw std::runtime_error("Expected original 16-color palette");
        for (const auto& item : sprites.at("matrices").items())
            m_matrices[item.key()] = item.value().get<Matrix>();

        const auto fonts = LoadJson("assets/fonts.json");
        m_atlasWidth = fonts.at("width").get<double>();
        m_atlasHeight = fonts.at("height").get<double>();
        const auto masks = fonts.contains("maskFile") ? LoadBytes(fonts.at("maskFile")) : std::vector<uint8_t>{};
        const auto readMask = [&](const nlohmann::json& value, const std::string& key, size_t count) {
            if (value.contains(key)) return value.at(key).get<std::vector<uint8_t>>();
            const auto offsetKey = key + "Offset";
            if (!value.contains(offsetKey)) return std::vector<uint8_t>{};
            const size_t offset = value.at(offsetKey).get<size_t>();
            if (offset > masks.size() || count > masks.size() - offset)
                throw std::runtime_error("Font mask range exceeds the packed mask asset");
            return std::vector<uint8_t>(masks.begin() + offset, masks.begin() + offset + count);
        };
        const auto parseGlyph = [&](const nlohmann::json& value) {
            Glyph glyph;
            glyph.x = value.at("x"); glyph.y = value.at("y");
            glyph.w = value.at("w"); glyph.h = value.at("h");
            glyph.offsetX = value.at("offsetX"); glyph.offsetY = value.at("offsetY");
            glyph.advance = value.at("advance");
            const size_t count = static_cast<size_t>(glyph.w * glyph.h);
            glyph.alpha = readMask(value, "alpha", count);
            if (value.contains("colors")) {
                for (const auto& item : value.at("colors").items()) {
                    glyph.colors[item.key()] = {
                        readMask(item.value(), "alpha", count),
                        readMask(item.value(), "premul", count * 3)
                    };
                }
            }
            return glyph;
        };
        for (const auto& size : fonts.at("sizes").items()) {
            auto& font = m_fonts[std::stoi(size.key())];
            for (const auto& item : size.value().at("glyphs").items()) {
                font.glyphs[item.key()] = parseGlyph(item.value());
                if (item.value().contains("phases")) {
                    for (const auto& phase : item.value().at("phases"))
                        font.phases[item.key()].push_back(parseGlyph(phase));
                }
            }
        }
        const auto path = ProjectManager::GetInstance().ResolveAssetPath("assets/fonts.png");
        if (!Renderer2D::GetInstance().LoadTexture(kFontTexture, path, SamplerType::Nearest))
            throw std::runtime_error("Could not register original font atlas");
        m_ready = true;
        m_pixels.assign(640 * 400, 0);
    } catch (const std::exception& error) {
        LOGE("[PixelHop] Renderer initialization failed: %s", error.what());
    }
    return m_ready;
}

void NativeRender::Clear() {
    m_ready = false;
    m_renderer = nullptr;
    m_matrices.clear();
    m_palette.clear();
    m_fonts.clear();
    m_rasterizing = false;
    m_pixels.clear();
    m_lastQuadCount = 0;
}

void NativeRender::EmitRect(double x, double y, double w, double h, glm::vec4 color, int coverage) {
    if (w <= 0 || h <= 0 || color.a <= 0 || coverage <= 0) return;
    if (m_rasterizing) {
        const int left = std::clamp(static_cast<int>(x), 0, 640);
        const int top = std::clamp(static_cast<int>(y), 0, 400);
        const int right = std::clamp(static_cast<int>(x + w), left, 640);
        const int bottom = std::clamp(static_cast<int>(y + h), top, 400);
        const auto byte = [](float value) { return std::clamp(static_cast<int>(std::round(value * 255)), 0, 255); };
        const int paintAlpha = byte(color.a);
        const int scale = std::clamp(coverage, 0, 255) + 1;
        const int alpha = paintAlpha * scale >> 8;
        if (!alpha) return;
        const int red = byte(color.r), green = byte(color.g), blue = byte(color.b);
        const uint32_t opaque = static_cast<uint32_t>((red << 16) | (green << 8) | blue);
        const int sourceRed = ((red * paintAlpha + 127) / 255) * scale >> 8;
        const int sourceGreen = ((green * paintAlpha + 127) / 255) * scale >> 8;
        const int sourceBlue = ((blue * paintAlpha + 127) / 255) * scale >> 8;
        for (int row = top; row < bottom; ++row) {
            auto begin = m_pixels.begin() + row * 640 + left;
            auto end = m_pixels.begin() + row * 640 + right;
            if (alpha == 255) {
                std::fill(begin, end, opaque);
            } else {
                for (auto pixel = begin; pixel != end; ++pixel) {
                    const int r = sourceRed + (((*pixel >> 16) & 255) * (256 - alpha) >> 8);
                    const int g = sourceGreen + (((*pixel >> 8) & 255) * (256 - alpha) >> 8);
                    const int b = sourceBlue + ((*pixel & 255) * (256 - alpha) >> 8);
                    *pixel = static_cast<uint32_t>((r << 16) | (g << 8) | b);
                }
            }
        }
        return;
    }
    m_renderer->DrawRect(glm::vec2(m_offset + glm::dvec2(x, y) * m_scale),
                        glm::vec2(w * m_scale, h * m_scale), color, ++m_layer);
}

void NativeRender::Rect(double x, double y, double w, double h, glm::vec4 color) {
    const double left = std::max(0.0, x);
    const double right = std::min(640.0, x + w);
    const double top = std::max(0.0, y);
    const double bottom = std::min(400.0, y + h);
    if (left >= right || top >= bottom) return;
    // Coverage follows Skia's AntiFillRect fixed-point rasterization, including
    // the one-pixel span bias. Copyright 2011 The Android Open Source Project,
    // under the BSD-style license in licenses/SKIA_LICENSE.txt.
    const auto fixed = [](double value) {
        return (static_cast<int64_t>(static_cast<float>(value) * 65536.0f) + 128) >> 8;
    };
    const int l = static_cast<int>(fixed(left)), r = static_cast<int>(fixed(right));
    const int t = static_cast<int>(fixed(top)), b = static_cast<int>(fixed(bottom));
    if (l >= r || t >= b) return;
    const auto scanline = [&](int row, int alpha) {
        if ((l >> 8) == ((r - 1) >> 8)) {
            EmitRect(l >> 8, row, 1, 1, color, alpha * (r - l) >> 8);
            return;
        }
        int first = l >> 8;
        if (l & 255) {
            EmitRect(first, row, 1, 1, color, alpha * (256 - (l & 255)) >> 8);
            ++first;
        }
        const int last = r >> 8;
        if (last > first) EmitRect(first, row, last - first, 1, color, alpha);
        if (r & 255) EmitRect(last, row, 1, 1, color, alpha * (r & 255) >> 8);
    };
    int firstRow = t >> 8;
    if (firstRow == ((b - 1) >> 8)) {
        scanline(firstRow, b - t - 1);
        return;
    }
    if (t & 255) {
        scanline(firstRow, 256 - (t & 255));
        ++firstRow;
    }
    const int lastRow = b >> 8;
    if (lastRow > firstRow) {
        const int rows = lastRow - firstRow;
        int first = l >> 8;
        if (first == ((r - 1) >> 8)) {
            EmitRect(first, firstRow, 1, rows, color, r - l - 1);
        } else {
            if (l & 255) {
                EmitRect(first, firstRow, 1, rows, color, 256 - (l & 255));
                ++first;
            }
            const int last = r >> 8;
            if (last > first) EmitRect(first, firstRow, last - first, rows, color);
            if (r & 255) EmitRect(last, firstRow, 1, rows, color, r & 255);
        }
    }
    if (b & 255) scanline(lastRow, b & 255);
}

void NativeRender::Pixels(const std::string& name, double x, double y, double w, double h, bool flip) {
    const auto found = m_matrices.find(name);
    if (found == m_matrices.end() || x >= 640 || y >= 400 || x + w <= 0 || y + h <= 0) return;
    const auto& matrix = found->second;
    if (matrix.empty() || matrix.front().empty()) return;
    const double pixelWidth = w / matrix.front().size();
    const double pixelHeight = h / matrix.size();
    for (size_t row = 0; row < matrix.size(); ++row) {
        for (size_t column = 0; column < matrix[row].size(); ++column) {
            const int index = matrix[row][column];
            if (index < 0) continue;
            const double drawX = flip ? x + w - (column + 1) * pixelWidth : x + column * pixelWidth;
            Rect(drawX, y + row * pixelHeight, pixelWidth, pixelHeight, m_palette[index & 15]);
        }
    }
}

double NativeRender::TextWidth(const std::string& text, int fontSize) const {
    const auto font = m_fonts.find(fontSize);
    if (font == m_fonts.end()) return 0;
    double width = 0;
    for (const auto& character : Characters(text)) {
        const auto glyph = font->second.glyphs.find(character);
        if (glyph != font->second.glyphs.end()) width += glyph->second.advance;
    }
    return width;
}

void NativeRender::Text(const std::string& text, double x, double baseline, int fontSize,
                        glm::vec4 color, const std::string& alignment) {
    const auto font = m_fonts.find(fontSize);
    if (font == m_fonts.end()) return;
    const double width = TextWidth(text, fontSize);
    if (alignment == "center") x -= width / 2;
    else if (alignment == "right") x -= width;
    const auto texture = m_renderer->GetTexture(kFontTexture);
    for (const auto& character : Characters(text)) {
        const auto found = font->second.glyphs.find(character);
        if (found == font->second.glyphs.end()) continue;
        const Glyph* selected = &found->second;
        double drawX = x;
        const auto phases = font->second.phases.find(character);
        if (phases != font->second.phases.end() && !phases->second.empty()) {
            const auto count = phases->second.size();
            // Chromium's glyph rasterizer uses quarter-pixel X positions. Choose
            // that position directly to avoid double rounding through the atlas.
            constexpr long long positions = 4;
            const auto quantized = static_cast<long long>(std::floor(x * positions + 0.5));
            const auto fractional = (quantized % positions + positions) % positions;
            const auto phase = static_cast<size_t>(fractional) * count / positions;
            selected = &phases->second[phase];
            drawX = std::floor(static_cast<double>(quantized) / positions);
        }
        const auto& glyph = *selected;
        const glm::dvec2 position(drawX + glyph.offsetX, baseline + glyph.offsetY);
        if (m_rasterizing && glyph.alpha.size() == static_cast<size_t>(glyph.w * glyph.h)) {
            std::ostringstream colorKey;
            colorKey << '#' << std::uppercase << std::hex << std::setfill('0');
            for (int channel = 0; channel < 3; ++channel)
                colorKey << std::setw(2) << static_cast<int>(std::round(color[channel] * 255));
            const auto mask = glyph.colors.find(colorKey.str());
            for (int row = 0; row < static_cast<int>(glyph.h); ++row) {
                for (int column = 0; column < static_cast<int>(glyph.w); ++column) {
                    const int index = row * static_cast<int>(glyph.w) + column;
                    if (mask != glyph.colors.end()) {
                        const int px = static_cast<int>(position.x) + column;
                        const int py = static_cast<int>(position.y) + row;
                        if (px < 0 || px >= 640 || py < 0 || py >= 400) continue;
                        const int alpha = mask->second.alpha[index];
                        const auto& source = mask->second.premul;
                        auto& pixel = m_pixels[py * 640 + px];
                        const int red = source[index * 3] + (((pixel >> 16) & 255) * (256 - alpha) >> 8);
                        const int green = source[index * 3 + 1] + (((pixel >> 8) & 255) * (256 - alpha) >> 8);
                        const int blue = source[index * 3 + 2] + ((pixel & 255) * (256 - alpha) >> 8);
                        pixel = static_cast<uint32_t>((red << 16) | (green << 8) | blue);
                        continue;
                    }
                    auto covered = color;
                    covered.a *= glyph.alpha[index] / 255.0f;
                    EmitRect(position.x + column, position.y + row, 1, 1, covered);
                }
            }
            x += found->second.advance;
            continue;
        }
        const glm::vec2 uv0(glyph.x / m_atlasWidth, 1.0 - (glyph.y + glyph.h) / m_atlasHeight);
        const glm::vec2 uv1((glyph.x + glyph.w) / m_atlasWidth, 1.0 - glyph.y / m_atlasHeight);
        m_renderer->DrawSprite(glm::vec2(m_offset + position * m_scale),
                              glm::vec2(glyph.w * m_scale, glyph.h * m_scale),
                              texture, uv0, uv1, color, ++m_layer);
        x += found->second.advance;
    }
}

void NativeRender::DrawLabel(Renderer2D& renderer, const std::string& text, glm::vec2 baseline,
                             int fontSize, glm::vec4 color, int layer) {
    if (!m_ready) return;
    const auto savedOffset = m_offset;
    const auto savedScale = m_scale;
    const auto savedLayer = m_layer;
    m_renderer = &renderer;
    m_offset = {};
    m_scale = 1;
    m_layer = layer;
    Text(text, baseline.x, baseline.y, fontSize, color, "center");
    m_offset = savedOffset;
    m_scale = savedScale;
    m_layer = savedLayer;
}

void NativeRender::Mountains(double offsetX, double baseY, double spacing, double height, glm::vec4 color) {
    int pendingRow = -1;
    int pendingX = 0;
    int pendingWidth = 0;
    int pendingAlpha = 0;
    const auto flush = [&] {
        if (pendingWidth) EmitRect(pendingX, pendingRow, pendingWidth, 1, color, pendingAlpha);
        pendingWidth = 0;
    };
    LineCoverage::DrawMountains(offsetX, baseY, spacing, height,
        [&](int row, int column, int alpha, bool) {
            if (alpha <= 0) return;
            if (row == pendingRow && column == pendingX + pendingWidth && alpha == pendingAlpha) {
                ++pendingWidth;
            } else {
                flush();
                pendingRow = row;
                pendingX = column;
                pendingWidth = 1;
                pendingAlpha = alpha;
            }
        });
    flush();
}

void NativeRender::Aura(double x, double y, double rx, double ry, glm::vec4 color) {
    const auto coverage = ConvexCoverage::EllipseMask(x, y, rx, ry);
    const int top = std::max(0, static_cast<int>(std::floor(y - ry)) - 1);
    const int bottom = std::min(400, static_cast<int>(std::ceil(y + ry)) + 1);
    const int left = std::max(0, static_cast<int>(std::floor(x - rx)) - 1);
    const int right = std::min(640, static_cast<int>(std::ceil(x + rx)) + 1);
    for (int row = top; row < bottom; ++row) {
        for (int column = left; column < right;) {
            const int alpha = coverage[row * 640 + column];
            int end = column + 1;
            while (end < right && coverage[row * 640 + end] == alpha) ++end;
            EmitRect(column, row, end - column, 1, color, alpha);
            column = end;
        }
    }
}

void NativeRender::World(const Model& model, double wallMilliseconds) {
    const auto& level = model._level;
    const auto& camera = model._camera;
    const double ox = -std::floor(camera.x + 0.5);
    const double oy = -std::floor(camera.y + 0.5);
    const int minColumn = std::max(0, static_cast<int>(std::floor(camera.x / 32)) - 1);
    const int maxColumn = std::min(level.cols - 1, static_cast<int>(std::ceil((camera.x + 640) / 32)) + 1);
    const int minRow = std::max(0, static_cast<int>(std::floor(camera.y / 32)) - 1);
    const int maxRow = std::min(level.rows - 1, static_cast<int>(std::ceil((camera.y + 400) / 32)) + 1);
    static const std::array<const char*, 5> tileMatrices{"", "TILE_SOLID_MAT", "TILE_PLATFORM_MAT", "SPIKE_MAT", "FLAG_MAT"};
    for (int row = minRow; row <= maxRow; ++row) {
        for (int column = minColumn; column <= maxColumn; ++column) {
            const int tile = level.TileAt(column, row);
            if (tile <= 0 || tile >= static_cast<int>(tileMatrices.size())) continue;
            const double x = column * 32 + ox, y = row * 32 + oy;
            if (tile == 1) Rect(x, y, 32, 32, m_palette[9]);
            Pixels(tileMatrices[tile], x, y);
        }
    }
    const double bob = std::sin(level._t * 3) * 4;
    for (const auto& gem : level.gems) {
        if (!gem.collected)
            Pixels("GEM_MAT", gem.x * 32 + 16 - 11.2 + ox, gem.y * 32 + 16 + bob - 11.2 + oy, 22.4, 22.4);
    }
    const double powerSize = 32 * (0.85 + 0.15 * std::sin(level._t * 4));
    for (const auto& power : level.powerUps) {
        if (!power.collected)
            Pixels("PRISMA_MAT", power.x * 32 + 16 - powerSize / 2 + ox,
                   power.y * 32 + 16 - powerSize / 2 + oy, powerSize, powerSize);
    }
    for (const auto& enemy : model._enemies) {
        if (!enemy.alive) continue;
        if (enemy.type == "jump") {
            Pixels(enemy._onGround ? "ENEMY_JUMP_SQUAT" : "ENEMY_JUMP", enemy.x + ox, enemy.y + oy, enemy.w, enemy.h);
        } else {
            Pixels(enemy._walkFrame == 1 ? "ENEMY_GROUND2" : "ENEMY_GROUND", enemy.x + ox, enemy.y + oy,
                   enemy.w, enemy.h, enemy.vx < 0);
        }
    }
    const auto& player = model._player;
    if (!player.alive || (player._iframes > 0 && static_cast<int>(std::floor(player._iframes * 10)) % 2 == 0)) return;
    const auto sprite = player.prismaMode ? "PLAYER_PRISMA" :
        (player._walkFrame == 1 ? "PLAYER_WALK2_NORMAL" : "PLAYER_NORMAL");
    Pixels(sprite, player.x + ox, player.y + oy, player.w, player.h, player._facingLeft);
    if (player.prismaMode) {
        auto color = m_palette[15];
        color.a = static_cast<float>(0.18 + 0.07 * std::sin(wallMilliseconds * 0.01));
        Aura(player.x + player.w / 2 + ox, player.y + player.h - 4 + oy, player.w * 0.6, 5, color);
    }
}

void NativeRender::Hud(const Model& model) {
    Rect(0, 0, 640, 36, {0, 0, 0, 0.55f});
    Text("SCORE " + Padded(model._score, 6), 8, 22, 14, m_palette[10]);
    Text("♥ ×" + std::to_string(model._lives), 200, 22, 14, m_palette[11], "right");
    Text(model._level.name, 320, 22, 14, m_palette[5], "center");
    const int time = static_cast<int>(std::ceil(model._timeLeft));
    Text("TIME " + Padded(time, 3), 540, 22, 14, m_palette[time < 30 ? 11 : 6]);
    if (model.muted) Text("M", 620, 22, 14, m_palette[7]);
    if (model._player.prismaMode) {
        Rect(8, 30, 120, 6, {0, 0, 0, 0.4f});
        Rect(8, 30, 120 * model._player._prismaTimer / 15, 6, m_palette[15]);
    }
}

void NativeRender::Overlays(const Model& model) {
    if (model._state == "playing") return;
    if (model._state == "loading") {
        Text("LOADING…", 320, 200, 24, m_palette[10], "center");
        return;
    }
    Rect(0, 0, 640, 400, {0, 0, 0, model._state == "gameOver" ? 0.85f : 0.6f});
    if (model._state == "paused") {
        Text("PAUSED", 320, 180, 28, m_palette[10], "center");
        Text("Press P or Enter to continue", 320, 220, 16, m_palette[5], "center");
    } else if (model._state == "dead") {
        Text(model._lives > 0 ? "YOU DIED!" : "GAME OVER", 320, 180, 28, m_palette[11], "center");
        const auto summary = model._lives > 0 ? "Lives remaining: " + std::to_string(model._lives) + "  –  Press Enter to retry" :
            "Press Enter to restart";
        Text(summary, 320, 220, 14, m_palette[5], "center");
    } else if (model._state == "levelComplete") {
        Text("LEVEL COMPLETE!", 320, 180, 28, m_palette[14], "center");
        Text("Score: " + std::to_string(model._score) + "  –  Press Enter to continue", 320, 220, 14, m_palette[5], "center");
    } else if (model._state == "gameOver") {
        Text(model._victory ? "YOU WIN!" : "GAME OVER", 320, 160, 32, m_palette[model._victory ? 10 : 11], "center");
        Text(std::string(model._victory ? "Final Score: " : "Score: ") + std::to_string(model._score),
             320, 200, 20, m_palette[model._victory ? 14 : 5], "center");
        Text("Press Enter to play again", 320, 240, 16, m_palette[7], "center");
    }
}

void NativeRender::Render(Renderer2D& renderer, const Model& model, int width, int height, double wallMilliseconds) {
    if (!m_ready || width <= 0 || height <= 0) return;
    m_renderer = &renderer;
    m_scale = std::min(width / 640.0, height / 400.0);
    m_offset = glm::dvec2((width - 640 * m_scale) / 2, (height - 400 * m_scale) / 2);
    m_layer = 0;
    renderer.DrawRect({0, 0}, {width, height}, {0, 0, 0, 1}, m_layer);
    m_rasterizing = true;
    Rect(0, 0, 640, 400, m_palette[1]);
    constexpr std::array<glm::ivec2, 20> stars{{
        {40,20},{90,45},{150,15},{220,60},{300,30},{380,50},{450,18},
        {510,40},{570,25},{620,55},{60,80},{130,70},{250,85},{420,75},
        {550,65},{180,35},{330,70},{480,30},{600,80},{20,90}
    }};
    for (const auto& star : stars) Rect(star.x, star.y, 2, 2, m_palette[5]);
    if (model._state != "loading") {
        const double offsetX = -model._camera.x * 0.2;
        Mountains(offsetX, 220, 180, 110, m_palette[2]);
        Mountains(offsetX + 60, 260, 120, 80, m_palette[3]);
        World(model, wallMilliseconds);
        Hud(model);
    }
    Overlays(model);
    m_rasterizing = false;
    EmitCanvas();
}

void NativeRender::EmitCanvas() {
    struct Run {
        int x, y, width, height;
        uint32_t color;
    };
    std::vector<Run> runs;
    runs.reserve(6000);
    std::unordered_map<uint64_t, size_t> previous;
    for (int y = 0; y < 400; ++y) {
        std::unordered_map<uint64_t, size_t> current;
        for (int x = 0; x < 640;) {
            const auto color = m_pixels[y * 640 + x];
            int end = x + 1;
            while (end < 640 && m_pixels[y * 640 + end] == color) ++end;
            const int width = end - x;
            const uint64_t key = (static_cast<uint64_t>(color) << 20) |
                                 (static_cast<uint64_t>(width) << 10) | static_cast<uint64_t>(x);
            const auto found = previous.find(key);
            if (found != previous.end()) {
                ++runs[found->second].height;
                current[key] = found->second;
            } else {
                current[key] = runs.size();
                runs.push_back({x, y, width, 1, color});
            }
            x = end;
        }
        previous = std::move(current);
    }
    for (const auto& run : runs) {
        const glm::vec4 color(((run.color >> 16) & 255) / 255.0f,
                              ((run.color >> 8) & 255) / 255.0f,
                              (run.color & 255) / 255.0f, 1);
        EmitRect(run.x, run.y, run.width, run.height, color);
    }
    m_lastQuadCount = static_cast<uint32_t>(runs.size() + 1);
}

} // namespace PixelHop
