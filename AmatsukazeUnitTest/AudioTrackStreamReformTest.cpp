#include "StreamReform.h"
#include "TranscodeSetting.h"
#include "AudioTrackBuilder.h"
#include "AudioTrackConverter.h"
#include "AdtsParser.h"
#include <fstream>
#include <filesystem>
#include <sstream>
#include <iostream>
#include <stdexcept>

namespace {
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

std::vector<uint8_t> MakePceSilentFrame(bool dualMono = false) {
    auto stereo = GenerateSilentAdtsFrame(AUDIO_STEREO, 3);
    AutoBuffer raw;
    BitWriter writer(raw);
    writer.write<3>(5); // PCE。
    writer.write<4>(0);
    writer.write<2>(1); // AAC-LC。
    writer.write<4>(3); // 48kHz。
    writer.write<4>(dualMono ? 2 : 1); // 前方に2SCEまたは1CPE。
    writer.write<4>(0);
    writer.write<4>(0);
    writer.write<2>(0);
    writer.write<3>(0);
    writer.write<4>(0);
    writer.write<1>(0);
    writer.write<1>(0);
    writer.write<1>(0);
    for (int element = 0; element < (dualMono ? 2 : 1); ++element) {
        writer.write<1>(dualMono ? 0 : 1);
        writer.write<4>(element);
    }
    writer.byteAlign<false>();
    writer.write<8>(0); // コメントなし。
    writer.flush();
    if (dualMono) {
        for (int channel = 0; channel < 2; ++channel) {
            writer.write<3>(0); // SCE。
            writer.write<4>(channel);
            writer.write<8>(100);
            writer.write<14>(0);
        }
        writer.write<3>(7);
        writer.byteAlign<false>();
        writer.flush();
    } else {
        raw.add(MemoryChunk(stereo.data() + 7, stereo.size() - 7));
    }
    stereo.resize(7);
    stereo.insert(stereo.end(), raw.ptr(), raw.ptr() + raw.size());
    const int length = static_cast<int>(stereo.size());
    stereo[2] &= 0xfe;
    stereo[3] = (stereo[3] & 0x3c) | (length >> 11);
    stereo[4] = (length >> 3) & 0xff;
    stereo[5] = ((length & 7) << 5) | 0x1f;
    return stereo;
}

void RunScenario(int missingFrames, bool cut, int sourcePhaseTicks = 0, int fallback = 0, const tstring& directory = tstring(), AUDIO_FORMAT_CHANGE_MODE mode = AFC_SEPARATE) {
    AMTContext ctx;
    const int audioTicks = AAC_LC_FRAME_SAMPLES * MPEG_CLOCK_HZ / 48000;
    const int audioCount = 600;
    const int videoCount = 384;
    std::vector<FileVideoFrameInfo> video(videoCount);
    for (int i = 0; i < videoCount; ++i) {
        auto& frame = video[i];
        frame.PTS = frame.DTS = i * 3000;
        frame.pic = PIC_FRAME;
        frame.isGopStart = i % 30 == 0;
        frame.progressive = true;
        frame.format.format = VS_MPEG2;
        frame.format.width = frame.format.displayWidth = 1920;
        frame.format.height = frame.format.displayHeight = 1080;
        frame.format.frameRateNum = 30;
        frame.format.frameRateDenom = 1;
        frame.format.sarWidth = frame.format.sarHeight = 1;
        frame.format.progressive = true;
        frame.codedDataSize = 100;
    }
    std::vector<FileAudioFrameInfo> audio;
    int track1First = -1;
    for (int i = 0; i < audioCount; ++i) {
        // track0の中央に閾値前後の欠落を置く。後半で5.1chへ切り替える。
        if (i < 100 || i >= 100 + missingFrames) {
            FileAudioFrameInfo frame;
            frame.PTS = i * audioTicks + sourcePhaseTicks;
            frame.numSamples = AAC_LC_FRAME_SAMPLES;
            frame.format = { i < 300 ? AUDIO_STEREO : AUDIO_32_LFE, 48000 };
            frame.audioIdx = 0;
            frame.codedDataSize = 14;
            frame.fileOffset = audio.size() * 14;
            audio.push_back(frame);
        }
        // track1は途中で現れ、いったん消え、再出現して映像より早く終わる。
        if ((i >= 200 && i < 260) || (i >= 400 && i < 450)) {
            FileAudioFrameInfo frame;
            frame.PTS = i * audioTicks;
            frame.numSamples = AAC_LC_FRAME_SAMPLES;
            frame.format = { AUDIO_STEREO, 48000 };
            frame.audioIdx = 1;
            frame.codedDataSize = 14;
            frame.fileOffset = audio.size() * 14;
            if (track1First == -1) track1First = static_cast<int>(audio.size());
            audio.push_back(frame);
        }
    }
    int change = 0;
    while (change < static_cast<int>(audio.size()) && audio[change].PTS < 300 * audioTicks) ++change;
    std::vector<StreamEvent> events = {
        { PID_TABLE_CHANGED, 0, 0, 1 }, { VIDEO_FORMAT_CHANGED, 0, 0, 0 },
        { AUDIO_FORMAT_CHANGED, 0, 0, 0 },
        { PID_TABLE_CHANGED, 128, 0, 2 }, { AUDIO_FORMAT_CHANGED, track1First, 1, 0 },
        { AUDIO_FORMAT_CHANGED, change, 0, 0 }, { PID_TABLE_CHANGED, 166, 0, 1 },
        { PID_TABLE_CHANGED, 256, 0, 2 }, { PID_TABLE_CHANGED, 288, 0, 1 }
    };
    std::stable_sort(events.begin() + 3, events.end(), [&](const StreamEvent& a, const StreamEvent& b) {
        const int64_t ap = a.type == AUDIO_FORMAT_CHANGED ? audio[a.frameIdx].PTS : video[a.frameIdx].PTS;
        const int64_t bp = b.type == AUDIO_FORMAT_CHANGED ? audio[b.frameIdx].PTS : video[b.frameIdx].PTS;
        return ap < bp;
    });
    if (fallback == 1) {
        for (auto& frame : audio) if (frame.PTS >= 300 * audioTicks) frame.format.sampleRate = 44100;
    } else if (fallback == 2) {
        for (auto& frame : audio) if (frame.format.channels == AUDIO_32_LFE) frame.format.channels = AUDIO_22;
    }
    if (fallback == 5) {
        for (auto& frame : audio) if (frame.audioIdx == 0) frame.format.channels = AUDIO_2LANG;
    }
    tstring audioPath;
    if (fallback >= 3) {
        audioPath = directory + (fallback == 3 ? _T("/non-lc-source.aac") : fallback == 4 ? _T("/pce-source.aac") : _T("/pce-dual-source.aac"));
        File file(audioPath, _T("wb"));
        int64_t offset = 0;
        for (auto& frame : audio) {
            auto bytes = fallback == 4 ? MakePceSilentFrame() : fallback == 5 && frame.audioIdx == 0 ? MakePceSilentFrame(true) : GenerateSilentAdtsFrame(frame.format.channels, 3);
            if (fallback == 3) bytes[2] &= 0x3f; // AAC Mainを指定し、非LC検出を検証する。
            frame.fileOffset = offset;
            frame.codedDataSize = static_cast<int>(bytes.size());
            offset += bytes.size();
            file.write(MemoryChunk(bytes.data(), bytes.size()));
        }
    }
    std::vector<CaptionItem> captions;
    std::vector<TimeInfo> times;
    const auto audioInfo = audio;
    StreamReformInfo reform(ctx, 1, video, audio, captions, events, times);
    reform.prepare(false, false, false, mode, audioPath);
    // cut時は、track1の最初の出現区間とtrack0の変更を含む範囲を落とす。
    std::vector<EncoderZone> zones;
    if (cut) zones.push_back({ 120, 220 });
    reform.applyCMZones(0, zones, {});
    reform.genAudio({ cut ? CMTYPE_NONCM : CMTYPE_BOTH });
    if (fallback != 0 && fallback != 5) {
        Require(reform.getOutFileKeys().size() > 1, "未対応音声がジョブ全体のsplitに戻りません");
        for (const auto& key : reform.getOutFileKeys()) {
            Require(!reform.getEncodeFile(key).isAudioTrackPlanned, "fallbackに統合音声計画が残りました");
        }
        return;
    }
    Require(reform.getOutFileKeys().size() == 1, "音声変更でファイルが分割されました");
    const auto& file = reform.getEncodeFile(reform.getOutFileKeys()[0]);
    Require(file.audioFrames.size() == 2, "途中で増えた音声トラックが失われました");
    Require(file.audioFrames[0].size() == file.audioFrames[1].size(), "音声列の長さが一致しません");
    Require(file.audioFrames[1].front() == -1 && file.audioFrames[1].back() == -1, "副音声の先頭と末尾が無音になりません");
    if (!cut) {
        for (int i = 100; i < 100 + missingFrames; ++i) {
            Require((file.audioFrames[0][i] == -1) == (missingFrames > 5), "MAX_DUP閾値前後の処理が不正です");
        }
        Require(file.audioFrames[1][210] >= 0 && file.audioFrames[1][300] == -1 && file.audioFrames[1][420] >= 0,
            "副音声の消失または再出現が不正です");
    } else {
        const int originalTrack1 = static_cast<int>(std::count_if(file.audioFrames[1].begin(), file.audioFrames[1].end(), [](int value) { return value >= 0; }));
        Require(originalTrack1 == 50, "CMカット後の副音声の再出現区間が不正です");
    }
    if (sourcePhaseTicks != 0) {
        for (size_t i = 1; i < file.audioFrames[0].size(); ++i) {
            const int previous = file.audioFrames[0][i - 1];
            const int current = file.audioFrames[0][i];
            if (previous < 0 || current < 0 || audioInfo[current].PTS - audioInfo[previous].PTS != audioTicks) {
                throw std::runtime_error("位相ずれのある連続音声を再選択または欠落しました: 位相="
                    + std::to_string(sourcePhaseTicks) + " 出力位置="
                    + std::to_string(i) + " 前参照=" + std::to_string(previous) + " 現参照=" + std::to_string(current));
            }
        }
    }
    if (mode == AFC_MERGE) {
        const size_t expectedTracks = fallback == 5 ? 3 : 2;
        Require(file.audioTrackPlan.size() == expectedTracks, "統合音声の論理トラック数が不正です");
        for (size_t track = 0; track < file.audioTrackPlan.size(); ++track) {
            Require(file.audioTrackPlan[track].name == _T("Audio") + std::to_string(track), "統合音声のトラック名が不正です");
        }
        if (fallback != 5) {
            const auto& plan = file.audioTrackPlan.front();
            Require(std::any_of(plan.frames.begin(), plan.frames.end(), [](const AudioTrackReference& ref) {
                return ref.operation == AudioTrackOperation::CONVERT;
            }), "異なるレイアウトがCONVERT参照になりませんでした");
        }
    }
    if (fallback == 5) Require(file.audioTrackPlan.size() == 3, "cfg0デュアルモノが主副monoへ展開されませんでした");
    ValidateAudioTrackPlans(file.audioTrackPlan, file.duration);
    for (const auto& plan : file.audioTrackPlan) Require(plan.frames.size() == file.audioFrames[0].size(), "プランと参照列の長さが一致しません");
    const auto pcm = reform.getWaveInput(file.audioFrames[1]);
    Require(pcm.front().frameIndex == -1 && pcm.front().waveLength == 0, "無音参照のPCM入力が不正です");
}
}

