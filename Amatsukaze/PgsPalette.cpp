#include "PgsPalette.h"
#include "PgsSimd.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <unordered_map>

namespace amatsukaze::pgs {
namespace {

constexpr size_t MaxVisibleColors = 255;
constexpr double FixedScale = 1024.0;

uint32_t ColorKey(const Rgba& color)
{
    return (uint32_t(color.r) << 24) | (uint32_t(color.g) << 16)
        | (uint32_t(color.b) << 8) | uint32_t(color.a);
}

// 透明度の小さい画素のRGB差を過大評価しないよう、距離は合成後の色で測る。
std::array<double, 4> Coordinates(const Rgba& color)
{
    const double opacity = color.a / 255.0;
    return { color.r * opacity, color.g * opacity, color.b * opacity, double(color.a) };
}

struct ColorSample {
    Rgba color;
    std::array<double, 4> position;
    size_t count;
};

struct ColorBox {
    size_t begin, end;
    int axis;
    double score;
};

ColorBox MeasureBox(size_t begin, size_t end, const std::vector<size_t>& order,
    const std::vector<ColorSample>& samples)
{
    std::array<double, 4> lower, upper;
    lower.fill(std::numeric_limits<double>::max());
    upper.fill(std::numeric_limits<double>::lowest());
    double weight = 0;
    for (size_t i = begin; i < end; ++i) {
        const auto& sample = samples[order[i]];
        weight += double(sample.count) * sample.color.a;
        for (size_t axis = 0; axis < 4; ++axis) {
            lower[axis] = std::min(lower[axis], sample.position[axis]);
            upper[axis] = std::max(upper[axis], sample.position[axis]);
        }
    }
    int axis = 0;
    for (int candidate = 1; candidate < 4; ++candidate) {
        if (upper[candidate] - lower[candidate] > upper[axis] - lower[axis]) axis = candidate;
    }
    const double extent = upper[axis] - lower[axis];
    return { begin, end, axis, end - begin > 1 ? extent * extent * weight : 0.0 };
}

uint8_t RoundedByte(double value)
{
    return static_cast<uint8_t>(std::clamp(std::lround(value), 0L, 255L));
}

std::vector<Rgba> ReduceColors(const std::vector<ColorSample>& samples)
{
    std::vector<size_t> order(samples.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::vector<ColorBox> boxes{ MeasureBox(0, order.size(), order, samples) };
    while (boxes.size() < MaxVisibleColors) {
        auto selected = std::max_element(boxes.begin(), boxes.end(),
            [](const ColorBox& lhs, const ColorBox& rhs) { return lhs.score < rhs.score; });
        if (selected->end - selected->begin <= 1) break;
        const ColorBox box = *selected;
        std::sort(order.begin() + box.begin, order.begin() + box.end,
            [&](size_t lhs, size_t rhs) {
                const double a = samples[lhs].position[box.axis];
                const double b = samples[rhs].position[box.axis];
                return a != b ? a < b : lhs < rhs;
            });
        double total = 0;
        for (size_t i = box.begin; i < box.end; ++i) {
            const auto& sample = samples[order[i]];
            total += double(sample.count) * sample.color.a;
        }
        double accumulated = 0;
        size_t middle = box.begin;
        do {
            const auto& sample = samples[order[middle++]];
            accumulated += double(sample.count) * sample.color.a;
        } while (middle < box.end - 1 && accumulated < total / 2);
        *selected = MeasureBox(box.begin, middle, order, samples);
        boxes.push_back(MeasureBox(middle, box.end, order, samples));
    }

    std::vector<Rgba> colors;
    colors.reserve(boxes.size());
    for (const auto& box : boxes) {
        std::array<double, 3> rgb{};
        double opacityWeight = 0, count = 0;
        for (size_t i = box.begin; i < box.end; ++i) {
            const auto& sample = samples[order[i]];
            const double weight = double(sample.count) * sample.color.a;
            rgb[0] += sample.color.r * weight;
            rgb[1] += sample.color.g * weight;
            rgb[2] += sample.color.b * weight;
            opacityWeight += weight;
            count += double(sample.count);
        }
        // RGBはアルファ重み付き平均、アルファは画素数平均でstraight表現を保つ。
        colors.push_back({ RoundedByte(rgb[0] / opacityWeight), RoundedByte(rgb[1] / opacityWeight),
            RoundedByte(rgb[2] / opacityWeight), RoundedByte(opacityWeight / count) });
    }
    return colors;
}


PaletteEntry ConvertColor(const Rgba& color, bool bt709)
{
    // FFmpegのpgssubdecが使う10bit固定小数点係数を逆に解く。
    // 近傍の整数YCbCrも比較し、係数と丸めによる往復誤差を小さくする。
    const auto fixed = [](double value) { return std::floor(value * FixedScale + 0.5); };
    const double yy = fixed(255.0 / 219.0);
    const double rr = fixed((bt709 ? 1.5747 : 1.40200) * 255.0 / 224.0);
    const double gb = fixed((bt709 ? 0.1873 : 0.34414) * 255.0 / 224.0);
    const double gr = fixed((bt709 ? 0.4682 : 0.71414) * 255.0 / 224.0);
    const double bb = fixed((bt709 ? 1.8556 : 1.77200) * 255.0 / 224.0);
    const double luminance = (color.g + gb / bb * color.b + gr / rr * color.r)
        / (1.0 + gb / bb + gr / rr);
    const int initialY = static_cast<int>(std::lround(16.0 + luminance * FixedScale / yy));
    const int initialCb = static_cast<int>(std::lround(128.0 + (color.b - luminance) * FixedScale / bb));
    const int initialCr = static_cast<int>(std::lround(128.0 + (color.r - luminance) * FixedScale / rr));
    int bestMaximum = std::numeric_limits<int>::max();
    int bestSquared = std::numeric_limits<int>::max();
    PaletteEntry entry{ color, 16, 128, 128 };
    const auto decode = [](double value) {
        return std::clamp(static_cast<int>(std::floor((value + FixedScale / 2) / FixedScale)), 0, 255);
    };
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dcb = -1; dcb <= 1; ++dcb) {
            for (int dcr = -1; dcr <= 1; ++dcr) {
                const int y = std::clamp(initialY + dy, 16, 235);
                const int cb = std::clamp(initialCb + dcb, 16, 240);
                const int cr = std::clamp(initialCr + dcr, 16, 240);
                const double base = (y - 16) * yy;
                const int dr = decode(base + rr * (cr - 128)) - color.r;
                const int dg = decode(base - gb * (cb - 128) - gr * (cr - 128)) - color.g;
                const int db = decode(base + bb * (cb - 128)) - color.b;
                const int maximum = std::max({ std::abs(dr), std::abs(dg), std::abs(db) });
                const int squared = dr * dr + dg * dg + db * db;
                if (maximum < bestMaximum || (maximum == bestMaximum && squared < bestSquared)) {
                    bestMaximum = maximum;
                    bestSquared = squared;
                    entry.y = static_cast<uint8_t>(y);
                    entry.cb = static_cast<uint8_t>(cb);
                    entry.cr = static_cast<uint8_t>(cr);
                }
            }
        }
    }
    return entry;
}

}

