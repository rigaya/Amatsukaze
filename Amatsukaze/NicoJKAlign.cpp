#include "NicoJKAlign.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <locale>
#include <numeric>
#include <sstream>
#include <stdexcept>
extern "C" {
#include <libavutil/version.h>
#include <libavutil/mem.h>
#if LIBAVUTIL_VERSION_MAJOR >= 58
#include <libavutil/tx.h>
#else
#include <libavcodec/avfft.h>
#endif
}
namespace {
constexpr int SAMPLE_RATE = 8000, WINDOW = 512, HOP = 80, BANDS = 24, PROBE = 800, STEP = 2000;
constexpr int CORRELATION_FFT = 65536;
constexpr int CORRELATION_STEP = CORRELATION_FFT - PROBE + 1;
constexpr double MIN_RATIO = 1.5, SAME_DELTA = 0.05, PI = 3.14159265358979323846;
struct Complex {
    float re, im;
};
// 新旧FFmpegの実数FFTの違いだけを、この包みに閉じ込める。
class RealFFT {
    int length_;
#if LIBAVUTIL_VERSION_MAJOR >= 58
    AVTXContext* forward_ = nullptr;
    AVTXContext* inverse_ = nullptr;
    av_tx_fn forwardFunction_ = nullptr;
    av_tx_fn inverseFunction_ = nullptr;
#else
    RDFTContext* forward_ = nullptr;
    RDFTContext* inverse_ = nullptr;
    float* packed_ = nullptr;
#endif
  public:
    explicit RealFFT(int length) : length_(length) {
#if LIBAVUTIL_VERSION_MAJOR >= 58
        const float forwardScale = 1, inverseScale = 1.0f / length;
        if (av_tx_init(&forward_, &forwardFunction_, AV_TX_FLOAT_RDFT, 0, length, &forwardScale, AV_TX_UNALIGNED) < 0 ||
            av_tx_init(&inverse_, &inverseFunction_, AV_TX_FLOAT_RDFT, 1, length, &inverseScale, AV_TX_UNALIGNED) < 0) {
            av_tx_uninit(&forward_);
            av_tx_uninit(&inverse_);
            throw std::runtime_error(u8"実況照合用FFTの初期化に失敗しました");
        }
#else
        int bits = 0;
        while ((1 << bits) < length) {
            ++bits;
        }
        forward_ = av_rdft_init(bits, DFT_R2C);
        inverse_ = av_rdft_init(bits, IDFT_C2R);
        if (!forward_ || !inverse_) {
            if (forward_) {
                av_rdft_end(forward_);
            }
            if (inverse_) {
                av_rdft_end(inverse_);
            }
            throw std::runtime_error(u8"実況照合用FFTの初期化に失敗しました");
        }
        packed_ = (float*)av_malloc_array(length, sizeof(float));
        if (!packed_) {
            av_rdft_end(forward_);
            av_rdft_end(inverse_);
            throw std::bad_alloc();
        }
#endif
    }
    ~RealFFT() {
#if LIBAVUTIL_VERSION_MAJOR >= 58
        av_tx_uninit(&forward_);
        av_tx_uninit(&inverse_);
#else
        av_rdft_end(forward_);
        av_rdft_end(inverse_);
        av_free(packed_);
#endif
    }
    RealFFT(const RealFFT&) = delete;
    RealFFT& operator=(const RealFFT&) = delete;
    void forward(std::vector<float>& input, std::vector<Complex>& output) {
        output.resize(length_ / 2 + 1);
#if LIBAVUTIL_VERSION_MAJOR >= 58
        forwardFunction_(forward_, output.data(), input.data(), sizeof(float));
#else
        std::copy(input.begin(), input.end(), packed_);
        av_rdft_calc(forward_, packed_);
        output[0] = {packed_[0], 0};
        output[length_ / 2] = {packed_[1], 0};
        for (int i = 1; i < length_ / 2; ++i) {
            output[i] = {packed_[2 * i], packed_[2 * i + 1]};
        }
#endif
    }
    void inverse(std::vector<Complex>& input, std::vector<float>& output) {
        output.resize(length_);
#if LIBAVUTIL_VERSION_MAJOR >= 58
        inverseFunction_(inverse_, output.data(), input.data(), sizeof(Complex));
#else
        packed_[0] = input[0].re;
        packed_[1] = input[length_ / 2].re;
        for (int i = 1; i < length_ / 2; ++i) {
            packed_[2 * i] = input[i].re;
            packed_[2 * i + 1] = input[i].im;
        }
        av_rdft_calc(inverse_, packed_);
        for (int i = 0; i < length_; ++i) {
            output[i] = packed_[i] * (2.0f / length_);
        }
#endif
    }
};
using Features = std::array<std::vector<float>, BANDS>;
Features GetFeatures(const std::vector<float>& audio) {
    Features features;
    if (audio.size() <= WINDOW) {
        return features;
    }
    const size_t frames = (audio.size() - WINDOW) / HOP;
    for (auto& band : features) {
        band.resize(frames);
    }
    std::array<double, WINDOW> window;
    for (int i = 0; i < WINDOW; ++i) {
        window[i] = 0.5 - 0.5 * std::cos(2 * PI * i / (WINDOW - 1));
    }
    std::array<int, WINDOW / 2 + 1> bandIndex;
    bandIndex.fill(-1);
    for (int i = 0; i <= WINDOW / 2; ++i) {
        const double hz = i * (double)SAMPLE_RATE / WINDOW;
        for (int band = 0; band < BANDS; ++band) {
            const double lower = 100 * std::pow(38.0, band / (double)BANDS),
                         upper = 100 * std::pow(38.0, (band + 1) / (double)BANDS);
            if (hz >= lower && hz < upper) {
                bandIndex[i] = band;
                break;
            }
        }
    }
    RealFFT fft(WINDOW);
    std::vector<float> input(WINDOW);
    std::vector<Complex> spectrum;
    std::array<double, BANDS> previous = {};
    for (size_t frame = 0; frame < frames; ++frame) {
        for (int i = 0; i < WINDOW; ++i) {
            input[i] = (float)(audio[frame * HOP + i] * window[i]);
        }
        fft.forward(input, spectrum);
        std::array<double, BANDS> energy = {};
        for (int i = 0; i <= WINDOW / 2; ++i) {
            if (bandIndex[i] >= 0) {
                energy[bandIndex[i]] +=
                    (double)spectrum[i].re * spectrum[i].re + (double)spectrum[i].im * spectrum[i].im;
            }
        }
        for (int band = 0; band < BANDS; ++band) {
            const double level = std::log(energy[band] + 1e-9);
            features[band][frame] = frame ? (float)std::max(0.0, level - previous[band]) : 0;
            previous[band] = level;
        }
    }
    for (auto& band : features) {
        const double mean = std::accumulate(band.begin(), band.end(), 0.0) / frames;
        double variance = 0;
        for (const auto value : band) {
            variance += (value - mean) * (value - mean);
        }
        const double scale = 1 / (std::sqrt(variance / frames) + 1e-9);
        for (auto& value : band) {
            value = (float)((value - mean) * scale);
        }
    }
    return features;
}
struct Probe {
    int frame;
    double delta, ratio;
    bool matched;
};
using ReferenceBlock = std::array<std::vector<Complex>, BANDS>;

// 参照ブロック間でプローブの順変換を共用し、有効な相関だけを時刻順へ戻す。
std::vector<float> Correlate(const Features& target, int frame, const std::vector<ReferenceBlock>& blocks, int starts,
                              RealFFT& fft) {
    std::array<std::vector<Complex>, BANDS> probeSpectra;
    std::vector<float> input(CORRELATION_FFT, 0);
    for (int band = 0; band < BANDS; ++band) {
        std::fill(input.begin(), input.end(), 0);
        for (int i = 0; i < PROBE; ++i) {
            input[i] = target[band][frame + PROBE - 1 - i];
        }
        fft.forward(input, probeSpectra[band]);
    }
    std::vector<float> result(starts), output;
    std::vector<Complex> sum(CORRELATION_FFT / 2 + 1);
    for (size_t block = 0; block < blocks.size(); ++block) {
        std::fill(sum.begin(), sum.end(), Complex{0, 0});
        for (int band = 0; band < BANDS; ++band) {
            for (size_t i = 0; i < sum.size(); ++i) {
                const auto a = blocks[block][band][i], b = probeSpectra[band][i];
                sum[i].re += a.re * b.re - a.im * b.im;
                sum[i].im += a.re * b.im + a.im * b.re;
            }
        }
        fft.inverse(sum, output);
        const int start = (int)block * CORRELATION_STEP;
        const int count = std::min(CORRELATION_STEP, starts - start);
        std::copy_n(output.begin() + PROBE - 1, count, result.begin() + start);
    }
    return result;
}

// 区間の端は既知のずれ付近だけを、中心を動かした1秒窓で照合する。
double LocalScore(const Features& target, const Features& reference, double center, double delta) {
    const int centerFrame = (int)std::round(center * 100), lag = (int)std::round(delta * 100);
    double best = -1;
    for (int adjustment = -5; adjustment <= 5; ++adjustment) {
        double dot = 0, left = 0, right = 0;
        int count = 0;
        for (int frame = centerFrame - 50; frame < centerFrame + 50; ++frame) {
            const int ref = frame + lag + adjustment;
            if (frame < 0 || frame >= (int)target[0].size() || ref < 0 || ref >= (int)reference[0].size()) {
                continue;
            }
            ++count;
            for (int band = 0; band < BANDS; ++band) {
                const double a = target[band][frame], b = reference[band][ref];
                dot += a * b;
                left += a * a;
                right += b * b;
            }
        }
        if (count >= 20 && left > 0 && right > 0) {
            best = std::max(best, dot / std::sqrt(left * right));
        }
    }
    return best;
}
double RefineEdge(const Features& target, const Features& reference, double good, double outside, double delta,
                  double limit) {
    const double goodScore = LocalScore(target, reference, good, delta);
    double outsideScore = LocalScore(target, reference, outside, delta);
    const double direction = outside >= good ? 1 : -1;
    const double threshold = goodScore * 0.5;
    // 外側点も一致する間は1秒ずつ歩き、隣の一致点または録画端まで調べる。
    while (outsideScore >= threshold) {
        if (outside == limit) {
            return limit;
        }
        good = outside;
        outside = direction > 0 ? std::min(outside + 1, limit) : std::max(outside - 1, limit);
        outsideScore = LocalScore(target, reference, outside, delta);
    }
    while (std::abs(good - outside) > 0.1) {
        const double middle = (good + outside) / 2;
        if (LocalScore(target, reference, middle, delta) > threshold) {
            good = middle;
        } else {
            outside = middle;
        }
    }
    return (good + outside) / 2;
}
}

