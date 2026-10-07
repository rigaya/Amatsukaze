#include "PgsEncoder.h"
#include "PgsSimd.h"

#include <algorithm>
#include <fstream>
#include <future>
#include <thread>
#include <limits>
#include <stdexcept>
#include <utility>

#if AMT_PGS_X86 && defined(_MSC_VER)
#include <intrin.h>
#endif

namespace amatsukaze::pgs {
namespace simd {
bool HasAvx2() {
    static const bool available = [] {
#if defined(AMT_PGS_DISABLE_AVX2)
        return false;
#elif AMT_PGS_X86 && defined(_MSC_VER)
        int regs[4];
        __cpuid(regs, 0);
        if (regs[0] < 7) return false;
        __cpuidex(regs, 1, 0);
        // CPUのAVX対応とOSによるYMMレジスタ保存を両方確認する。
        if ((regs[2] & 0x18000000) != 0x18000000 || (_xgetbv(0) & 6) != 6) return false;
        __cpuidex(regs, 7, 0);
        return (regs[1] & (1 << 5)) != 0;
#elif AMT_PGS_X86 && (defined(__GNUC__) || defined(__clang__))
        return bool(__builtin_cpu_supports("avx2"));
#else
        return false;
#endif
    }();
    return available;
}
}
namespace {

constexpr int MaxObjectDimension = 4096;
constexpr int MaxCanvasDimension = MaxObjectDimension;
constexpr size_t MaxSegmentPayload = 65535;
constexpr size_t MaxObjectData = 0xffffff;
constexpr int MaxRunLength = 16383;
constexpr uint8_t Pcs = 0x16;
constexpr uint8_t Wds = 0x17;
constexpr uint8_t Pds = 0x14;
constexpr uint8_t Ods = 0x15;
constexpr uint8_t End = 0x80;

void Byte(std::vector<uint8_t>& dst, uint8_t value) { dst.push_back(value); }
void Be16(std::vector<uint8_t>& dst, int value) {
    Byte(dst, static_cast<uint8_t>(value >> 8));
    Byte(dst, static_cast<uint8_t>(value));
}
void Be24(std::vector<uint8_t>& dst, size_t value) {
    Byte(dst, static_cast<uint8_t>(value >> 16));
    Be16(dst, static_cast<int>(value & 0xffff));
}
void Be32(std::vector<uint8_t>& dst, uint32_t value) {
    Be16(dst, static_cast<int>(value >> 16));
    Be16(dst, static_cast<int>(value & 0xffff));
}
void Segment(std::vector<uint8_t>& dst, int64_t pts, uint8_t type, const std::vector<uint8_t>& payload) {
    if (payload.size() > MaxSegmentPayload) throw std::length_error("PGSセグメントが長すぎます");
    Byte(dst, 'P'); Byte(dst, 'G');
    Be32(dst, static_cast<uint32_t>(pts));
    Be32(dst, 0);
    Byte(dst, type);
    Be16(dst, static_cast<int>(payload.size()));
    dst.insert(dst.end(), payload.begin(), payload.end());
}

size_t Area(int width, int height) {
    if (width <= 0 || height <= 0 || static_cast<size_t>(width) > (std::numeric_limits<size_t>::max)() / height)
        throw std::invalid_argument("PGS画像サイズが不正です");
    return static_cast<size_t>(width) * height;
}

struct Bounds {
    int left, top, right, bottom;
};
Bounds Box(const Region& region) {
    return {region.x, region.y, region.x + region.width, region.y + region.height};
}
Bounds Union(const Bounds& a, const Bounds& b) {
    return {(std::min)(a.left, b.left), (std::min)(a.top, b.top),
        (std::max)(a.right, b.right), (std::max)(a.bottom, b.bottom)};
}
bool Overlaps(const Bounds& a, const Bounds& b) {
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}


Region Trim(const Region& region) {
    int left = region.width, top = region.height, right = 0, bottom = 0;
#if AMT_PGS_X86
    if (simd::HasAvx2()) {
        const auto bounds = simd::AlphaBoundsAvx2(region);
        left = bounds.left; top = bounds.top; right = bounds.right; bottom = bounds.bottom;
    } else
#endif
    {
        for (int y = 0; y < region.height; y++) {
            for (int x = 0; x < region.width; x++) {
                if (region.pixels[static_cast<size_t>(y) * region.width + x].a != 0) {
                    left = (std::min)(left, x); top = (std::min)(top, y);
                    right = (std::max)(right, x + 1); bottom = (std::max)(bottom, y + 1);
                }
            }
        }
    }
    if (right == 0) return {};
    Region out{region.x + left, region.y + top, right - left, bottom - top, {}};
    out.pixels.reserve(Area(out.width, out.height));
    for (int y = top; y < bottom; y++) {
        const auto begin = region.pixels.begin() + static_cast<size_t>(y) * region.width + left;
        out.pixels.insert(out.pixels.end(), begin, begin + out.width);
    }
    return out;
}

Rgba Blend(const Rgba& dst, const Rgba& src) {
    if (src.a == 0) return dst;
    if (src.a == 255 || dst.a == 0) return src;
    // straightアルファのsource-over。除算前は255倍の精度を保持する。
    const uint32_t srcWeight = static_cast<uint32_t>(src.a) * 255;
    const uint32_t dstWeight = static_cast<uint32_t>(dst.a) * (255 - src.a);
    const uint32_t alpha = srcWeight + dstWeight;
    const auto channel = [=](uint8_t d, uint8_t s) {
        return static_cast<uint8_t>((s * srcWeight + d * dstWeight + alpha / 2) / alpha);
    };
    return {channel(dst.r, src.r), channel(dst.g, src.g), channel(dst.b, src.b),
        static_cast<uint8_t>((alpha + 127) / 255)};
}

struct Group {
    Bounds box;
    std::vector<size_t> members;
};
void Merge(std::vector<Group>& groups, size_t a, size_t b) {
    groups[a].box = Union(groups[a].box, groups[b].box);
    groups[a].members.insert(groups[a].members.end(), groups[b].members.begin(), groups[b].members.end());
    groups.erase(groups.begin() + b);
}

std::vector<Region> Prepare(const Event& event, int canvasWidth, int canvasHeight) {
    std::vector<Region> regions;
    for (const auto& region : event.regions) {
        const size_t area = Area(region.width, region.height);
        if (region.x < 0 || region.y < 0 || region.width > canvasWidth || region.height > canvasHeight ||
            region.x > canvasWidth - region.width || region.y > canvasHeight - region.height ||
            region.pixels.size() != area)
            throw std::invalid_argument("PGS領域がキャンバス外か画像サイズが不正です");
        auto trimmed = Trim(region);
        if (trimmed.width != 0) regions.push_back(std::move(trimmed));
    }

    std::vector<Group> groups;
    for (size_t i = 0; i < regions.size(); i++) groups.push_back({Box(regions[i]), {i}});
    while (groups.size() > 1) {
        bool merged = false;
        for (size_t a = 0; a < groups.size() && !merged; a++) {
            for (size_t b = a + 1; b < groups.size(); b++) {
                if (Overlaps(groups[a].box, groups[b].box)) {
                    Merge(groups, a, b); merged = true; break;
                }
            }
        }
        if (merged) continue;
        if (groups.size() <= 2) break;

        // 矩形間の距離が最小の組をまとめ、同距離なら外接矩形が小さい組を選ぶ。
        size_t bestA = 0, bestB = 1;
        int64_t bestDistance = (std::numeric_limits<int64_t>::max)();
        int64_t bestArea = bestDistance;
        for (size_t a = 0; a < groups.size(); a++) {
            for (size_t b = a + 1; b < groups.size(); b++) {
                const auto& first = groups[a].box;
                const auto& second = groups[b].box;
                const int64_t dx = (std::max)({0, first.left - second.right, second.left - first.right});
                const int64_t dy = (std::max)({0, first.top - second.bottom, second.top - first.bottom});
                const int64_t distance = dx * dx + dy * dy;
                const auto united = Union(first, second);
                const int64_t area = static_cast<int64_t>(united.right - united.left) * (united.bottom - united.top);
                if (distance < bestDistance || (distance == bestDistance && area < bestArea)) {
                    bestDistance = distance; bestArea = area; bestA = a; bestB = b;
                }
            }
        }
        Merge(groups, bestA, bestB);
    }

    std::vector<Region> result;
    for (auto& group : groups) {
        // 単独の領域は合成し直す必要がない。透明画素のRGBはパレット化時に無視される。
        if (group.members.size() == 1) {
            result.push_back(std::move(regions[group.members.front()]));
            continue;
        }
        Region out{group.box.left, group.box.top, group.box.right - group.box.left, group.box.bottom - group.box.top, {}};
        if (out.width > MaxObjectDimension || out.height > MaxObjectDimension)
            throw std::length_error("PGSオブジェクトは幅・高さ4096以下である必要があります");
        out.pixels.resize(Area(out.width, out.height), Rgba{0, 0, 0, 0});
        // 結合順にかかわらず、元の領域順で描画して前後関係を保つ。
        std::sort(group.members.begin(), group.members.end());
        for (const auto index : group.members) {
            const auto& source = regions[index];
            for (int y = 0; y < source.height; y++) {
                for (int x = 0; x < source.width; x++) {
                    auto& dst = out.pixels[static_cast<size_t>(y + source.y - out.y) * out.width + x + source.x - out.x];
                    dst = Blend(dst, source.pixels[static_cast<size_t>(y) * source.width + x]);
                }
            }
        }
        result.push_back(std::move(out));
    }
    return result;
}

void Composition(std::vector<uint8_t>& dst, int64_t pts, int width, int height, uint16_t number,
    bool epoch, const std::vector<Region>& regions) {
    std::vector<uint8_t> pcs;
    Be16(pcs, width); Be16(pcs, height);
    Byte(pcs, 0x10); Be16(pcs, number); Byte(pcs, epoch ? 0x80 : 0);
    // Normalは消去専用。オブジェクトを外してもエポックのウィンドウ定義は維持する。
    const size_t objectCount = epoch ? regions.size() : 0;
    Byte(pcs, 0); Byte(pcs, 0); Byte(pcs, static_cast<uint8_t>(objectCount));
    for (size_t i = 0; i < objectCount; i++) {
        Be16(pcs, static_cast<int>(i)); Byte(pcs, static_cast<uint8_t>(i)); Byte(pcs, 0);
        Be16(pcs, regions[i].x); Be16(pcs, regions[i].y);
    }
    Segment(dst, pts, Pcs, pcs);
    std::vector<uint8_t> wds{static_cast<uint8_t>(regions.size())};
    for (size_t i = 0; i < regions.size(); i++) {
        Byte(wds, static_cast<uint8_t>(i)); Be16(wds, regions[i].x); Be16(wds, regions[i].y);
        Be16(wds, regions[i].width); Be16(wds, regions[i].height);
    }
    Segment(dst, pts, Wds, wds);
}

void Object(std::vector<uint8_t>& dst, int64_t pts, int id, const IndexedImage& image) {
    const auto rle = PgsEncoder::EncodeRle(image.pixels, image.width, image.height);
    if (rle.size() > MaxObjectData - 4) throw std::length_error("PGSオブジェクトデータが長すぎます");
    size_t position = 0;
    bool first = true;
    while (position < rle.size()) {
        const size_t headerSize = first ? 11 : 4;
        const size_t count = (std::min)(MaxSegmentPayload - headerSize, rle.size() - position);
        const bool last = count == rle.size() - position;
        std::vector<uint8_t> ods;
        Be16(ods, id); Byte(ods, 0); Byte(ods, static_cast<uint8_t>((first ? 0x80 : 0) | (last ? 0x40 : 0)));
        if (first) { Be24(ods, rle.size() + 4); Be16(ods, image.width); Be16(ods, image.height); }
        ods.insert(ods.end(), rle.begin() + position, rle.begin() + position + count);
        Segment(dst, pts, Ods, ods);
        position += count; first = false;
    }
}

}

std::vector<uint8_t> PgsEncoder::EncodeRle(const std::vector<uint8_t>& pixels, int width, int height) {
    if (pixels.size() != Area(width, height)) throw std::invalid_argument("PGSインデックス画像サイズが不正です");
    std::vector<uint8_t> result;
    result.reserve(pixels.size() + static_cast<size_t>(height) * 2);
#if AMT_PGS_X86
    const bool avx2 = simd::HasAvx2();
#endif
    for (int y = 0; y < height; y++) {
        int x = 0;
        const size_t offset = static_cast<size_t>(y) * width;
        while (x < width) {
            const uint8_t color = pixels[offset + x];
            int length = 1;
            const int limit = (std::min)(MaxRunLength, width - x);
            // 短いランは関数呼び出しを避け、長い同色区間だけ32byteずつ走査する。
            while (length < limit && length < 8 && pixels[offset + x + length] == color) length++;
#if AMT_PGS_X86
            if (avx2 && length == 8 && limit >= 32) length = simd::RunLengthAvx2(pixels.data() + offset + x, limit);
            else
#endif
                while (length < limit && pixels[offset + x + length] == color) length++;
            if (color != 0 && length <= 2) {
                for (int i = 0; i < length; i++) Byte(result, color);
            } else {
                Byte(result, 0);
                const uint8_t flags = color != 0 ? 0x80 : 0;
                if (length < 64) Byte(result, static_cast<uint8_t>(flags | length));
                else { Byte(result, static_cast<uint8_t>(flags | 0x40 | (length >> 8))); Byte(result, static_cast<uint8_t>(length)); }
                if (color != 0) Byte(result, color);
            }
            x += length;
        }
        Byte(result, 0); Byte(result, 0);
    }
    return result;
}

std::vector<uint8_t> PgsEncoder::Encode(int canvasWidth, int canvasHeight, const std::vector<Event>& events) {
    if (canvasWidth <= 0 || canvasHeight <= 0 || canvasWidth > MaxCanvasDimension || canvasHeight > MaxCanvasDimension)
        throw std::invalid_argument("PGSキャンバスサイズが不正です");
    for (size_t i = 0; i < events.size(); i++) {
        if (events[i].start90k < 0 || events[i].end90k <= events[i].start90k ||
            (i != 0 && events[i].start90k < events[i - 1].end90k))
            throw std::invalid_argument("PGSイベントは非負時刻の時系列・非重複区間である必要があります");
    }

    const auto hasClear = [&](size_t index) {
        return index + 1 == events.size() || events[index + 1].start90k != events[index].end90k;
    };
    const auto encodeEvent = [&](size_t index, uint16_t number) {
        std::vector<uint8_t> chunk;
        const auto& event = events[index];
        const auto regions = Prepare(event, canvasWidth, canvasHeight);
        Composition(chunk, event.start90k, canvasWidth, canvasHeight, number++, true, regions);
        if (!regions.empty()) {
            std::vector<std::vector<Rgba>> images;
            std::vector<std::pair<int, int>> sizes;
            for (const auto& region : regions) { images.push_back(region.pixels); sizes.emplace_back(region.width, region.height); }
            const auto palette = MakePalette(images, sizes, canvasHeight);
            std::vector<uint8_t> pds{0, 0};
            for (size_t i = 0; i < palette.entries.size(); i++) {
                const auto& entry = palette.entries[i];
                Byte(pds, static_cast<uint8_t>(i)); Byte(pds, entry.y); Byte(pds, entry.cr); Byte(pds, entry.cb); Byte(pds, entry.rgba.a);
            }
            Segment(chunk, event.start90k, Pds, pds);
            for (size_t i = 0; i < palette.images.size(); i++) Object(chunk, event.start90k, static_cast<int>(i), palette.images[i]);
        }
        Segment(chunk, event.start90k, End, {});
        if (index + 1 == events.size() || events[index + 1].start90k != event.end90k) {
            Composition(chunk, event.end90k, canvasWidth, canvasHeight, number++, false, regions);
            Segment(chunk, event.end90k, End, {});
        }
        return chunk;
    };

    // 独立したイベントを最大4個のバッチに制限して処理し、一時画像のメモリ増加を抑える。
    size_t totalPixels = 0;
    for (const auto& event : events) for (const auto& region : event.regions) {
        const size_t remaining = 262144 - (std::min)(totalPixels, size_t(262144));
        totalPixels += (std::min)(region.pixels.size(), remaining);
    }
    const size_t workers = events.size() >= 2 && totalPixels >= 262144
        ? (std::min)(size_t(4), size_t((std::max)(1u, std::thread::hardware_concurrency()))) : 1;
    std::vector<uint8_t> result;
    uint16_t number = 0;
    for (size_t first = 0; first < events.size(); first += workers) {
        const size_t count = (std::min)(workers, events.size() - first);
        size_t batchPixels = 0;
        for (size_t index = first; index < first + count; index++) for (const auto& region : events[index].regions)
            batchPixels += (std::min)(region.pixels.size(), size_t(65536) - batchPixels);
        if (count == 1 || batchPixels < 65536) {
            for (size_t index = first; index < first + count; index++) {
                const auto chunk = encodeEvent(index, number);
                number = static_cast<uint16_t>(number + 1 + hasClear(index));
                result.insert(result.end(), chunk.begin(), chunk.end());
            }
            continue;
        }
        std::vector<std::future<std::vector<uint8_t>>> pending;
        pending.reserve(count);
        for (size_t index = first; index < first + count; index++) {
            pending.push_back(std::async(std::launch::async, encodeEvent, index, number));
            number = static_cast<uint16_t>(number + 1 + hasClear(index));
        }
        for (auto& task : pending) {
            const auto chunk = task.get();
            result.insert(result.end(), chunk.begin(), chunk.end());
        }
    }
    return result;
}

void PgsEncoder::WriteFile(const std::filesystem::path& path, int canvasWidth, int canvasHeight,
    const std::vector<Event>& events) {
    const auto data = Encode(canvasWidth, canvasHeight, events);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("PGS出力ファイルを開けません");
    // streamsizeの範囲を超えない単位で書き込み、close時の失敗も通知する。
    size_t position = 0;
    constexpr size_t ChunkSize = 1024 * 1024;
    while (position < data.size()) {
        const size_t count = (std::min)(ChunkSize, data.size() - position);
        file.write(reinterpret_cast<const char*>(data.data() + position), static_cast<std::streamsize>(count));
        if (!file) throw std::runtime_error("PGS出力ファイルを書き込めません");
        position += count;
    }
    file.close();
    if (!file) throw std::runtime_error("PGS出力ファイルを閉じられません");
}

}
