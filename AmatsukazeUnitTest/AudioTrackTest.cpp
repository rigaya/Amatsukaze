#include "AudioTrackPlanner.h"
#include "AudioTrackBuilder.h"
#include "StreamReform.h"

#include <fstream>
#include <iostream>
#include <stdexcept>

#ifdef AUDIO_TRACK_STANDALONE
// 単体実行では巨大なStreamReform実装をリンクせず、同じ初期値を使う。
FileAudioFrameInfo::FileAudioFrameInfo()
    : AudioFrameInfo(), audioIdx(0), codedDataSize(0), waveDataSize(0), fileOffset(0), waveOffset(-1) {}
#else
#include "gtest/gtest.h"
#endif

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<typename Function>
void ExpectFormatError(Function function) {
    try { function(); }
    catch (const FormatException&) { return; }
    throw std::runtime_error("不正なプランが拒否されませんでした");
}

std::vector<FileAudioFrameInfo> MakeFrames(std::initializer_list<AUDIO_CHANNELS> layouts) {
    std::vector<FileAudioFrameInfo> frames(layouts.size());
    size_t index = 0;
    for (auto layout : layouts) {
        frames[index].format = { layout, 48000 };
        frames[index].numSamples = AAC_LC_FRAME_SAMPLES;
        index++;
    }
    return frames;
}

double Duration(int frames) {
    return static_cast<double>(frames) * AAC_LC_FRAME_SAMPLES * MPEG_CLOCK_HZ / 48000;
}

void PlannerTests() {
    auto info = MakeFrames({ AUDIO_STEREO, AUDIO_STEREO, AUDIO_32_LFE, AUDIO_32_LFE, AUDIO_32_LFE, AUDIO_MONO });
    auto plans = PlanSeparateAudioTracks({ { 0, 2, 3, 4 }, { -1, -1, 5, -1 } }, info, Duration(4));
    Check(plans.size() == 3, "変化後のトラック数が不正です");
    Check(plans[0].sourceTrack == 0 && plans[0].layout == AUDIO_32_LFE && plans[0].name == _T("Audio0-5.1ch"), "最長レイアウトが先頭になりません");
    Check(plans[0].frames[0].operation == AudioTrackOperation::SILENCE, "異なるレイアウトが無音になりません");
    Check(plans[1].layout == AUDIO_STEREO && plans[1].frames[0].frameIndex == 0, "ステレオのコピーが不正です");
    Check(plans[2].sourceTrack == 1 && plans[2].name == _T("Audio1-1ch") && plans[2].frames[2].frameIndex == 5, "途中で出現する音声が欠落しました");
    plans = PlanSeparateAudioTracks({ { 2, 3 }, { -1, -1 } }, info, Duration(2));
    Check(plans.size() == 1, "CMカット後に消えたトラックが残っています");
    plans = PlanSeparateAudioTracks({ { 0, 2 } }, info, Duration(2));
    Check(plans.size() == 2 && plans.front().layout == AUDIO_32_LFE, "同じ時間のレイアウト順が不正です");
    auto dualInfo = MakeFrames({ AUDIO_STEREO, AUDIO_2LANG, AUDIO_2LANG, AUDIO_MONO });
    plans = PlanSeparateAudioTracks({ { 0, 1, 2, 3 } }, dualInfo, Duration(4));
    Check(plans.size() == 3, "デュアルモノ混在のトラック数が不正です");
    Check(plans[0].logicalTrack == 0 && plans[0].layout == AUDIO_MONO, "デュアルモノの主音声順が不正です");
    Check(plans[0].frames[1].dualMonoChannel == 0 && plans[0].frames[3].dualMonoChannel == -1, "主音声のコピー参照が不正です");
    Check(plans[1].logicalTrack == 1 && plans[1].frames[1].dualMonoChannel == 1 &&
        plans[1].frames[0].operation == AudioTrackOperation::SILENCE &&
        plans[1].frames[3].operation == AudioTrackOperation::SILENCE, "副音声の無音区間が不正です");
    Check(plans[0].name == _T("Audio0-lang1-1ch") && plans[1].name == _T("Audio0-lang2-1ch") &&
        plans[2].name == _T("Audio0-lang1-2ch"), "デュアルモノ混在の言語とレイアウト名が不正です");
    auto mixed = PlanSeparateAudioTracks({ { 0, 1, 2, 3 }, { 0, 1, 2, 3 } }, dualInfo, Duration(4));
    std::set<tstring> names;
    for (const auto& plan : mixed) {
        Check(plan.name.find(_T(' ')) == tstring::npos, "音声名に空白が含まれています");
        Check(names.insert(plan.name).second, "音声名が重複しています");
    }
    auto badPlans = plans;
    badPlans[0].sampleRate = 0;
    badPlans[0].samplingFrequencyIndex = -1;
    ExpectFormatError([&] { ValidateAudioTrackPlans(badPlans, Duration(4)); });
    badPlans = plans;
    badPlans[0].frames.pop_back();
    ExpectFormatError([&] { ValidateAudioTrackPlans(badPlans, Duration(4)); });
    badPlans = plans;
    badPlans[0].frames[0].dstLayout = AUDIO_STEREO;
    ExpectFormatError([&] { ValidateAudioTrackPlans(badPlans, Duration(4)); });
    badPlans = plans;
    badPlans[0].frames[1].operation = static_cast<AudioTrackOperation>(99);
    ExpectFormatError([&] { ValidateAudioTrackPlans(badPlans, Duration(4)); });
    badPlans = plans;
    badPlans[0].frames[1].srcLayout = AUDIO_STEREO;
    ExpectFormatError([&] { ValidateAudioTrackPlans(badPlans, Duration(4)); });
    ExpectFormatError([&] { PlanSeparateAudioTracks({ { 0 }, { 1, 2 } }, info, Duration(1)); });
    ExpectFormatError([&] { PlanSeparateAudioTracks({ { 99 } }, info, Duration(1)); });
    ExpectFormatError([&] { PlanSeparateAudioTracks({ { -2 } }, info, Duration(1)); });
    ExpectFormatError([&] { PlanSeparateAudioTracks({ { 0, 1 } }, info, Duration(5)); });
    auto badInfo = info;
    badInfo[1].format.sampleRate = 44100;
    ExpectFormatError([&] { PlanSeparateAudioTracks({ { 0, 1 } }, badInfo, Duration(2)); });
    badInfo = info;
    badInfo[0].numSamples = 2048;
    ExpectFormatError([&] { PlanSeparateAudioTracks({ { 0 } }, badInfo, Duration(1)); });
    badInfo = info;
    badInfo[0].format.channels = AUDIO_22;
    ExpectFormatError([&] { PlanSeparateAudioTracks({ { 0 } }, badInfo, Duration(1)); });
    Check(PlanSeparateAudioTracks({ { -1, -1 } }, info, Duration(2)).empty(), "全区間が無音のトラックが残っています");
}