void BuilderCopyTest(const tstring& directory) {
    AMTContext ctx;
    auto bytes = GenerateSilentAdtsFrame(AUDIO_STEREO, 3);
    const auto source = directory + _T("/builder-source.aac");
    const auto destination = directory + _T("/builder-copy.aac");
    {
        File file(source, _T("wb"));
        file.write(MemoryChunk(bytes.data(), bytes.size()));
        file.write(MemoryChunk(bytes.data(), bytes.size()));
    }
    std::vector<FileAudioFrameInfo> info(2);
    for (auto& frame : info) {
        frame.format = { AUDIO_STEREO, 48000 };
        frame.numSamples = AAC_LC_FRAME_SAMPLES;
    }
    const auto plans = PlanSeparateAudioTracks({ { 0, -1, 1 } }, info,
        3.0 * AAC_LC_FRAME_SAMPLES * MPEG_CLOCK_HZ / 48000);
    PacketCache cache(ctx, source, { 0, static_cast<int64_t>(bytes.size()), static_cast<int64_t>(2 * bytes.size()) }, 2, 2);
    BuildAudioTrack(ctx, cache, plans[0], destination);
    File result(destination, _T("rb"));
    std::vector<uint8_t> actual(3 * bytes.size());
    result.read(MemoryChunk(actual.data(), actual.size()));
    for (size_t i = 0; i < actual.size(); ++i) Require(actual[i] == bytes[i % bytes.size()], "PacketCacheコピーと無音の結合が不正です");
}

