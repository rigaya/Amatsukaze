#include "StreamReform.h"
#include "TranscodeSetting.h"
#include "AudioTrackBuilder.h"
#include "AdtsParser.h"
#include <fstream>
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
    if (fallback != 0 && fallback != 5) {
        Require(reform.getNumEncoders(0) > 1, "未対応音声がジョブ全体のsplitに戻りません");
        return;
    }
    // cut時は、track1の最初の出現区間とtrack0の変更を含む範囲を落とす。
    std::vector<EncoderZone> zones;
    if (cut) zones.push_back({ 120, 220 });
    reform.applyCMZones(0, zones, {});
    reform.genAudio({ cut ? CMTYPE_NONCM : CMTYPE_BOTH });
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
            Require(previous >= 0 && current >= 0 && audioInfo[current].PTS - audioInfo[previous].PTS == audioTicks,
                "位相が0.7フレームの連続音声を再選択または欠落しました");
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

int main(int argc, char** argv) {
    try {
        RunScenario(5, false);
        RunScenario(6, false);
        RunScenario(20, true);
        RunScenario(0, false, 1344);
        RunScenario(5, false, 0, 0, tstring(), AFC_MERGE);
        RunScenario(6, false, 0, 0, tstring(), AFC_MERGE);
        RunScenario(20, true, 0, 0, tstring(), AFC_MERGE);
        RunScenario(0, false, 1344, 0, tstring(), AFC_MERGE);
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
