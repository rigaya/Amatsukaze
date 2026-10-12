#include "AudioTrackBuilder.h"
#include "AdtsParser.h"
#include "AudioTrackConverter.h"

namespace {
constexpr int ADTS_HEADER_BYTES = 7;
constexpr int AAC_LC_PROFILE = 1;
constexpr int SILENT_GLOBAL_GAIN = 100;

void WriteSilentIcs(BitWriter& writer) {
    writer.write<8>(SILENT_GLOBAL_GAIN);
    writer.write<1>(0);
    writer.write<2>(0); // 長窓のみを使用する。
    writer.write<1>(0);
    writer.write<6>(0); // スペクトル係数を持たない。
    writer.write<1>(0);
    writer.write<1>(0);
    writer.write<1>(0);
    writer.write<1>(0);
}

class TrackDualMonoSplitter : public DualMonoSplitter {
public:
    TrackDualMonoSplitter(AMTContext& ctx, File& file) : DualMonoSplitter(ctx), file_(file) {}
    void write(MemoryChunk chunk, int channel) {
        channel_ = channel;
        written_ = false;
        inputPacket(chunk);
        if (!written_) THROW(FormatException, "デュアルモノ音声の分離に失敗しました");
    }
    void OnOutFrame(int index, MemoryChunk chunk) override {
        if (index == channel_) {
            file_.write(chunk);
            written_ = true;
        }
    }
private:
    File& file_;
    int channel_ = -1;
    bool written_ = false;
};
}

extern "C" int GenerateSilentAdtsFrameForTest(int layout, int samplingFrequencyIndex,
    uint8_t* output, size_t outputCapacity, size_t* frameLength) {
    constexpr int RESULT_INVALID = -1;
    constexpr int RESULT_BUFFER_TOO_SMALL = -2;
    if (frameLength == nullptr) return RESULT_INVALID;
    *frameLength = 0;
    try {
        const auto frame = GenerateSilentAdtsFrame((AUDIO_CHANNELS)layout, samplingFrequencyIndex);
        *frameLength = frame.size();
        if (output == nullptr || outputCapacity < frame.size()) return RESULT_BUFFER_TOO_SMALL;
        std::copy(frame.begin(), frame.end(), output);
        return 0;
    } catch (...) {
        return RESULT_INVALID;
    }
}

std::vector<uint8_t> GenerateSilentAdtsFrame(AUDIO_CHANNELS layout, int samplingFrequencyIndex) {
    const int config = GetAudioAdtsChannelConfiguration(layout);
    if (config < 0 || samplingFrequencyIndex < 0 || samplingFrequencyIndex > 12) {
        THROW(FormatException, "無音AACのフォーマットが未対応です");
    }
    AutoBuffer raw;
    BitWriter writer(raw);
    int sce = 0, cpe = 0;
    auto element = [&](int type) {
        writer.write<3>(type);
        writer.write<4>(type == ID_CPE ? cpe++ : type == ID_SCE ? sce++ : 0);
        if (type == ID_CPE) writer.write<1>(0);
        WriteSilentIcs(writer);
        if (type == ID_CPE) WriteSilentIcs(writer);
    };
    if (config == 1) element(ID_SCE);
    else if (config == 2) element(ID_CPE);
    else {
        element(ID_SCE);
        element(ID_CPE);
        if (config == 4) element(ID_SCE);
        if (config >= 5) element(ID_CPE);
        if (config == 6) element(ID_LFE);
    }
    writer.write<3>(ID_END);
    writer.byteAlign<false>();
    writer.flush();
    const int length = ADTS_HEADER_BYTES + static_cast<int>(raw.size());
    AutoBuffer result;
    BitWriter header(result);
    header.write<12>(0xfff);
    header.write<1>(0);
    header.write<2>(0);
    header.write<1>(1);
    header.write<2>(AAC_LC_PROFILE);
    header.write<4>(samplingFrequencyIndex);
    header.write<1>(0);
    header.write<3>(config);
    header.write<4>(0);
    header.write<13>(length);
    header.write<11>(0x7ff);
    header.write<2>(0);
    header.flush();
    result.add(raw.get());
    return std::vector<uint8_t>(result.ptr(), result.ptr() + result.size());
}