NicoJKAlignResult AlignNicoJKAudio(const std::vector<float>& target8k, const std::vector<float>& reference8k) {
    const auto begin = std::chrono::steady_clock::now();
    NicoJKAlignResult result;
    const double targetDuration = target8k.size() / (double)SAMPLE_RATE;
    const auto target = GetFeatures(target8k), reference = GetFeatures(reference8k);
    if (target[0].size() < PROBE + STEP || reference[0].size() < PROBE) {
        result.reason = u8"照合に必要な音声が短すぎます";
        return result;
    }
    RealFFT fft(CORRELATION_FFT);
    const int starts = (int)reference[0].size() - PROBE + 1;
    std::vector<ReferenceBlock> blocks;
    std::vector<float> input(CORRELATION_FFT);
    // 基準をPROBE-1点重なるブロックへ分け、各帯域のFFTを最初に1回だけ求める。
    for (int start = 0; start < starts; start += CORRELATION_STEP) {
        ReferenceBlock block;
        for (int band = 0; band < BANDS; ++band) {
            std::fill(input.begin(), input.end(), 0);
            const int count = std::min(CORRELATION_FFT, (int)reference[0].size() - start);
            std::copy_n(reference[band].begin() + start, count, input.begin());
            fft.forward(input, block[band]);
        }
        blocks.push_back(std::move(block));
    }
    std::vector<Probe> probes;
    for (int frame = 100; frame + PROBE <= (int)target[0].size(); frame += STEP) {
        const auto correlation = Correlate(target, frame, blocks, starts, fft);
        const auto bestPosition = std::max_element(correlation.begin(), correlation.end());
        const int location = (int)(bestPosition - correlation.begin());
        const double best = *bestPosition;
        double second = 0;
        for (int i = 0; i < starts; ++i) {
            if (i < location - 100 || i >= location + 100) {
                second = std::max(second, (double)correlation[i]);
            }
        }
        const double ratio = best > 0 ? best / std::max(second, 1e-12) : 0;
        probes.push_back({frame, (location - frame) / 100.0, ratio, ratio > MIN_RATIO});
    }
    for (size_t first = 0; first < probes.size();) {
        if (!probes[first].matched) {
            ++first;
            continue;
        }
        size_t last = first;
        while (last + 1 < probes.size() && probes[last + 1].matched &&
               std::abs(probes[last + 1].delta - probes[last].delta) <= SAME_DELTA + 1e-9) {
            ++last;
        }
        if (last > first) {
            std::vector<double> offsets;
            double ratio = 0;
            for (size_t i = first; i <= last; ++i) {
                offsets.push_back(probes[i].delta);
                ratio += probes[i].ratio;
            }
            std::sort(offsets.begin(), offsets.end());
            const double delta = offsets[offsets.size() / 2];
            const double firstCenter = probes[first].frame / 100.0 + 4, lastCenter = probes[last].frame / 100.0 + 4;
            const double left = first ? probes[first - 1].frame / 100.0 + 4 : 0;
            const double right = last + 1 < probes.size() ? probes[last + 1].frame / 100.0 + 4 : targetDuration;
            double leftLimit = 0, rightLimit = targetDuration;
            for (int i = (int)first - 1; i >= 0; --i) {
                if (probes[i].matched) {
                    leftLimit = probes[i].frame / 100.0 + 4;
                    break;
                }
            }
            for (size_t i = last + 1; i < probes.size(); ++i) {
                if (probes[i].matched) {
                    rightLimit = probes[i].frame / 100.0 + 4;
                    break;
                }
            }
            result.segments.push_back({RefineEdge(target, reference, firstCenter, left, delta, leftLimit),
                                       RefineEdge(target, reference, lastCenter, right, delta, rightLimit), delta,
                                       (int)(last - first + 1), ratio / offsets.size()});
        }
        first = last + 1;
    }
    double matched = 0, coveredEnd = 0;
    // 境界の推定が重なっても、同じ対象時間を二重に数えない。
    for (const auto& segment : result.segments) {
        matched += std::max(0.0, segment.end - std::max(segment.start, coveredEnd));
        coveredEnd = std::max(coveredEnd, segment.end);
    }
    result.success = matched >= targetDuration * 0.5 && !result.segments.empty();
    if (!result.success) {
        result.reason = u8"一致した区間が対象の長さの5割未満です";
    }
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    return result;
}