void BuilderDualMonoTest(const tstring& directory) {
    AMTContext ctx;
    auto dual = MakePceSilentFrame(true);
    AdtsParser parser(ctx);
    bool recognized = false;
    for (int i = 0; i < 3; ++i) {
        std::vector<AudioFrameData> frames;
        parser.inputFrame(MemoryChunk(dual.data(), dual.size()), frames, i * 1920);
        for (const auto& frame : frames) {
            Require(frame.format.channels == AUDIO_2LANG, "PCEの2SCE音声がデュアルモノとして判定されませんでした");
            recognized = true;
        }
    }
    Require(recognized, "cfg0デュアルモノをADTS parserで読めませんでした");
    auto stereo = GenerateSilentAdtsFrame(AUDIO_STEREO, 3);
    const auto source = directory + _T("/builder-dual-source.aac");
    {
        File file(source, _T("wb"));
        file.write(MemoryChunk(dual.data(), dual.size()));
        file.write(MemoryChunk(stereo.data(), stereo.size()));
        file.write(MemoryChunk(dual.data(), dual.size()));
    }
    std::vector<FileAudioFrameInfo> info(3);
    for (int i = 0; i < 3; ++i) {
        info[i].format = { i == 1 ? AUDIO_STEREO : AUDIO_2LANG, 48000 };
        info[i].numSamples = AAC_LC_FRAME_SAMPLES;
    }
    const auto plans = PlanSeparateAudioTracks({ { 0, 1, 2 } }, info,
        3.0 * AAC_LC_FRAME_SAMPLES * MPEG_CLOCK_HZ / 48000);
    PacketCache cache(ctx, source, { 0, static_cast<int64_t>(dual.size()),
        static_cast<int64_t>(dual.size() + stereo.size()), static_cast<int64_t>(dual.size() * 2 + stereo.size()) }, 2, 2);
    Require(plans.size() == 3, "デュアルモノ混在の出力トラック数が不正です");
    for (size_t track = 0; track < plans.size(); ++track) {
        const auto destination = directory + _T("/builder-dual-") + std::to_string(track) + _T(".aac");
        BuildAudioTrack(ctx, cache, plans[track], destination);
        File result(destination, _T("rb"));
        uint8_t header[7];
        result.read(MemoryChunk(header, 7));
        const int config = ((header[2] & 1) << 2) | (header[3] >> 6);
        Require(config == GetAudioAdtsChannelConfiguration(plans[track].layout), "デュアルモノ分離後のチャンネル構成が不正です");
    }
}

void BuilderRejectPceTest(const tstring& directory) {
    AMTContext ctx;
    auto bytes = MakePceSilentFrame();
    const auto source = directory + _T("/builder-pce-source.aac");
    {
        File file(source, _T("wb"));
        file.write(MemoryChunk(bytes.data(), bytes.size()));
    }
    PacketCache cache(ctx, source, { 0, static_cast<int64_t>(bytes.size()) }, 2, 2);
    for (const auto layout : { AUDIO_STEREO }) {
        std::vector<FileAudioFrameInfo> info(1);
        info[0].format = { layout, 48000 };
        info[0].numSamples = AAC_LC_FRAME_SAMPLES;
        const auto plans = PlanSeparateAudioTracks({ { 0 } }, info,
            1.0 * AAC_LC_FRAME_SAMPLES * MPEG_CLOCK_HZ / 48000);
        for (const auto& plan : plans) {
            bool rejected = false;
            try {
                BuildAudioTrack(ctx, cache, plan, directory + _T("/builder-pce-rejected-") + plan.name + _T(".aac"));
            } catch (const FormatException& error) {
                Require(tstring(error.message()).find(_T("ADTSヘッダ")) != tstring::npos,
                    "PCE音声がCOPYヘッダ検証で拒否されませんでした");
                rejected = true;
            }
            Require(rejected, "Builder直呼びでchannel_config=0が拒否されませんでした");
        }
    }
}

void BuilderMergeSilenceTest(const tstring& directory) {
    AMTContext ctx;
    for (const bool upmix : { false, true }) {
        const auto stereo = GenerateSilentAdtsFrame(AUDIO_STEREO, 3);
        const auto surround = GenerateSilentAdtsFrame(AUDIO_32_LFE, 3);
        const auto source = directory + (upmix ? _T("/merge-up-source.aac") : _T("/merge-down-source.aac"));
        const auto destination = directory + (upmix ? _T("/merge-up.aac") : _T("/merge-down.aac"));
        std::vector<FileAudioFrameInfo> info(3);
        std::vector<int64_t> offsets = { 0 };
        {
            File file(source, _T("wb"));
            for (int index = 0; index < 3; ++index) {
                const auto layout = upmix ? (index == 0 ? AUDIO_STEREO : AUDIO_32_LFE)
                    : (index == 2 ? AUDIO_32_LFE : AUDIO_STEREO);
                const auto& bytes = layout == AUDIO_STEREO ? stereo : surround;
                info[index].audioIdx = 0;
                info[index].PTS = index * 1920;
                info[index].format = { layout, 48000 };
                info[index].numSamples = AAC_LC_FRAME_SAMPLES;
                info[index].fileOffset = offsets.back();
                info[index].codedDataSize = static_cast<int>(bytes.size());
                offsets.push_back(offsets.back() + bytes.size());
                file.write(MemoryChunk(const_cast<uint8_t*>(bytes.data()), bytes.size()));
            }
        }
        const auto plans = PlanMergeAudioTracks({ { 0, 1, 2 } }, info, 3.0 * AAC_LC_FRAME_SAMPLES * MPEG_CLOCK_HZ / 48000);
        Require(plans.size() == 1 && plans.front().layout == (upmix ? AUDIO_32_LFE : AUDIO_STEREO), "統合変換の目標レイアウトが不正です");
        PacketCache cache(ctx, source, offsets, 2, 2);
        BuildAudioTrack(ctx, cache, plans.front(), destination, info);
    }
}