void MergePlannerTests() {
    auto info = MakeFrames({ AUDIO_STEREO, AUDIO_32_LFE, AUDIO_32_LFE, AUDIO_32_LFE, AUDIO_STEREO, AUDIO_2LANG });
    auto plans = PlanMergeAudioTracks({ { 0, 1, 2, -1 }, { -1, 4, -1, -1 } }, info, Duration(4));
    Check(plans.size() == 2 && plans[0].layout == AUDIO_32_LFE && plans[0].name == _T("Audio0") &&
        plans[1].sourceTrack == 1 && plans[1].name == _T("Audio1"), "統合の最長レイアウトまたはトラック順が不正です");
    Check(plans[0].frames[0].operation == AudioTrackOperation::CONVERT && plans[0].frames[0].frameIndex == 0 &&
        plans[0].frames[0].srcLayout == AUDIO_STEREO && plans[0].frames[0].dstLayout == AUDIO_32_LFE &&
        plans[0].frames[1].operation == AudioTrackOperation::COPY && plans[0].frames[3].operation == AudioTrackOperation::SILENCE,
        "統合のCOPY/CONVERT/SILENCE参照が不正です");
    plans = PlanMergeAudioTracks({ { 0, 4, 1 } }, info, Duration(3));
    Check(plans[0].layout == AUDIO_STEREO && plans[0].frames[2].operation == AudioTrackOperation::CONVERT &&
        plans[0].frames[2].srcLayout == AUDIO_32_LFE && plans[0].frames[2].dstLayout == AUDIO_STEREO,
        "ステレオ最長時の5.1ch変換が不正です");
    plans = PlanMergeAudioTracks({ { 0, 1 } }, info, Duration(2));
    Check(plans.size() == 1 && plans[0].layout == AUDIO_32_LFE, "統合の同率時に多チャンネルを選びません");
    plans = PlanMergeAudioTracks({ { 0, 4, 5 } }, info, Duration(3));
    Check(plans.size() == 2 && plans[0].layout == AUDIO_STEREO && plans[1].layout == AUDIO_MONO,
        "デュアルモノ論理トラックの統合レイアウトが不正です");
    Check(plans[0].frames[2].operation == AudioTrackOperation::CONVERT && plans[0].frames[2].dualMonoChannel == 0 &&
        plans[0].frames[2].srcLayout == AUDIO_MONO && plans[1].frames[2].operation == AudioTrackOperation::COPY &&
        plans[1].frames[2].dualMonoChannel == 1 && plans[1].frames[0].operation == AudioTrackOperation::SILENCE,
        "デュアルモノ統合の主副言語参照が不正です");
    plans = PlanMergeAudioTracks({ { 0, 5, 5 } }, info, Duration(3));
    Check(plans[0].layout == AUDIO_MONO && plans[0].frames[0].operation == AudioTrackOperation::CONVERT &&
        plans[0].frames[1].dualMonoChannel == 0, "モノラル最長時のステレオ変換が不正です");
    auto invalid = plans;
    invalid[0].frames[0].srcLayout = AUDIO_MONO;
    ExpectFormatError([&] { ValidateAudioTrackPlans(invalid, Duration(3)); });
    invalid = plans;
    invalid[0].frames[0].frameIndex = -1;
    ExpectFormatError([&] { ValidateAudioTrackPlans(invalid, Duration(3)); });
    invalid = plans;
    invalid[0].frames[0].srcLayout = AUDIO_NONE;
    ExpectFormatError([&] { ValidateAudioTrackPlans(invalid, Duration(3)); });
    Check(PlanMergeAudioTracks({ { 0 }, { -1 } }, info, Duration(1)).size() == 1,
        "統合でCMカット後の全無音トラックが残っています");
    Check(PlanMergeAudioTracks({ { -1 } }, info, Duration(1)).empty(), "統合で全無音トラックが残っています");
}