PaletteResult MakePalette(const std::vector<std::vector<Rgba>>& images,
    const std::vector<std::pair<int, int>>& sizes, int canvasHeight)
{
    if (images.size() != sizes.size() || canvasHeight <= 0) {
        throw std::invalid_argument("PGSパレットの画像数またはキャンバス高さが不正です");
    }
    std::vector<ColorSample> samples;
    std::unordered_map<uint32_t, size_t> lookup;
    for (size_t i = 0; i < images.size(); ++i) {
        const auto [width, height] = sizes[i];
        if (width <= 0 || height <= 0 || size_t(width) > std::numeric_limits<size_t>::max() / size_t(height)
            || images[i].size() != size_t(width) * size_t(height)) {
            throw std::invalid_argument("PGSパレットの画像寸法と画素数が一致しません");
        }
        // 字幕の塗りつぶし領域では直前の色を再利用し、ハッシュ探索を省く。
        uint32_t previousKey = 0;
        size_t previousSample = 0;
        for (const auto& color : images[i]) {
            if (color.a == 0) continue;
            const uint32_t key = ColorKey(color);
            if (key != previousKey) {
                const auto found = lookup.find(key);
                if (found != lookup.end()) {
                    previousSample = found->second;
                } else {
                    previousSample = samples.size();
                    lookup.emplace(key, previousSample);
                    samples.push_back({ color, Coordinates(color), 0 });
                }
                previousKey = key;
            }
            ++samples[previousSample].count;
        }
    }

    std::vector<Rgba> colors;
    if (samples.size() <= MaxVisibleColors) {
        for (const auto& sample : samples) colors.push_back(sample.color);
    } else {
        colors = ReduceColors(samples);
    }
    PaletteResult result;
    result.entries.push_back(ConvertColor({ 0, 0, 0, 0 }, canvasHeight > 576));
    for (const auto& color : colors) result.entries.push_back(ConvertColor(color, canvasHeight > 576));

    // 同じRGBAの最近傍探索は一度だけ行い、全画像で割り当てを共有する。
    std::vector<uint8_t> indices(samples.size());
    std::vector<std::array<double, 4>> positions;
    for (const auto& color : colors) positions.push_back(Coordinates(color));
#if AMT_PGS_X86
    if (samples.size() > MaxVisibleColors && simd::HasAvx2()) {
        alignas(32) std::array<std::array<double, 256>, 4> coordinates;
        for (size_t axis = 0; axis < 4; ++axis) {
            coordinates[axis].fill(std::numeric_limits<double>::infinity());
            for (size_t j = 0; j < positions.size(); ++j) coordinates[axis][j] = positions[j][axis];
        }
        for (size_t i = 0; i < samples.size(); ++i) {
            indices[i] = simd::FindNearestColorAvx2(samples[i].position, coordinates, positions.size());
        }
    } else
#endif
    for (size_t i = 0; i < samples.size(); ++i) {
        if (samples.size() <= MaxVisibleColors) {
            indices[i] = static_cast<uint8_t>(i + 1);
            continue;
        }
        double best = std::numeric_limits<double>::max();
        for (size_t j = 0; j < positions.size(); ++j) {
            double distance = 0;
            for (size_t axis = 0; axis < 4; ++axis) {
                const double difference = samples[i].position[axis] - positions[j][axis];
                distance += difference * difference;
            }
            if (distance < best) {
                best = distance;
                indices[i] = static_cast<uint8_t>(j + 1);
            }
        }
    }
    result.images.reserve(images.size());
    for (size_t i = 0; i < images.size(); ++i) {
        IndexedImage image{ sizes[i].first, sizes[i].second, {} };
        image.pixels.resize(images[i].size());
        uint32_t previousKey = 0;
        uint8_t previousIndex = 0;
        for (size_t offset = 0; offset < images[i].size(); ++offset) {
            const auto& color = images[i][offset];
            if (color.a == 0) continue;
            const uint32_t key = ColorKey(color);
            if (key != previousKey) {
                previousIndex = indices[lookup.at(key)];
                previousKey = key;
            }
            image.pixels[offset] = previousIndex;
        }
        result.images.push_back(std::move(image));
    }
    return result;
}

}
