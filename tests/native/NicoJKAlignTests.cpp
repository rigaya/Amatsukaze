#include "../../Amatsukaze/NicoJKAlign.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <vector>
namespace {
constexpr int RATE = 8000;
void Check(bool ok, const char* message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}
std::vector<float> Noise(int seconds, unsigned seed) {
    std::mt19937 generator(seed);
    std::uniform_real_distribution<float> distribution(-0.5f, 0.5f);
    std::vector<float> result(seconds * RATE);
    for (auto& sample : result) {
        sample = distribution(generator);
    }
    return result;
}
void Append(std::vector<float>& out, const std::vector<float>& in, int start, int end) {
    out.insert(out.end(), in.begin() + start * RATE, in.begin() + end * RATE);
}
void TestMatching() {
    const auto audio = Noise(210, 123);
    const auto identity = AlignNicoJKAudio(audio, audio);
    Check(identity.success && identity.segments.size() == 1, "同一音声が1区間になりません");
    Check(std::abs(identity.segments[0].delta) < 0.01, "同一音声のずれが0ではありません");
    Check(identity.segments[0].start < 0.1 && identity.segments[0].end > 209.9, "同一音声の端が欠けています");
    auto delayed = audio;
    delayed.erase(delayed.begin(), delayed.begin() + 2240);
    const auto shifted = AlignNicoJKAudio(delayed, audio);
    Check(shifted.success && shifted.segments.size() == 1, "一律のずれを照合できません");
    Check(std::abs(shifted.segments[0].delta - 0.28) < 0.01, "一律のずれが0.28秒ではありません");
    std::vector<float> reference;
    const auto insertion = Noise(30, 456);
    Append(reference, audio, 0, 60);
    Append(reference, insertion, 0, 10);
    Append(reference, audio, 60, 120);
    Append(reference, insertion, 10, 30);
    Append(reference, audio, 120, 210);
    const auto cuts = AlignNicoJKAudio(audio, reference);
    Check(cuts.success && cuts.segments.size() == 3, "差し込み付き音声が3区間になりません");
    const double expected[] = {0, 10, 30};
    for (size_t i = 0; i < 3; ++i) {
        const auto& part = cuts.segments[i];
        Check(std::abs(part.delta - expected[i]) < 0.01, "区間ごとのずれが一致しません");
        const double start = i * 60.0, end = i == 2 ? 210 : (i + 1) * 60.0;
        std::fprintf(stderr, "境界: %zu %.6f %.6f\n", i, part.start, part.end);
        Check(std::abs(part.start - start) < 0.1 && std::abs(part.end - end) < 0.1,
              "区間境界の誤差が0.1秒を超えました");
    }
    // 8秒の照合では境界をまたいで不一致だが、外側点の1秒窓はまだ一致する。
    // 右向き・左向きの両方で、そこからさらに歩いて本当の境界を探す。
    for (double boundary : {64.7, 65.3}) {
        const size_t split = (size_t)std::llround(boundary * RATE);
        std::vector<float> withInsertion(audio.begin(), audio.begin() + split);
        Append(withInsertion, insertion, 0, 10);
        withInsertion.insert(withInsertion.end(), audio.begin() + split, audio.end());
        const auto walked = AlignNicoJKAudio(audio, withInsertion);
        Check(walked.success && walked.segments.size() == 2, "境界歩行の合成音声が2区間になりません");
        std::fprintf(stderr, "歩行境界%.1f: %.6f %.6f\n", boundary, walked.segments[0].end, walked.segments[1].start);
        Check(std::abs(walked.segments[0].end - boundary) < 0.1, "外側点から右へ歩いた境界の誤差が0.1秒を超えました");
        Check(std::abs(walked.segments[1].start - boundary) < 0.1, "外側点から左へ歩いた境界の誤差が0.1秒を超えました");
    }
    const std::vector<float> shortReference(audio.begin(), audio.begin() + 60 * RATE);
    Check(!AlignNicoJKAudio(audio, shortReference).success, "一致が5割未満でも成功しました");
    Check(!AlignNicoJKAudio(Noise(90, 1), Noise(90, 2)).success, "無関係の音声を一致させました");
    Check(!AlignNicoJKAudio(std::vector<float>(90 * RATE), std::vector<float>(90 * RATE)).success,
          "無音を一致させました");
    Check(!AlignNicoJKAudio({}, {}).success, "空の音声を一致させました");
}
// 旧FFmpegの単一FFT上限を超える録画長でも、ブロック境界をまたいで照合できる。
void TestLongAudio() {
    for (int duration : {1800, 3600}) {
        const auto audio = Noise(duration, (unsigned)duration);
        const auto result = AlignNicoJKAudio(audio, audio);
        Check(result.success && result.segments.size() == 1, "長尺の同一音声が1区間になりません");
        Check(std::abs(result.segments[0].delta) < 0.01, "長尺の同一音声のずれが0ではありません");
        Check(result.segments[0].start < 0.1 && result.segments[0].end > duration - 0.1,
              "長尺の照合で録画範囲が欠けています");
        std::printf("長尺%d秒: 照合%.3f秒\n", duration, result.seconds);
    }
}
double Map(const std::vector<NicoJKTimeMapEntry>& entries, double time) {
    for (const auto& entry : entries) {
        if (time >= entry.referenceStart && time < entry.referenceEnd) {
            return entry.targetStart + (time - entry.referenceStart) * (entry.targetEnd - entry.targetStart) /
                                           (entry.referenceEnd - entry.referenceStart);
        }
    }
    return -1;
}
void TestMap() {
    const auto map = MakeNicoJKTimeMap({{10, 40, 0, 2, 2}, {70, 100, -20, 2, 2}}, 120, 100);
    Check(std::abs(Map(map, 5) - 5) < 1e-9, "前方の外挿が違います");
    Check(std::abs(Map(map, 20) - 20) < 1e-9, "一致区間の写像が違います");
    Check(std::abs(Map(map, 45) - 55) < 1e-9, "CMの伸縮が違います");
    Check(std::abs(Map(map, 50) - 70) < 1e-9, "後側境界の写像が違います");
    Check(std::abs(Map(map, 90) - 110) < 1e-9, "後方の外挿が違います");
    Check(Map(map, -0.01) < 0 && Map(map, 100) < 0, "範囲外を写しました");
    const auto overlap = MakeNicoJKTimeMap({{10, 50, 0, 2, 2}, {60, 90, -20, 2, 2}}, 100, 100);
    for (size_t i = 1; i < overlap.size(); ++i) {
        Check(overlap[i].referenceStart >= overlap[i - 1].referenceEnd, "基準側の範囲が重なります");
        Check(overlap[i].targetStart >= overlap[i - 1].targetEnd, "対象側の範囲が逆転します");
    }
    Check(std::abs(Map(overlap, 50) - 70) < 1e-9, "重複区間を切り詰めていません");
    const auto clipped = MakeNicoJKTimeMap({{0, 90, -10, 2, 2}}, 90, 70);
    Check(Map(clipped, 0) == 10 && Map(clipped, 69) == 79 && Map(clipped, 70) < 0, "録画範囲の切り詰めが違います");
    const auto positive = MakeNicoJKTimeMap({{20, 40, 10, 2, 2}}, 50, 60);
    Check(Map(positive, 10) == 0 && Map(positive, 55) == 45, "正のずれの外挿が違います");
    Check(MakeNicoJKTimeMap({}, 10, 10).empty(), "空の区間から写像を作りました");
    Check(NicoJKTimeMapJSON(map).find('[') == 0, "対応表JSONが配列ではありません");
}
}
int main() {
    try {
        TestMatching();
        TestLongAudio();
        TestMap();
        std::puts("実況照合テスト成功");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "実況照合テスト失敗: %s\n", error.what());
        return 1;
    }
}