std::vector<uint8_t> FromHex(const std::string& hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < hex.size(); i += 2) bytes.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    return bytes;
}

void SilenceTests(const std::string& directory = "") {
    const AUDIO_CHANNELS layouts[] = { AUDIO_MONO, AUDIO_STEREO, AUDIO_32_LFE };
    const char* expected[] = {
        "fff14c40017ffc00c80007",
        "fff14c8001dffc2064000190000e",
        "fff14d80039ffc00c800010320000c800011320000c8000306400038"
    };
    for (int i = 0; i < 3; i++) {
        const auto bytes = GenerateSilentAdtsFrame(layouts[i], 3);
        Check(bytes == FromHex(expected[i]), "無音AACのバイト列が設計書と一致しません");
    }
    for (const auto layout : { AUDIO_MONO, AUDIO_STEREO, AUDIO_30, AUDIO_31, AUDIO_32, AUDIO_32_LFE }) {
        const auto bytes = GenerateSilentAdtsFrame(layout, 3);
        if (!directory.empty()) {
            std::ofstream file(directory + "/silent" + std::to_string(GetAudioAdtsChannelConfiguration(layout)) + ".aac", std::ios::binary);
            Check(static_cast<bool>(file), "無音フレームの出力ファイルを開けません");
            for (int frame = 0; frame < 200; frame++) file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        }
    }
    ExpectFormatError([] { GenerateSilentAdtsFrame(AUDIO_2LANG, 3); });
    ExpectFormatError([] { GenerateSilentAdtsFrame(AUDIO_MONO, 15); });
    Check(GetAudioSamplingFrequencyIndex(48000) == 3 && GetAudioSamplingFrequencyIndex(12345) == -1, "サンプルレート逆引きが不正です");
}
}

#ifdef AUDIO_TRACK_STANDALONE
int main(int argc, char** argv) {
    try {
        PlannerTests();
        MergePlannerTests();
        SilenceTests(argc > 1 ? argv[1] : "");
        std::cout << "Planner・無音AAC単体テスト成功\n";
        return 0;
    } catch (const Exception& error) {
        std::cerr << error.message() << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
    }
    return 1;
}
#else
TEST(AudioTrack, SeparatePlanner) { EXPECT_NO_THROW(PlannerTests()); }
TEST(AudioTrack, MergePlanner) { EXPECT_NO_THROW(MergePlannerTests()); }
TEST(AudioTrack, SilentAdtsBytes) { EXPECT_NO_THROW(SilenceTests()); }
#endif