std::vector<NicoJKTimeMapEntry> MakeNicoJKTimeMap(const std::vector<NicoJKAlignSegment>& segments,
                                                  double targetDuration, double referenceDuration) {
    if (segments.empty() || targetDuration <= 0 || referenceDuration <= 0) {
        return {};
    }

    // 1. 一致区間、CMの間、前後の外挿を対象時刻順に並べる。
    std::vector<NicoJKTimeMapEntry> candidates;
    const auto& first = segments.front();
    candidates.push_back({first.delta, first.start + first.delta, 0, first.start});
    for (size_t i = 0; i < segments.size(); ++i) {
        const auto& part = segments[i];
        candidates.push_back({part.start + part.delta, part.end + part.delta, part.start, part.end});
        if (i + 1 < segments.size()) {
            const auto& next = segments[i + 1];
            candidates.push_back({part.end + part.delta, next.start + next.delta, part.end, next.start});
        }
    }
    const auto& last = segments.back();
    candidates.push_back({last.end + last.delta, targetDuration + last.delta, last.end, targetDuration});

    // 2. 基準または対象の録画範囲から外れる分を、傾きを保って切り詰める。
    std::vector<NicoJKTimeMapEntry> clipped;
    for (const auto& entry : candidates) {
        if (entry.referenceEnd <= entry.referenceStart || entry.targetEnd <= entry.targetStart) {
            continue;
        }
        const double slope = (entry.targetEnd - entry.targetStart) / (entry.referenceEnd - entry.referenceStart);
        const double begin = std::max({entry.referenceStart, 0.0, entry.referenceStart - entry.targetStart / slope});
        const double end = std::min({entry.referenceEnd, referenceDuration,
                                     entry.referenceStart + (targetDuration - entry.targetStart) / slope});
        if (end > begin) {
            clipped.push_back({begin, end, entry.targetStart + (begin - entry.referenceStart) * slope,
                               entry.targetStart + (end - entry.referenceStart) * slope});
        }
    }

    // 3. 直前の要素と重なる分を削り、二重配置と時刻の逆転を防ぐ。
    std::vector<NicoJKTimeMapEntry> map;
    for (auto entry : clipped) {
        if (!map.empty()) {
            const double slope = (entry.targetEnd - entry.targetStart) / (entry.referenceEnd - entry.referenceStart);
            const double begin = std::max({entry.referenceStart, map.back().referenceEnd,
                                           entry.referenceStart + (map.back().targetEnd - entry.targetStart) / slope});
            entry.targetStart += (begin - entry.referenceStart) * slope;
            entry.referenceStart = begin;
        }
        if (entry.referenceEnd > entry.referenceStart) {
            map.push_back(entry);
        }
    }
    return map;
}
std::string NicoJKTimeMapJSON(const std::vector<NicoJKTimeMapEntry>& map) {
    std::ostringstream json;
    json.imbue(std::locale::classic());
    json << std::fixed << std::setprecision(6) << "[";
    for (size_t i = 0; i < map.size(); ++i) {
        if (i) {
            json << ",";
        }
        const auto& entry = map[i];
        json << "[" << entry.referenceStart << "," << entry.referenceEnd << "," << entry.targetStart << ","
             << entry.targetEnd << "]";
    }
    return json.str() + "]";
}
std::string NicoJKAlignResultJSON(const NicoJKAlignResult& result, double targetDuration, double referenceDuration) {
    std::ostringstream json;
    json.imbue(std::locale::classic());
    json << std::fixed << std::setprecision(6) << "{\"success\":" << (result.success ? "true" : "false")
         << ",\"seconds\":" << result.seconds << ",\"segments\":[";
    for (size_t i = 0; i < result.segments.size(); ++i) {
        if (i) {
            json << ",";
        }
        const auto& segment = result.segments[i];
        json << "{\"start\":" << segment.start << ",\"end\":" << segment.end << ",\"delta\":" << segment.delta
             << ",\"probes\":" << segment.probes << ",\"ratio\":" << segment.ratio << "}";
    }
    json << "],\"timemap\":" << NicoJKTimeMapJSON(MakeNicoJKTimeMap(result.segments, targetDuration, referenceDuration))
         << "}";
    return json.str();
}