void ConverterExcludedWarmTest(const tstring& directory) {
    AMTContext ctx;
    constexpr int SOURCE_FRAME_COUNT = 4;
    constexpr int SOURCE_AUDIO_TICKS = 1920;
    const auto silent = GenerateSilentAdtsFrame(AUDIO_STEREO, 3);
    std::vector<FileAudioFrameInfo> info(SOURCE_FRAME_COUNT);
    std::vector<int64_t> offsets = { 0 };
    for (int index = 0; index < SOURCE_FRAME_COUNT; ++index) {
        info[index].audioIdx = 0;
        info[index].PTS = index * SOURCE_AUDIO_TICKS;
        info[index].format = { AUDIO_STEREO, 48000 };
        info[index].numSamples = AAC_LC_FRAME_SAMPLES;
        info[index].fileOffset = offsets.back();
        info[index].codedDataSize = static_cast<int>(silent.size());
        offsets.push_back(offsets.back() + silent.size());
    }
    AudioTrackPlan plan;
    plan.layout = AUDIO_32_LFE;
    plan.sampleRate = 48000;
    plan.samplingFrequencyIndex = 3;
    for (int index = 1; index < SOURCE_FRAME_COUNT; ++index) {
        plan.frames.push_back({ AudioTrackOperation::CONVERT, index, -1, AUDIO_STEREO, AUDIO_32_LFE });
    }
    Require(GetAudioTrackDecoderWarmFrames(plan, info) == std::vector<int>({ 0 }),
        "通常助走の元packet集合が実変換と一致しません");
    auto excludedPlan = plan;
    excludedPlan.excludedDecoderFrames = { 0 };
    Require(GetAudioTrackDecoderWarmFrames(excludedPlan, info).empty(),
        "最短除外packetが助走元集合へ戻りました");
    auto convert = [&](bool corrupt, bool excluded) {
        const auto source = directory + (corrupt ? _T("/warm-invalid-source.aac") : _T("/warm-normal-source.aac"));
        {
            File file(source, _T("wb"));
            for (int index = 0; index < SOURCE_FRAME_COUNT; ++index) {
                auto bytes = silent;
                if (corrupt && index == 0) bytes[0] = 0;
                file.write(MemoryChunk(bytes.data(), bytes.size()));
            }
        }
        auto current = plan;
        if (excluded) current.excludedDecoderFrames = { 0 };
        PacketCache cache(ctx, source, offsets, 2, 2);
        return ConvertAudioTrackRun(ctx, cache, current, 0, current.frames.size(), info);
    };
    const auto baseline = convert(false, false);
    Require(baseline.size() == plan.frames.size(), "通常助走の変換AAC数が不正です");
    bool rejected = false;
    try {
        convert(true, false);
    } catch (const FormatException& error) {
        Require(tstring(error.message()).find(_T("ADTSヘッダ")) != tstring::npos,
            "通常助走の不正ADTSがヘッダ検査で拒否されませんでした");
        rejected = true;
    }
    Require(rejected, "除外指定のない不正助走packetが黙って無視されました");
    Require(convert(true, true) == baseline, "最短除外助走の代替無音が正常packetと一致しません");
    Require(convert(false, true) == baseline, "正常助走packetの除外指定で変換AACが変わりました");
    std::cout << "Converter最短除外助走・通常不正ADTS拒否テスト成功\n";
}

