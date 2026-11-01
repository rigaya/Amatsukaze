#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace logo {

// denpaのlogo-area.ts: 上位2%では中身の滲みを拾ったため、上位1%だけを縁とする。
inline constexpr double kEdgeAreaTop = 0.01;
inline constexpr double kEdgeAreaMiddleQuantile = 0.5;
// 出現率の下限は案Fの試算値。denpaの比3倍は、ロゴなしでも上位1%が取れることへの対策。
inline constexpr float kEdgeAreaFloor = 0.15f;
inline constexpr float kEdgeAreaRatio = 3.0f;
// denpaでは静止背景との競合を比だけで分離できず、強い縁の65%以上を必要とした。
inline constexpr double kEdgeAreaShare = 0.65;
// 文字の画同士を繋ぐ4画素と、Sobelが縁の外側に出す1画素を後で戻す。
inline constexpr int kEdgeAreaDilate = 4;
inline constexpr int kEdgeAreaSobelBorder = 1;
// denpaの1440幅で75×30のロゴを含み、テロップの帯を除くフレーム比率。
inline constexpr double kEdgeAreaMinWidth = 0.02;
inline constexpr double kEdgeAreaMaxWidth = 0.15;
inline constexpr double kEdgeAreaMinHeight = 0.015;
inline constexpr double kEdgeAreaMaxHeight = 0.10;
// テレビ朝日の図形と文字を括るための距離と、付随する塊に必要な縁の割合。
inline constexpr int kEdgeAreaMergeGap = 16;
inline constexpr double kEdgeAreaMergeShare = 0.1;
// BS日テレで外接枠に滲みが残ったため、各辺から強い縁を2.5%ずつ捨てて締める。
inline constexpr double kEdgeAreaTrim = 0.025;

struct EdgeAreaRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

enum class EdgeAreaHoldReason { None, InvalidInput, WeakEdge, NoFittingBlob, InsufficientShare };

inline const char* EdgeAreaHoldReasonText(const EdgeAreaHoldReason reason) {
    switch (reason) {
    case EdgeAreaHoldReason::None: return "なし";
    case EdgeAreaHoldReason::InvalidInput: return "入力不正";
    case EdgeAreaHoldReason::WeakEdge: return "縁の強さ不足";
    case EdgeAreaHoldReason::NoFittingBlob: return "サイズ内の塊なし";
    case EdgeAreaHoldReason::InsufficientShare: return "独占率不足";
    }
    return "不明";
}

struct EdgeAreaResult {
    bool accepted = false;
    float middle = 0.0f;
    float limit = 0.0f;
    EdgeAreaRect selected;
    EdgeAreaRect merged;
    EdgeAreaRect rect;
    size_t selectedStrong = 0;
    size_t mergedStrong = 0;
    size_t rivals = 0;
    double share = 0.0;
    EdgeAreaHoldReason reason = EdgeAreaHoldReason::InvalidInput;
};

namespace edge_area_detail {

struct Blob {
    EdgeAreaRect rect;
    size_t strong = 0;
};

inline EdgeAreaRect Deflate(const EdgeAreaRect& rect) {
    constexpr int kDeflate = kEdgeAreaDilate + kEdgeAreaSobelBorder;
    const int w = std::max(1, rect.w - kDeflate * 2);
    const int h = std::max(1, rect.h - kDeflate * 2);
    // 非負値のMath.roundと同じく、半画素の端数は大きい側へ丸める。
    return { rect.x + (rect.w - w + 1) / 2, rect.y + (rect.h - h + 1) / 2, w, h };
}

inline std::vector<Blob> Blobs(const std::vector<uint8_t>& strong, const int width, const int height) {
    std::vector<uint8_t> mask(strong.size(), 0);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            if (!strong[x + size_t(y) * width]) continue;
            for (int yy = std::max(0, y - kEdgeAreaDilate); yy <= std::min(height - 1, y + kEdgeAreaDilate); yy++) {
                for (int xx = std::max(0, x - kEdgeAreaDilate); xx <= std::min(width - 1, x + kEdgeAreaDilate); xx++) {
                    mask[xx + size_t(yy) * width] = 1;
                }
            }
        }
    }
    std::vector<Blob> found;
    std::vector<size_t> stack;
    for (size_t start = 0; start < mask.size(); start++) {
        if (!mask[start]) continue;
        mask[start] = 0;
        stack.push_back(start);
        int minX = width, minY = height, maxX = -1, maxY = -1;
        size_t own = 0;
        while (!stack.empty()) {
            const size_t at = stack.back();
            stack.pop_back();
            const int x = int(at % width), y = int(at / width);
            own += strong[at];
            minX = std::min(minX, x); maxX = std::max(maxX, x);
            minY = std::min(minY, y); maxY = std::max(maxY, y);
            const auto visit = [&](const size_t next) {
                if (!mask[next]) return;
                mask[next] = 0;
                stack.push_back(next);
            };
            // denpaと同じ上下左右の連結。斜めの縁へ橋を架けない。
            if (x > 0) visit(at - 1);
            if (x < width - 1) visit(at + 1);
            if (y > 0) visit(at - width);
            if (y < height - 1) visit(at + width);
        }
        found.push_back({ Deflate({ minX, minY, maxX - minX + 1, maxY - minY + 1 }), own });
    }
    return found;
}