void BuildAudioTrack(AMTContext& ctx, PacketCache& cache, const AudioTrackPlan& plan, const tstring& path,
    const std::vector<FileAudioFrameInfo>& frameInfo) {
    auto silence = GenerateSilentAdtsFrame(plan.layout, plan.samplingFrequencyIndex);
    File file(path, _T("wb"));
    TrackDualMonoSplitter splitter(ctx, file);
    for (size_t position = 0; position < plan.frames.size(); position++) {
        const auto& ref = plan.frames[position];
        if (ref.dstLayout != plan.layout) THROW(FormatException, "出力音声のレイアウトが一致しません");
        switch (ref.operation) {
        case AudioTrackOperation::SILENCE:
            file.write(MemoryChunk(silence.data(), silence.size()));
            break;
        case AudioTrackOperation::COPY: {
            if (ref.frameIndex < 0 || ref.srcLayout != plan.layout || ref.dualMonoChannel < -1) {
                THROW(FormatException, "音声コピー参照が不正です");
            }
            auto chunk = cache[ref.frameIndex];
            AdtsHeader header;
            if (!header.parse(chunk.data, static_cast<int>(chunk.length)) || header.profile != AAC_LC_PROFILE ||
                header.sampling_frequency_index != plan.samplingFrequencyIndex ||
                header.number_of_raw_data_blocks_in_frame != 0 || header.frame_length != chunk.length ||
                (header.channel_configuration == 0 && ref.dualMonoChannel < 0)) {
                THROW(FormatException, "音声コピー元のADTSヘッダが不正です");
            }
            if (ref.dualMonoChannel >= 0) {
                if (plan.layout != AUDIO_MONO || ref.dualMonoChannel > 1) {
                    THROW(FormatException, "デュアルモノ音声の参照が不正です");
                }
                splitter.write(chunk, ref.dualMonoChannel);
            } else {
                if (header.channel_configuration != GetAudioAdtsChannelConfiguration(plan.layout)) {
                    THROW(FormatException, "音声コピー元のレイアウトが一致しません");
                }
                file.write(chunk);
            }
            break;
        }
        case AudioTrackOperation::CONVERT: {
            size_t end = position + 1;
            while (end < plan.frames.size() && plan.frames[end].operation == AudioTrackOperation::CONVERT &&
                plan.frames[end].srcLayout == ref.srcLayout && plan.frames[end].dualMonoChannel == ref.dualMonoChannel) end++;
            auto converted = ConvertAudioTrackRun(ctx, cache, plan, position, end, frameInfo);
            for (auto& frame : converted) file.write(MemoryChunk(frame.data(), frame.size()));
            position = end - 1;
            break;
        }
        default:
            THROW(FormatException, "音声トラックの出力操作が不正です");
        }
    }
}

