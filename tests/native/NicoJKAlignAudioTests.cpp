#include "../../Amatsukaze/NicoJKAlignAudio.h"
#include "../../Amatsukaze/StringUtils.h"
#include <cmath>
#include <cstdio>

namespace {

void Require(bool value, const tchar * message) {
    if (!value) {
        THROWF(RuntimeException, "音声照合テスト失敗: %s", message);
    }
}

void PutConstant(std::vector<float> & samples, int64_t pts, double origin, int rate,
    int count, int left, int right) {
    std::vector<int16_t> pcm;
    for (int i = 0; i < count; ++i) {
        pcm.push_back((int16_t)left);
        pcm.push_back((int16_t)right);
    }
    AddNicoJKAlignAudioFrame(samples, pts, origin, rate, pcm.data(), count);
}

void ConstantRange(const std::vector<float> & samples, int first, int last,
    double expected, const tchar * message) {
    Require(last <= (int)samples.size(), _T("検査する範囲が出力外です"));
    for (int i = first; i < last; ++i) {
        Require(std::abs(samples[i] - expected) < 0.00001, message);
    }
}

void TestPlacement() {
    constexpr int64_t origin = 900000;
    std::vector<float> samples;
    PutConstant(samples, origin, origin, 48000, 4800, 12000, -4000);
    PutConstant(samples, origin + 18000, origin, 48000, 4800, -12000, 4000);
    Require(samples.size() == 2400, _T("指定したPTSに8kHz音声が置かれません"));
    ConstantRange(samples, 0, 800, 8000.0 / 65536, _T("L+R平均が違います"));
    ConstantRange(samples, 800, 1600, 0, _T("PTS欠落部分が無音になりません"));
    ConstantRange(samples, 1600, 2400, -8000.0 / 65536, _T("PTS欠落後の音声位置が違います"));
    PutConstant(samples, origin + 2250, origin, 48000, 1200, 2000, 2000);
    ConstantRange(samples, 200, 400, 4000.0 / 65536, _T("重複した位置を上書きしません"));
}

void TestWrapAndLeadingAudio() {
    constexpr int64_t wrap = (int64_t)1 << 33;
    const int64_t origin = wrap - 9000;
    std::vector<float> samples;
    PutConstant(samples, origin - 4500, origin, 48000, 4800, 10000, 10000);
    PutConstant(samples, 9000, origin, 48000, 4800, 6000, 6000);
    ConstantRange(samples, 0, 400, 20000.0 / 65536, _T("映像先頭前の音声の切り捨てが違います"));
    ConstantRange(samples, 400, 1600, 0, _T("ラップ前後の空白位置が違います"));
    ConstantRange(samples, 1600, 2400, 12000.0 / 65536, _T("33bitラップ後の音声位置が違います"));
}

void TestUnknownPTSAndRateChange() {
    constexpr int64_t origin = 900000;
    std::vector<float> samples;
    PutConstant(samples, -1, origin, 48000, 4800, 25000, 25000);
    Require(samples.empty(), _T("不明PTSの先頭音声を使っています"));
    PutConstant(samples, origin, origin, 48000, 4800, 4000, 4000);
    PutConstant(samples, -1, origin, 48000, 4800, 25000, 25000);
    PutConstant(samples, origin + 18000, origin, 44100, 4410, 4000, 4000);
    Require(samples.size() == 2400, _T("サンプルレート変更で長さが変わりました"));
    ConstantRange(samples, 0, 800, 8000.0 / 65536, _T("48kHzの平均が違います"));
    ConstantRange(samples, 800, 1600, 0, _T("不明PTSが無音になりません"));
    ConstantRange(samples, 1600, 2400, 8000.0 / 65536, _T("44.1kHzの平均が違います"));
}

void TestFrameAverage() {
    constexpr int64_t origin = 900000;
    std::vector<float> samples;
    PutConstant(samples, origin, origin, 48000, 4800, 16000, -16000);
    ConstantRange(samples, 0, 800, 0, _T("左右反相が無音になりません"));

    const int16_t pcm[] = { 0, 600, 600, 1200, 1200, 1800, 1800, 2400,
        2400, 3000, 3000, 3600, 6000, 6600, 6600, 7200, 7200, 7800,
        7800, 8400, 8400, 9000, 9000, 9600 };
    std::vector<float> averaged;
    AddNicoJKAlignAudioFrame(averaged, origin, origin, 48000, pcm, 12);
    Require(averaged.size() == 2, _T("48kHzの6サンプルを1サンプルにしません"));
    ConstantRange(averaged, 0, 1, 3600.0 / 65536, _T("最初の6サンプルの平均が違います"));
    ConstantRange(averaged, 1, 2, 15600.0 / 65536, _T("次の6サンプルの平均が違います"));
}

}

int main() {
    try {
        TestPlacement();
        TestWrapAndLeadingAudio();
        TestUnknownPTSAndRateChange();
        TestFrameAverage();
        _ftprintf(stdout, _T("音声取り出しの合成テスト4件成功\n"));
        return 0;
    } catch (Exception & error) {
        _ftprintf(stderr, _T("%s\n"), error.message());
    } catch (const std::exception & error) {
        _ftprintf(stderr, _T("%s\n"), char_to_tstring(error.what()).c_str());
    }
    return 1;
}