inline int Gap(const EdgeAreaRect& a, const EdgeAreaRect& b) {
    const int dx = std::max(0, std::max(a.x, b.x) - std::min(a.x + a.w, b.x + b.w));
    const int dy = std::max(0, std::max(a.y, b.y) - std::min(a.y + a.h, b.y + b.h));
    return std::max(dx, dy);
}

inline EdgeAreaRect Union(const EdgeAreaRect& a, const EdgeAreaRect& b) {
    const int x = std::min(a.x, b.x), y = std::min(a.y, b.y);
    return { x, y, std::max(a.x + a.w, b.x + b.w) - x, std::max(a.y + a.h, b.y + b.h) - y };
}

inline EdgeAreaRect Trim(const std::vector<uint8_t>& mask, const int width, const EdgeAreaRect& rect) {
    std::vector<size_t> cols(rect.w, 0), rows(rect.h, 0);
    size_t total = 0;
    for (int y = 0; y < rect.h; y++) {
        for (int x = 0; x < rect.w; x++) {
            if (!mask[rect.x + x + size_t(rect.y + y) * width]) continue;
            cols[x]++; rows[y]++; total++;
        }
    }
    const auto span = [&](const std::vector<size_t>& counts) {
        const double cut = total * kEdgeAreaTrim;
        int from = 0, to = int(counts.size()) - 1;
        size_t sum = 0;
        for (; from < to && sum + counts[from] <= cut; from++) sum += counts[from];
        sum = 0;
        for (; to > from && sum + counts[to] <= cut; to--) sum += counts[to];
        return std::pair<int, int>{ from, to };
    };
    const auto xs = span(cols), ys = span(rows);
    return { rect.x + xs.first, rect.y + ys.first, xs.second - xs.first + 1, ys.second - ys.first + 1 };
}

} // 名前空間edge_area_detail

// 走査領域の座標で余白なしの枠と診断値を返す。入出力以外の状態や環境変数には触れない。
inline EdgeAreaResult PickEdgeArea(const std::vector<float>& strength, const int width, const int height,
    const int frameWidth, const int frameHeight) {
    EdgeAreaResult result;
    if (width <= 0 || height <= 0 || frameWidth < width || frameHeight < height
        || strength.size() != size_t(width) * height) return result;
    for (const float value : strength) {
        if (!std::isfinite(value) || value < 0.0f) return result;
    }
    std::vector<float> sorted = strength;
    std::sort(sorted.begin(), sorted.end());
    // denpaのfloor(N*q)を踏襲し、既存の行帯拡張のfloor((N-1)*q)とは区別する。
    result.middle = sorted[size_t(sorted.size() * kEdgeAreaMiddleQuantile)];
    result.limit = sorted[size_t(sorted.size() * (1.0 - kEdgeAreaTop))];
    if (result.limit < kEdgeAreaFloor || result.limit < result.middle * kEdgeAreaRatio) {
        result.reason = EdgeAreaHoldReason::WeakEdge;
        return result;
    }
    std::vector<uint8_t> mask(strength.size(), 0);
    for (size_t at = 0; at < strength.size(); at++) mask[at] = strength[at] >= result.limit;
    const auto found = edge_area_detail::Blobs(mask, width, height);
    const auto tooBig = [&](const EdgeAreaRect& r) {
        return r.w > frameWidth * kEdgeAreaMaxWidth || r.h > frameHeight * kEdgeAreaMaxHeight;
    };
    const auto fits = [&](const EdgeAreaRect& r) {
        return !tooBig(r) && r.w >= frameWidth * kEdgeAreaMinWidth && r.h >= frameHeight * kEdgeAreaMinHeight;
    };
    const edge_area_detail::Blob* best = nullptr;
    for (const auto& blob : found) {
        if (tooBig(blob.rect)) continue;
        result.rivals += blob.strong;
        if (fits(blob.rect) && (best == nullptr || blob.strong > best->strong)) best = &blob;
    }
    if (best == nullptr) {
        result.reason = EdgeAreaHoldReason::NoFittingBlob;
        return result;
    }
    result.selected = result.merged = best->rect;
    result.selectedStrong = result.mergedStrong = best->strong;
    // 最初に選んだ塊の縁数を基準とし、括った枠との距離を走査順に評価する。
    for (const auto& blob : found) {
        if (&blob == best || tooBig(blob.rect) || blob.strong < best->strong * kEdgeAreaMergeShare) continue;
        if (edge_area_detail::Gap(result.merged, blob.rect) > kEdgeAreaMergeGap) continue;
        const auto merged = edge_area_detail::Union(result.merged, blob.rect);
        if (!fits(merged)) continue;
        result.merged = merged;
        result.mergedStrong += blob.strong;
    }
    result.share = result.rivals ? double(result.mergedStrong) / result.rivals : 0.0;
    if (result.mergedStrong < result.rivals * kEdgeAreaShare) {
        result.reason = EdgeAreaHoldReason::InsufficientShare;
        return result;
    }
    result.rect = edge_area_detail::Trim(mask, width, result.merged);
    result.accepted = true;
    result.reason = EdgeAreaHoldReason::None;
    return result;
}

} // 名前空間logo