namespace {
void CopyMessage(tchar* destination, size_t capacity, const tchar* source) {
    size_t i = 0;
    for (; source != nullptr && source[i] != 0 && i + 1 < capacity; i++) destination[i] = source[i];
    destination[i] = 0;
}

// channel_configuration=0 でPCEを持つ無音ADTS (旧StreamReformTestのMakePceSilentFrame)。
// dualMonoなら2つのSCE、そうでなければ1つのCPE (ステレオ)
std::vector<uint8_t> MakePceSilentFrameForTest(bool dualMono) {
    auto stereo = GenerateSilentAdtsFrame(AUDIO_STEREO, 3);
    AutoBuffer raw;
    BitWriter writer(raw);
    writer.write<3>(5); // PCE
    writer.write<4>(0);
    writer.write<2>(1); // AAC-LC
    writer.write<4>(3); // 48kHz
    writer.write<4>(dualMono ? 2 : 1); // 前方に2SCEまたは1CPE
    writer.write<4>(0);
    writer.write<4>(0);
    writer.write<2>(0);
    writer.write<3>(0);
    writer.write<4>(0);
    writer.write<1>(0);
    writer.write<1>(0);
    writer.write<1>(0);
    for (int element = 0; element < (dualMono ? 2 : 1); element++) {
        writer.write<1>(dualMono ? 0 : 1);
        writer.write<4>(element);
    }
    writer.byteAlign<false>();
    writer.write<8>(0); // コメントなし
    writer.flush();
    if (dualMono) {
        for (int channel = 0; channel < 2; channel++) {
            writer.write<3>(0); // SCE
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
}

extern "C" int BuildAudioTracksForTest(const BuildAudioTracksInputForTest* input,
    BuildAudioTracksPlanResultForTest* plans, size_t planCapacity, size_t* planCount,
    uint8_t* output, size_t outputCapacity, size_t* outputLength) {
    if (input == nullptr || planCount == nullptr || outputLength == nullptr || input->directory == nullptr ||
        (input->frameCount > 0 && (input->stream == nullptr || input->frameLengths == nullptr || input->frameChannels == nullptr)) ||
        (input->trackFrameCount > 0 && input->trackFrames == nullptr)) {
        return -1;
    }
    *planCount = 0;
    *outputLength = 0;
    try {
        AMTContext ctx;
        const tstring directory = input->directory;
        const tstring source = directory + _T("/builder-source.aac");
        std::vector<int64_t> offsets = { 0 };
        std::vector<FileAudioFrameInfo> info(input->frameCount);
        {
            File file(source, _T("wb"));
            size_t position = 0;
            for (size_t i = 0; i < input->frameCount; i++) {
                const size_t length = input->frameLengths[i];
                if (position + length > input->streamLength) return -1;
                info[i].audioIdx = 0;
                info[i].PTS = static_cast<int64_t>(i) * AAC_LC_FRAME_SAMPLES * MPEG_CLOCK_HZ / 48000;
                info[i].format = { (AUDIO_CHANNELS)input->frameChannels[i], 48000 };
                info[i].numSamples = AAC_LC_FRAME_SAMPLES;
                info[i].fileOffset = offsets.back();
                info[i].codedDataSize = static_cast<int>(length);
                file.write(MemoryChunk(const_cast<uint8_t*>(input->stream) + position, length));
                position += length;
                offsets.push_back(offsets.back() + length);
            }
        }
        const double duration = static_cast<double>(input->trackFrameCount) * AAC_LC_FRAME_SAMPLES * MPEG_CLOCK_HZ / 48000;
        std::vector<AudioTrackPlan> trackPlans;
        try {
            const FileAudioFrameList list = { std::vector<int>(input->trackFrames, input->trackFrames + input->trackFrameCount) };
            trackPlans = input->merge ? PlanMergeAudioTracks(list, info, duration) : PlanSeparateAudioTracks(list, info, duration);
        } catch (const FormatException&) {
            return 1;
        }
        PacketCache cache(ctx, source, offsets, 2, 2);
        std::vector<BuildAudioTracksPlanResultForTest> results(trackPlans.size());
        std::vector<uint8_t> outputs;
        for (size_t i = 0; i < trackPlans.size(); i++) {
            auto& result = results[i];
            result = {};
            result.layout = trackPlans[i].layout;
            result.logicalTrack = trackPlans[i].logicalTrack;
            CopyMessage(result.name, BUILD_AUDIO_TRACKS_NAME_LENGTH, trackPlans[i].name.c_str());
            const tstring destination = directory + _T("/builder-out-") + StringFormat(_T("%d"), (int)i) + _T(".aac");
            try {
                BuildAudioTrack(ctx, cache, trackPlans[i], destination, info);
            } catch (const FormatException& error) {
                result.status = 1;
                CopyMessage(result.message, BUILD_AUDIO_TRACKS_MESSAGE_LENGTH, error.message());
            }
            result.outputOffset = outputs.size();
            if (result.status == 0) {
                File written(destination, _T("rb"));
                const size_t size = static_cast<size_t>(written.size());
                outputs.resize(outputs.size() + size);
                written.read(MemoryChunk(outputs.data() + result.outputOffset, size));
                result.outputLength = size;
            }
        }
        *planCount = results.size();
        *outputLength = outputs.size();
        if ((!results.empty() && (plans == nullptr || planCapacity < results.size())) ||
            (!outputs.empty() && (output == nullptr || outputCapacity < outputs.size()))) {
            return -2;
        }
        std::copy(results.begin(), results.end(), plans);
        std::copy(outputs.begin(), outputs.end(), output);
        return 0;
    } catch (...) {
        return -1;
    }
}

extern "C" int RunAudioReformScenarioForTest(const AudioReformScenarioForTest* scenario,
    AudioReformResultForTest* result, int* track0, int64_t* track0Pts, int* track1, size_t trackCapacity) {
    if (scenario == nullptr || result == nullptr || track0 == nullptr || track0Pts == nullptr || track1 == nullptr ||
        (scenario->fallback >= 3 && scenario->directory == nullptr)) {
        return -1;
    }
    *result = {};
    try {
        AMTContext ctx;
        const int audioTicks = AAC_LC_FRAME_SAMPLES * MPEG_CLOCK_HZ / 48000;
        const int audioCount = 600;
        const int videoCount = 384;
        const int fallback = scenario->fallback;
        std::vector<FileVideoFrameInfo> video(videoCount);
        for (int i = 0; i < videoCount; i++) {
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
        for (int i = 0; i < audioCount; i++) {
            // トラック0の中央に閾値前後の欠落を置く。後半で5.1chへ切り替える。
            if (i < 100 || i >= 100 + scenario->missingFrames) {
                FileAudioFrameInfo frame;
                frame.PTS = i * audioTicks + scenario->sourcePhaseTicks;
                frame.numSamples = AAC_LC_FRAME_SAMPLES;
                frame.format = { i < 300 ? AUDIO_STEREO : AUDIO_32_LFE, 48000 };
                frame.audioIdx = 0;
                frame.codedDataSize = 14;
                frame.fileOffset = audio.size() * 14;
                audio.push_back(frame);
            }
            // トラック1は途中で現れ、いったん消え、再出現して映像より早く終わる。
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
        while (change < static_cast<int>(audio.size()) && audio[change].PTS < 300 * audioTicks) change++;
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
            const tstring directory = scenario->directory;
            audioPath = directory + (fallback == 3 ? _T("/non-lc-source.aac") : fallback == 4 ? _T("/pce-source.aac") : _T("/pce-dual-source.aac"));
            File file(audioPath, _T("wb"));
            int64_t offset = 0;
            for (auto& frame : audio) {
                auto bytes = fallback == 4 ? MakePceSilentFrameForTest(false)
                    : fallback == 5 && frame.audioIdx == 0 ? MakePceSilentFrameForTest(true)
                    : GenerateSilentAdtsFrame(frame.format.channels, 3);
                if (fallback == 3) bytes[2] &= 0x3f; // AAC Mainを指定し、非LC検出を検証する
                frame.fileOffset = offset;
                frame.codedDataSize = static_cast<int>(bytes.size());
                offset += bytes.size();
                file.write(MemoryChunk(bytes.data(), bytes.size()));
            }
        }
        std::vector<CaptionItem> captions;
        std::vector<TimeInfo> times;
        const auto audioInfo = audio;
        try {
            StreamReformInfo reform(ctx, 1, video, audio, captions, events, times);
            reform.prepare(false, false, false, (AUDIO_FORMAT_CHANGE_MODE)scenario->mode, audioPath);
            result->numEncoders = reform.getNumEncoders(0);
            if (fallback != 0 && fallback != 5) return 0;
            // cut時は、トラック1の最初の出現区間とトラック0の変更を含む範囲を落とす
            std::vector<EncoderZone> zones;
            if (scenario->cut) zones.push_back({ 120, 220 });
            reform.applyCMZones(0, zones, {});
            reform.genAudio({ scenario->cut ? CMTYPE_NONCM : CMTYPE_BOTH }, 0);
            const auto& keys = reform.getOutFileKeys();
            result->outFileCount = static_cast<int>(keys.size());
            if (keys.empty()) return 0;
            const auto& file = reform.getEncodeFile(keys[0]);
            result->trackCount = static_cast<int>(file.audioFrames.size());
            if (file.audioFrames.size() < 2) return 0;
            result->track0Count = static_cast<int>(file.audioFrames[0].size());
            result->track1Count = static_cast<int>(file.audioFrames[1].size());
            if (file.audioFrames[0].size() > trackCapacity || file.audioFrames[1].size() > trackCapacity) return -2;
            for (size_t i = 0; i < file.audioFrames[0].size(); i++) {
                track0[i] = file.audioFrames[0][i];
                track0Pts[i] = track0[i] >= 0 ? audioInfo[track0[i]].PTS : -1;
            }
            std::copy(file.audioFrames[1].begin(), file.audioFrames[1].end(), track1);
            result->planCount = static_cast<int>(file.audioTrackPlan.size());
            for (size_t i = 0; i < file.audioTrackPlan.size() && i < AUDIO_REFORM_MAX_PLANS; i++) {
                const auto& plan = file.audioTrackPlan[i];
                result->planFrameCount[i] = static_cast<int>(plan.frames.size());
                result->planHasConvert[i] = std::any_of(plan.frames.begin(), plan.frames.end(), [](const AudioTrackReference& ref) {
                    return ref.operation == AudioTrackOperation::CONVERT;
                }) ? 1 : 0;
                CopyMessage(result->planName[i], 16, plan.name.c_str());
            }
            try {
                ValidateAudioTrackPlans(file.audioTrackPlan, file.duration);
            } catch (const FormatException&) {
                result->validatePlans = 1;
            }
            const auto pcm = reform.getWaveInput(file.audioFrames[1]);
            if (!pcm.empty()) {
                result->pcmFrontFrameIndex = pcm.front().frameIndex;
                result->pcmFrontWaveLength = static_cast<int>(pcm.front().waveLength);
            } else {
                result->pcmFrontFrameIndex = -2;
            }
        } catch (const Exception& error) {
            CopyMessage(result->message, BUILD_AUDIO_TRACKS_MESSAGE_LENGTH, error.message());
            return 1;
        }
        return 0;
    } catch (...) {
        return -1;
    }
}

extern "C" int ConvertAdtsTrackForTest(const ConvertAdtsTrackInputForTest* input,
    uint8_t* output, size_t outputCapacity, size_t* length, tchar* message, size_t messageCapacity) {
    if (input == nullptr || length == nullptr || input->directory == nullptr || input->stream == nullptr ||
        input->frameLengths == nullptr || input->frameChannels == nullptr || input->refFrameIndex == nullptr ||
        input->refConvert == nullptr) {
        return -1;
    }
    *length = 0;
    if (message != nullptr && messageCapacity > 0) message[0] = 0;
    try {
        AMTContext ctx;
        const tstring source = tstring(input->directory) + _T("/convert-source.aac");
        const tstring destination = tstring(input->directory) + _T("/convert-out.aac");
        std::vector<int64_t> offsets = { 0 };
        std::vector<FileAudioFrameInfo> info(input->frameCount);
        {
            File file(source, _T("wb"));
            size_t position = 0;
            for (size_t i = 0; i < input->frameCount; i++) {
                const size_t size = input->frameLengths[i];
                if (position + size > input->streamLength) return -1;
                info[i].audioIdx = 0;
                info[i].PTS = input->framePts ? input->framePts[i]
                    : static_cast<int64_t>(i) * AAC_LC_FRAME_SAMPLES * MPEG_CLOCK_HZ / 48000;
                info[i].format = { (AUDIO_CHANNELS)input->frameChannels[i], 48000 };
                info[i].numSamples = AAC_LC_FRAME_SAMPLES;
                info[i].fileOffset = offsets.back();
                info[i].codedDataSize = static_cast<int>(size);
                file.write(MemoryChunk(const_cast<uint8_t*>(input->stream) + position, size));
                position += size;
                offsets.push_back(offsets.back() + size);
            }
        }
        AudioTrackPlan plan;
        plan.logicalTrack = input->dualMonoChannel >= 0 ? input->dualMonoChannel : 0;
        plan.layout = (AUDIO_CHANNELS)input->dstLayout;
        plan.sampleRate = 48000;
        plan.samplingFrequencyIndex = 3;
        for (size_t i = 0; i < input->refCount; i++) {
            const int index = input->refFrameIndex[i];
            if (index < 0 || static_cast<size_t>(index) >= input->frameCount) return -1;
            AudioTrackReference ref;
            ref.operation = input->refConvert[i] ? AudioTrackOperation::CONVERT : AudioTrackOperation::COPY;
            ref.frameIndex = index;
            ref.srcLayout = info[index].format.channels;
            ref.dstLayout = plan.layout;
            if (info[index].format.channels == AUDIO_2LANG) {
                ref.dualMonoChannel = input->dualMonoChannel;
                ref.srcLayout = AUDIO_MONO;
            }
            plan.frames.push_back(ref);
        }
        PacketCache cache(ctx, source, offsets, 4, 8);
        try {
            BuildAudioTrack(ctx, cache, plan, destination, info);
        } catch (const FormatException& error) {
            CopyMessage(message, messageCapacity, error.message());
            return 1;
        }
        File written(destination, _T("rb"));
        const size_t size = static_cast<size_t>(written.size());
        *length = size;
        if (output == nullptr || outputCapacity < size) return -2;
        written.read(MemoryChunk(output, size));
        return 0;
    } catch (...) {
        return -1;
    }
}