namespace {
constexpr int MIN_DURATION_UNIT_TICKS = 48000;
constexpr int MIN_DURATION_VIDEO_TICKS = 3000;
constexpr int MIN_DURATION_AUDIO_TICKS = 1920;
constexpr int MIN_DURATION_THRESHOLD_SECONDS = 5;
constexpr int MIN_DURATION_SHORT_UNITS = 8;
constexpr int MIN_DURATION_LONG_UNITS = 24;
constexpr int MIN_DURATION_RESELECT_UNITS = 12;

enum class MinimumDurationFault { NONE, SAMPLE_RATE, UNKNOWN_RATE, SAMPLE_COUNT, LAYOUT, NON_LC, SFI, PCE, ADTS };

struct MinimumDurationSection {
    int units;
    AUDIO_CHANNELS layout;
    int tracks = 1;
    MinimumDurationFault fault = MinimumDurationFault::NONE;
    int videoDisplayWidth = 1920;
};

struct MinimumDurationFixture {
    std::vector<FileVideoFrameInfo> video;
    std::vector<FileAudioFrameInfo> audio;
    std::vector<StreamEvent> events;
    tstring audioPath;
};

MinimumDurationFixture MakeMinimumDurationFixture(const std::vector<MinimumDurationSection>& sections,
    const tstring& directory, const tstring& name) {
    MinimumDurationFixture fixture;
    fixture.audioPath = directory + _T("/minimum-") + name + _T(".aac");
    int totalUnits = 0;
    for (const auto& section : sections) totalUnits += section.units;
    const int totalVideo = totalUnits * MIN_DURATION_UNIT_TICKS / MIN_DURATION_VIDEO_TICKS;
    fixture.video.resize(totalVideo);
    for (int index = 0; index < totalVideo; ++index) {
        auto& frame = fixture.video[index];
        frame.PTS = frame.DTS = index * MIN_DURATION_VIDEO_TICKS;
        frame.pic = PIC_FRAME;
        frame.isGopStart = index % 30 == 0;
        frame.progressive = true;
        frame.format.format = VS_MPEG2;
        frame.format.width = frame.format.displayWidth = 1920;
        frame.format.height = frame.format.displayHeight = 1080;
        frame.format.frameRateNum = 30;
        frame.format.frameRateDenom = 1;
        frame.format.sarWidth = frame.format.sarHeight = 1;
        frame.format.progressive = true;
        frame.codedDataSize = 100;
    }
    File file(fixture.audioPath, _T("wb"));
    int64_t offset = 0;
    int sourceAudioFrame = 0;
    for (size_t sectionIndex = 0; sectionIndex < sections.size(); ++sectionIndex) {
        const auto& section = sections[sectionIndex];
        const int videoStart = sourceAudioFrame * MIN_DURATION_AUDIO_TICKS / MIN_DURATION_VIDEO_TICKS;
        const int firstAudio = static_cast<int>(fixture.audio.size());
        fixture.events.push_back({ PID_TABLE_CHANGED, videoStart, 0, section.tracks });
        const int sectionVideo = section.units * MIN_DURATION_UNIT_TICKS / MIN_DURATION_VIDEO_TICKS;
        for (int index = videoStart; index < videoStart + sectionVideo; ++index) {
            fixture.video[index].format.displayWidth = section.videoDisplayWidth;
        }
        if (sectionIndex == 0 || section.videoDisplayWidth != sections[sectionIndex - 1].videoDisplayWidth) {
            fixture.events.push_back({ VIDEO_FORMAT_CHANGED, videoStart, 0, 0 });
        }
        for (int track = 0; track < section.tracks; ++track) {
            fixture.events.push_back({ AUDIO_FORMAT_CHANGED, firstAudio + track, track, 0 });
        }
        const int sectionAudio = section.units * MIN_DURATION_UNIT_TICKS / MIN_DURATION_AUDIO_TICKS;
        for (int index = 0; index < sectionAudio; ++index, ++sourceAudioFrame) {
            for (int track = 0; track < section.tracks; ++track) {
                FileAudioFrameInfo frame;
                frame.PTS = sourceAudioFrame * MIN_DURATION_AUDIO_TICKS;
                frame.audioIdx = track;
                frame.numSamples = AAC_LC_FRAME_SAMPLES;
                frame.format = { track == 0 ? section.layout : AUDIO_STEREO, 48000 };
                auto bytes = frame.format.channels == AUDIO_2LANG ? MakePceSilentFrame(true)
                    : GenerateSilentAdtsFrame(frame.format.channels, 3);
                if (track == 0) {
                    switch (section.fault) {
                    case MinimumDurationFault::SAMPLE_RATE: frame.format.sampleRate = 44100; break;
                    case MinimumDurationFault::UNKNOWN_RATE: frame.format.sampleRate = 12345; break;
                    case MinimumDurationFault::SAMPLE_COUNT: frame.numSamples = 2048; break;
                    case MinimumDurationFault::LAYOUT: frame.format.channels = AUDIO_NONE; break;
                    case MinimumDurationFault::NON_LC: bytes[2] &= 0x3f; break;
                    case MinimumDurationFault::SFI: bytes[2] = (bytes[2] & 0xc3) | (15 << 2); break;
                    case MinimumDurationFault::PCE: bytes = MakePceSilentFrame(); break;
                    case MinimumDurationFault::ADTS: bytes[0] = 0; break;
                    default: break;
                    }
                }
                frame.fileOffset = offset;
                frame.codedDataSize = static_cast<int>(bytes.size());
                offset += bytes.size();
                fixture.audio.push_back(frame);
                file.write(MemoryChunk(bytes.data(), bytes.size()));
            }
        }
    }
    return fixture;
}

void MinimumDurationCmWaveTest(const tstring& directory) {
    constexpr int PCM_BYTES_PER_SAMPLE = 4;
    const int shortAudioFrames = MIN_DURATION_SHORT_UNITS * MIN_DURATION_UNIT_TICKS / MIN_DURATION_AUDIO_TICKS;
    auto fixture = MakeMinimumDurationFixture({ { MIN_DURATION_SHORT_UNITS, AUDIO_STEREO },
        { MIN_DURATION_LONG_UNITS, AUDIO_32_LFE } }, directory, _T("cm-wave-2048"));
    for (size_t index = 0; index < fixture.audio.size(); ++index) {
        fixture.audio[index].waveDataSize = AAC_LC_FRAME_SAMPLES * PCM_BYTES_PER_SAMPLE;
        fixture.audio[index].waveOffset = index * AAC_LC_FRAME_SAMPLES * PCM_BYTES_PER_SAMPLE;
    }
    const auto normalFixture = fixture;
    // 冒頭区間の音声を2048サンプル/42.667msへ作り直す。PCM長とPTSを同じ尺にそろえる。
    std::vector<FileAudioFrameInfo> audio;
    int64_t waveOffset = 0;
    for (int index = 0; index < static_cast<int>(fixture.audio.size()); ++index) {
        if (index < shortAudioFrames && index % 2 != 0) continue;
        auto frame = fixture.audio[index];
        if (index < shortAudioFrames) frame.numSamples = 2 * AAC_LC_FRAME_SAMPLES;
        frame.waveDataSize = frame.numSamples * PCM_BYTES_PER_SAMPLE;
        frame.waveOffset = waveOffset;
        waveOffset += frame.waveDataSize;
        audio.push_back(frame);
    }
    fixture.audio = std::move(audio);
    for (auto& event : fixture.events) {
        if (event.type == AUDIO_FORMAT_CHANGED && event.frameIdx >= shortAudioFrames) event.frameIdx -= shortAudioFrames / 2;
    }
    auto getWave = [&](AUDIO_FORMAT_CHANGE_MODE mode, bool normal = false) {
        const auto& current = normal ? normalFixture : fixture;
        AMTContext ctx;
        auto video = current.video;
        auto sourceAudio = current.audio;
        auto events = current.events;
        std::vector<CaptionItem> captions;
        std::vector<TimeInfo> times;
        StreamReformInfo reform(ctx, 1, video, sourceAudio, captions, events, times);
        // prepareのCMwaveだけを対象とし、出力ADTS検査や最短区間判定はまだ行わない。
        reform.prepare(false, false, false, mode);
        return std::vector<FilterAudioFrame>(reform.getFilterSourceAudioFrames(0));
    };
    // 初回は修正前の正常LC参照列を保存し、以後は同じ列と完全一致することを確認する。
    for (const auto mode : { AFC_SPLIT, AFC_MERGE, AFC_SEPARATE }) {
        const auto normalWave = getWave(mode, true);
        std::ostringstream serialized;
        for (const auto& frame : normalWave) serialized << frame.frameIndex << ' ' << frame.waveOffset << ' ' << frame.waveLength << '\n';
        const auto path = std::filesystem::path(directory + _T("/normal-lc-wave-") + std::to_string(static_cast<int>(mode)) + _T(".tsv"));
        std::ifstream previous(path);
        if (previous.good()) {
            std::ostringstream contents;
            contents << previous.rdbuf();
            Require(contents.str() == serialized.str(), "正常LCのprepare CMwave参照が修正前から変わりました");
        } else {
            std::ofstream captured(path);
            captured << serialized.str();
            Require(captured.good(), "正常LCのCMwave基準参照を保存できませんでした");
        }
        std::cout << "正常LC CMwave基準一致 モード=" << static_cast<int>(mode) << " 参照数=" << normalWave.size() << std::endl;
    }
    const auto baseline = getWave(AFC_SPLIT);
    auto report = [&](const std::vector<FilterAudioFrame>& wave, const char* name) {
        int64_t totalBytes = 0;
        int64_t shortBytes = 0;
        size_t shortReferences = 0;
        for (const auto& frame : wave) {
            totalBytes += frame.waveLength;
            if (frame.frameIndex >= 0 && frame.frameIndex < shortAudioFrames / 2) {
                shortBytes += frame.waveLength;
                ++shortReferences;
            }
        }
        std::cout << "CMwave " << name << " 参照数=" << wave.size() << " PCMバイト=" << totalBytes
            << " 2048区間参照数=" << shortReferences << " 2048区間PCMバイト=" << shortBytes << std::endl;
    };
    report(baseline, "split");
    bool matched = true;
    for (const auto mode : { AFC_MERGE, AFC_SEPARATE }) {
        const auto wave = getWave(mode);
        report(wave, mode == AFC_MERGE ? "merge" : "separate");
        const bool same = baseline.size() == wave.size() && std::equal(baseline.begin(), baseline.end(), wave.begin(),
            [](const FilterAudioFrame& a, const FilterAudioFrame& b) {
                return a.frameIndex == b.frameIndex && a.waveOffset == b.waveOffset && a.waveLength == b.waveLength;
            });
        matched &= same;
    }
    Require(matched, "2048サンプル短区間のprepare CMwave参照が旧splitの実尺割当と一致しません");
    std::cout << "CMwave metadata不適合区間の旧split互換テスト成功\n";
}

struct MinimumDurationResult {
    std::set<int> sourceAudioIndices;
    std::set<int> excludedDecoderIndices;
    std::map<std::pair<int, CMType>, std::set<int64_t>> videoPts;
    bool planned = true;
    size_t maximumTracks = 0;
    bool hasConvert = false;
};

MinimumDurationResult CollectMinimumDurationResult(const MinimumDurationFixture& fixture,
    AUDIO_FORMAT_CHANGE_MODE mode, int thresholdSeconds, const std::vector<CMType>& cmtypes,
    const std::vector<EncoderZone>& zones = {}, const std::vector<int>& divisions = {}, bool splitSub = false) {
    AMTContext ctx;
    auto video = fixture.video;
    auto audio = fixture.audio;
    auto events = fixture.events;
    std::vector<CaptionItem> captions;
    std::vector<TimeInfo> times;
    StreamReformInfo reform(ctx, 1, video, audio, captions, events, times);
    reform.prepare(splitSub, false, false, mode, fixture.audioPath);
    reform.applyCMZones(0, zones, divisions);
    reform.genAudio(cmtypes, thresholdSeconds);
    MinimumDurationResult result;
    for (const auto& key : reform.getOutFileKeys()) {
        const auto& file = reform.getEncodeFile(key);
        // splitは既存TranscodeManagerと同じ最終判定を使い、参照集合の基準とする。
        if (file.duration < thresholdSeconds * static_cast<double>(MPEG_CLOCK_HZ)) continue;
        if (file.videoFrames.empty()) continue;
        auto& pts = result.videoPts[{ key.div, key.cm }];
        const auto& frames = reform.getFilterSourceFrames(key.video);
        for (int index : file.videoFrames) pts.insert(frames[index].originalFramePTS);
        result.planned &= file.isAudioTrackPlanned;
        result.maximumTracks = std::max(result.maximumTracks, file.audioFrames.size());
        for (const auto& track : file.audioFrames) {
            for (int index : track) if (index >= 0) result.sourceAudioIndices.insert(index);
        }
        for (const auto& plan : file.audioTrackPlan) {
            result.excludedDecoderIndices.insert(plan.excludedDecoderFrames.begin(), plan.excludedDecoderFrames.end());
            for (const auto& reference : plan.frames) result.hasConvert |= reference.operation == AudioTrackOperation::CONVERT;
        }
        if (file.isAudioTrackPlanned) ValidateAudioTrackPlans(file.audioTrackPlan, file.duration);
    }
    return result;
}

size_t MinimumDurationFrameCount(const MinimumDurationResult& result) {
    size_t count = 0;
    for (const auto& entry : result.videoPts) count += entry.second.size();
    return count;
}

void VerifyMinimumDurationCase(const MinimumDurationFixture& fixture, size_t expectedFrames,
    bool expectConvert = false, const std::vector<CMType>& cmtypes = { CMTYPE_BOTH },
    const std::vector<EncoderZone>& zones = {}, const std::vector<int>& divisions = {}, bool splitSub = false, size_t expectedTracks = 1) {
    std::cout << "最短区間検証 " << fixture.audioPath << " cm=" << cmtypes.size() << " div=" << divisions.size() << " splitSub=" << splitSub << std::endl;
    const auto split = CollectMinimumDurationResult(fixture, AFC_SPLIT, MIN_DURATION_THRESHOLD_SECONDS, cmtypes, zones, divisions, splitSub);
    Require(MinimumDurationFrameCount(split) == expectedFrames, "最短区間テストのsplit基準フレーム数が想定と異なります");
    for (const auto mode : { AFC_MERGE, AFC_SEPARATE }) {
        const auto planned = CollectMinimumDurationResult(fixture, mode, MIN_DURATION_THRESHOLD_SECONDS, cmtypes, zones, divisions, splitSub);
        Require(planned.videoPts == split.videoPts, "最短区間除外後の映像PTSがsplitと一致しません");
        Require(std::includes(split.sourceAudioIndices.begin(), split.sourceAudioIndices.end(),
            planned.sourceAudioIndices.begin(), planned.sourceAudioIndices.end()),
            "除外したsplitキーの元音声が統合音声に参照されました");
        Require(planned.planned && planned.maximumTracks == (expectedFrames == 0 ? 0 : expectedTracks), "除外区間の異常または副音声が残区間へ影響しました");
        Require(planned.hasConvert == (mode == AFC_MERGE && expectConvert), "除外後のCONVERT有無が不正です");
    }
}
void RunMinimumDurationTests(const tstring& directory) {
    const MinimumDurationSection stereoShort = { MIN_DURATION_SHORT_UNITS, AUDIO_STEREO };
    const MinimumDurationSection surroundLong = { MIN_DURATION_LONG_UNITS, AUDIO_32_LFE };
    const size_t shortFrames = MIN_DURATION_SHORT_UNITS * MIN_DURATION_UNIT_TICKS / MIN_DURATION_VIDEO_TICKS;
    const size_t longFrames = MIN_DURATION_LONG_UNITS * MIN_DURATION_UNIT_TICKS / MIN_DURATION_VIDEO_TICKS;
    const auto beginning = MakeMinimumDurationFixture({ stereoShort, surroundLong }, directory, _T("beginning"));
    const auto middle = MakeMinimumDurationFixture({ surroundLong, stereoShort, surroundLong }, directory, _T("middle"));
    const auto end = MakeMinimumDurationFixture({ surroundLong, stereoShort }, directory, _T("end"));
    VerifyMinimumDurationCase(beginning, longFrames);
    VerifyMinimumDurationCase(middle, 2 * longFrames);
    VerifyMinimumDurationCase(end, longFrames);
    const auto aggregate = MakeMinimumDurationFixture({ stereoShort, surroundLong, stereoShort }, directory, _T("aggregate"));
    VerifyMinimumDurationCase(aggregate, 2 * shortFrames + longFrames, true);
    VerifyMinimumDurationCase(aggregate, longFrames, false, { CMTYPE_BOTH }, {}, {}, true);
    VerifyMinimumDurationCase(aggregate, longFrames, false, { CMTYPE_BOTH }, {},
        { 0, static_cast<int>(shortFrames + longFrames / 2), static_cast<int>(2 * shortFrames + longFrames) });
    VerifyMinimumDurationCase(aggregate, 2 * shortFrames + 2 * longFrames, true,
        { CMTYPE_BOTH, CMTYPE_NONCM, CMTYPE_CM }, { { 0, static_cast<int>(shortFrames) } });
    const std::vector<EncoderZone> edgesAndMiddle = {
        { 0, static_cast<int>(shortFrames) },
        { static_cast<int>(2 * shortFrames), static_cast<int>(3 * shortFrames) },
        { static_cast<int>(shortFrames + longFrames), static_cast<int>(2 * shortFrames + longFrames) }
    };
    VerifyMinimumDurationCase(aggregate, 3 * longFrames + 3 * shortFrames, true,
        { CMTYPE_BOTH, CMTYPE_EDGE_TRIM, CMTYPE_NONCM, CMTYPE_CM }, edgesAndMiddle);
    VerifyMinimumDurationCase(beginning, 0, false, { CMTYPE_CM }, { { 0, static_cast<int>(shortFrames) } });
    for (const auto mode : { AFC_MERGE, AFC_SEPARATE }) {
        const auto zeroThreshold = CollectMinimumDurationResult(beginning, mode, 0, { CMTYPE_BOTH });
        Require(MinimumDurationFrameCount(zeroThreshold) == shortFrames + longFrames && zeroThreshold.planned,
            "最短区間0秒で従来の音声区間が除外されました");
        Require(zeroThreshold.excludedDecoderIndices.empty(), "最短区間0秒で助走禁止frameが設定されました");
        const auto cmOnly = CollectMinimumDurationResult(beginning, mode, 0, { CMTYPE_NONCM },
            { { 0, static_cast<int>(shortFrames) } });
        Require(cmOnly.planned && cmOnly.excludedDecoderIndices.empty(), "CM除外音声が最短除外の助走禁止frameになりました");
    }
    const auto exact = MakeMinimumDurationFixture({ surroundLong }, directory, _T("exact"));
    for (const auto mode : { AFC_SPLIT, AFC_MERGE, AFC_SEPARATE }) {
        const auto result = CollectMinimumDurationResult(exact, mode, 10, { CMTYPE_NONCM }, { { 300, static_cast<int>(longFrames) } });
        Require(MinimumDurationFrameCount(result) == 300, "最短区間と同じ10秒のキーが除外されました");
    }
    const auto extraTrack = MakeMinimumDurationFixture({ { MIN_DURATION_SHORT_UNITS, AUDIO_STEREO, 2 }, surroundLong }, directory, _T("extra-track"));
    VerifyMinimumDurationCase(extraTrack, longFrames);
    const auto retainedExtraTrack = MakeMinimumDurationFixture({ stereoShort, { MIN_DURATION_LONG_UNITS, AUDIO_32_LFE, 2 } },
        directory, _T("retained-extra-track"));
    VerifyMinimumDurationCase(retainedExtraTrack, longFrames, false, { CMTYPE_BOTH }, {}, {}, false, 2);
    const auto dual = MakeMinimumDurationFixture({ { MIN_DURATION_SHORT_UNITS, AUDIO_2LANG }, surroundLong }, directory, _T("dual"));
    VerifyMinimumDurationCase(dual, longFrames);
    int faultIndex = 0;
    for (const auto fault : { MinimumDurationFault::SAMPLE_RATE, MinimumDurationFault::UNKNOWN_RATE,
        MinimumDurationFault::SAMPLE_COUNT, MinimumDurationFault::LAYOUT, MinimumDurationFault::NON_LC,
        MinimumDurationFault::SFI, MinimumDurationFault::PCE, MinimumDurationFault::ADTS }) {
        const auto excluded = MakeMinimumDurationFixture({ { MIN_DURATION_SHORT_UNITS, AUDIO_STEREO, 1, fault }, surroundLong },
            directory, _T("excluded-fault-") + std::to_string(faultIndex));
        VerifyMinimumDurationCase(excluded, longFrames);
        const auto retained = MakeMinimumDurationFixture({ stereoShort, { MIN_DURATION_LONG_UNITS, AUDIO_32_LFE, 1, fault } },
            directory, _T("retained-fault-") + std::to_string(faultIndex));
        const auto split = CollectMinimumDurationResult(retained, AFC_SPLIT, MIN_DURATION_THRESHOLD_SECONDS, { CMTYPE_BOTH });
        for (const auto mode : { AFC_MERGE, AFC_SEPARATE }) {
            const auto fallback = CollectMinimumDurationResult(retained, mode, MIN_DURATION_THRESHOLD_SECONDS, { CMTYPE_BOTH });
            Require(!fallback.planned && fallback.videoPts == split.videoPts,
                "残区間の異常によるsplitフォールバックがsplitの残フレームと一致しません");
        }
        ++faultIndex;
    }
    // splitの主Aは合計12.8秒、統合後の主Bは合計19.2秒。主変更後のAサブ各4.267秒も除外する。
    const MinimumDurationSection reselectedShort = { MIN_DURATION_SHORT_UNITS, AUDIO_STEREO, 1, MinimumDurationFault::ADTS };
    const auto reselected = MakeMinimumDurationFixture({ reselectedShort,
        { MIN_DURATION_RESELECT_UNITS, AUDIO_MONO, 1, MinimumDurationFault::NONE, 1280 }, reselectedShort,
        { MIN_DURATION_RESELECT_UNITS, AUDIO_32_LFE, 1, MinimumDurationFault::NONE, 1280 }, reselectedShort,
        { MIN_DURATION_RESELECT_UNITS, AUDIO_STEREO, 1, MinimumDurationFault::NONE, 1280 } },
        directory, _T("reselected-main"));
    const size_t reselectedLongFrames = 3 * MIN_DURATION_RESELECT_UNITS * MIN_DURATION_UNIT_TICKS / MIN_DURATION_VIDEO_TICKS;
    const auto originalSplit = CollectMinimumDurationResult(reselected, AFC_SPLIT, MIN_DURATION_THRESHOLD_SECONDS,
        { CMTYPE_BOTH }, {}, {}, true);
    Require(MinimumDurationFrameCount(originalSplit) == 3 * shortFrames + reselectedLongFrames,
        "主フォーマット再選択fixtureの元splitキーが不正です");
    std::set<int64_t> expectedReselectedPTS;
    for (const auto& frame : reselected.video) {
        if (frame.format.displayWidth == 1280) expectedReselectedPTS.insert(frame.PTS);
    }
    for (const auto mode : { AFC_MERGE, AFC_SEPARATE }) {
        const auto result = CollectMinimumDurationResult(reselected, mode, MIN_DURATION_THRESHOLD_SECONDS,
            { CMTYPE_BOTH }, {}, {}, true);
        Require(result.planned && MinimumDurationFrameCount(result) == reselectedLongFrames,
            "主再選択後に除外した短サブのADTS異常でsplitへ戻りました");
        Require(result.videoPts.size() == 1 && result.videoPts.begin()->second == expectedReselectedPTS,
            "主再選択後の残映像PTSが最終主フォーマットBと一致しません");
        Require(result.hasConvert == (mode == AFC_MERGE), "主再選択後のCONVERT有無が不正です");
        const size_t excludedAudioCount = 3 * MIN_DURATION_SHORT_UNITS * MIN_DURATION_UNIT_TICKS / MIN_DURATION_AUDIO_TICKS;
        const size_t expectedExcludedAudioCount = mode == AFC_MERGE ? excludedAudioCount : 0;
        if (result.excludedDecoderIndices.size() != expectedExcludedAudioCount) {
            throw std::runtime_error("主再選択後に除外した短サブの助走禁止frame数が不正です: モード="
                + std::to_string(static_cast<int>(mode)) + " 実値=" + std::to_string(result.excludedDecoderIndices.size())
                + " 期待=" + std::to_string(expectedExcludedAudioCount));
        }
        for (int index : result.excludedDecoderIndices) {
            Require(reselected.video[reselected.audio[index].PTS / MIN_DURATION_VIDEO_TICKS].format.displayWidth != 1280,
                "最終主フォーマットBの元音声が助走禁止frameに含まれました");
        }
    }
    std::cout << "最短区間・CM別キー・div・残区間事前検査テスト成功\n";
}

}

int main(int argc, char** argv) {
    try {
        if (argc > 2 && std::string(argv[2]) == "--cm-wave") {
            MinimumDurationCmWaveTest(argv[1]);
            return 0;
        }
        if (argc > 1) MinimumDurationCmWaveTest(argv[1]);
#ifndef AUDIO_TRACK_PLANNER_ONLY
        if (argc > 1) ConverterExcludedWarmTest(argv[1]);
#endif
        if (argc > 1) RunMinimumDurationTests(argv[1]);
        RunScenario(5, false);
        RunScenario(6, false);
        RunScenario(20, true);
        RunScenario(0, false, 1344);
        RunScenario(0, false, -576);
        RunScenario(5, false, 0, 0, tstring(), AFC_MERGE);
        RunScenario(6, false, 0, 0, tstring(), AFC_MERGE);
        RunScenario(20, true, 0, 0, tstring(), AFC_MERGE);
        RunScenario(0, false, 1344, 0, tstring(), AFC_MERGE);
        RunScenario(0, false, -576, 0, tstring(), AFC_MERGE);
        if (argc > 1) {
#ifndef AUDIO_TRACK_PLANNER_ONLY
            BuilderCopyTest(argv[1]);
            BuilderDualMonoTest(argv[1]);
            BuilderRejectPceTest(argv[1]);
            BuilderMergeSilenceTest(argv[1]);
#endif
            RunScenario(0, false, 0, 1);
            RunScenario(0, false, 0, 2);
            RunScenario(0, false, 0, 3, argv[1]);
            RunScenario(0, false, 0, 4, argv[1]);
            RunScenario(0, false, 0, 5, argv[1]);
            for (int fallback = 1; fallback <= 5; ++fallback) RunScenario(0, false, 0, fallback, argv[1], AFC_MERGE);
        }
        std::cout << "StreamReform合成入力テスト成功\n";
        return 0;
    } catch (const Exception& error) {
        std::cerr << error.message() << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
    }
    return 1;
}
