#include "AudioTrackPlanner.h"
#include "StreamReform.h"

#include <cmath>
#include <map>

int GetAudioSamplingFrequencyIndex(int sampleRate) {
    static const int rates[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };
    for (int i = 0; i < static_cast<int>(sizeof(rates) / sizeof(rates[0])); i++) {
        if (rates[i] == sampleRate) return i;
    }
    return -1;
}

int GetAudioAdtsChannelConfiguration(AUDIO_CHANNELS layout) {
    switch (layout) {
    case AUDIO_MONO: return 1;
    case AUDIO_STEREO: return 2;
    case AUDIO_30: return 3;
    case AUDIO_31: return 4;
    case AUDIO_32: return 5;
    case AUDIO_32_LFE: return 6;
    default: return -1;
    }
}

namespace {
tstring TrackName(AUDIO_CHANNELS layout) {
    switch (layout) {
    case AUDIO_MONO: return _T("1ch");
    case AUDIO_STEREO: return _T("2ch");
    case AUDIO_30: return _T("3ch");
    case AUDIO_31: return _T("4ch");
    case AUDIO_32: return _T("5ch");
    case AUDIO_32_LFE: return _T("5.1ch");
    default: THROW(FormatException, "音声トラックのレイアウトが未対応です");
    }
    return tstring();
}
}

void ValidateAudioTrackPlans(const std::vector<AudioTrackPlan>& plans, double duration90kHz) {
    if (!std::isfinite(duration90kHz) || duration90kHz < 0) {
        THROW(FormatException, "音声トラックの映像時間が不正です");
    }
    if (plans.empty()) return;
    const size_t count = plans.front().frames.size();
    const int rate = plans.front().sampleRate;
    for (const auto& plan : plans) {
        const int samplingFrequencyIndex = GetAudioSamplingFrequencyIndex(plan.sampleRate);
        if (plan.frames.size() != count || plan.sampleRate != rate ||
            GetAudioAdtsChannelConfiguration(plan.layout) < 0 ||
            samplingFrequencyIndex < 0 || samplingFrequencyIndex != plan.samplingFrequencyIndex) {
            THROW(FormatException, "音声トラックの長さまたはフォーマットが一致しません");
        }
        const double frameDuration = static_cast<double>(AAC_LC_FRAME_SAMPLES) * MPEG_CLOCK_HZ / plan.sampleRate;
        if (std::abs(count * frameDuration - duration90kHz) > frameDuration + 1e-6) {
            THROW(FormatException, "音声トラックと映像の時間差が1フレームを超えています");
        }
        bool hasAudio = false;
        for (const auto& ref : plan.frames) {
            if (ref.dstLayout != plan.layout ||
                (ref.operation != AudioTrackOperation::COPY && ref.operation != AudioTrackOperation::SILENCE &&
                    ref.operation != AudioTrackOperation::CONVERT) ||
                (ref.operation == AudioTrackOperation::CONVERT &&
                    (ref.frameIndex < 0 || GetAudioAdtsChannelConfiguration(ref.srcLayout) < 0 ||
                        ref.srcLayout == plan.layout || ref.dualMonoChannel < -1 || ref.dualMonoChannel > 1)) ||
                (ref.operation == AudioTrackOperation::COPY &&
                    (ref.frameIndex < 0 || ref.srcLayout != plan.layout || ref.dualMonoChannel < -1 || ref.dualMonoChannel > 1))) {
                THROW(FormatException, "音声トラックの参照が不正です");
            }
            hasAudio |= ref.operation != AudioTrackOperation::SILENCE;
        }
        if (!hasAudio) THROW(FormatException, "全区間が無音の音声トラックが残っています");
    }
}

std::vector<AudioTrackPlan> PlanSeparateAudioTracks(
    const FileAudioFrameList& input, const std::vector<FileAudioFrameInfo>& frameInfo, double duration90kHz) {
    int sampleRate = 0;
    size_t frameCount = input.empty() ? 0 : input.front().size();
    for (const auto& track : input) {
        if (track.size() != frameCount) THROW(FormatException, "入力音声トラックのフレーム数が一致しません");
        for (int index : track) {
            if (index == -1) continue;
            if (index < 0 || static_cast<size_t>(index) >= frameInfo.size()) {
                THROW(FormatException, "音声フレーム参照が範囲外です");
            }
            const auto& info = frameInfo[index];
            if ((sampleRate != 0 && sampleRate != info.format.sampleRate) ||
                GetAudioSamplingFrequencyIndex(info.format.sampleRate) < 0 || info.numSamples != AAC_LC_FRAME_SAMPLES ||
                (info.format.channels != AUDIO_2LANG && GetAudioAdtsChannelConfiguration(info.format.channels) < 0)) {
                THROW(FormatException, "分離音声トラックに未対応のフォーマットが含まれています");
            }
            sampleRate = info.format.sampleRate;
        }
    }
    std::vector<AudioTrackPlan> output;
    for (size_t source = 0; source < input.size(); source++) {
        const auto& track = input[source];
        bool hasDualMono = false;
        for (int index : track) {
            if (index >= 0 && frameInfo[index].format.channels == AUDIO_2LANG) hasDualMono = true;
        }
        struct Candidate { AudioTrackPlan plan; size_t copies = 0; };
        std::vector<Candidate> candidates;
        for (int logical = 0; logical < (hasDualMono ? 2 : 1); logical++) {
            std::map<AUDIO_CHANNELS, size_t> counts;
            for (int index : track) {
                if (index < 0) continue;
                auto layout = frameInfo[index].format.channels;
                if (layout == AUDIO_2LANG) layout = AUDIO_MONO;
                else if (logical == 1) continue;
                counts[layout]++;
            }
            for (const auto& entry : counts) {
                Candidate candidate;
                candidate.copies = entry.second;
                auto& plan = candidate.plan;
                plan.sourceTrack = static_cast<int>(source);
                plan.logicalTrack = logical;
                plan.layout = entry.first;
                plan.sampleRate = sampleRate;
                plan.samplingFrequencyIndex = GetAudioSamplingFrequencyIndex(sampleRate);
                plan.name = StringFormat(_T("Audio%d"), plan.sourceTrack);
                if (hasDualMono) plan.name += StringFormat(_T("-lang%d"), logical + 1);
                plan.name += _T("-") + TrackName(plan.layout);
                plan.frames.reserve(frameCount);
                for (int index : track) {
                    AudioTrackReference ref;
                    ref.dstLayout = plan.layout;
                    ref.srcLayout = plan.layout;
                    if (index >= 0) {
                        auto layout = frameInfo[index].format.channels;
                        const bool dual = layout == AUDIO_2LANG;
                        if (dual) layout = AUDIO_MONO;
                        if (layout == plan.layout && (logical == 0 || dual)) {
                            ref.operation = AudioTrackOperation::COPY;
                            ref.frameIndex = index;
                            ref.dualMonoChannel = dual ? logical : -1;
                        }
                    }
                    plan.frames.push_back(ref);
                }
                candidates.push_back(std::move(candidate));
            }
        }
        // 元トラック内では時間を優先し、同じ時間ならチャンネル数、主音声の順で揃える。
        std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            if (a.copies != b.copies) return a.copies > b.copies;
            const int ac = GetAudioAdtsChannelConfiguration(a.plan.layout);
            const int bc = GetAudioAdtsChannelConfiguration(b.plan.layout);
            if (ac != bc) return ac > bc;
            return a.plan.logicalTrack < b.plan.logicalTrack;
        });
        for (auto& candidate : candidates) output.push_back(std::move(candidate.plan));
    }
    ValidateAudioTrackPlans(output, duration90kHz);
    return output;
}

std::vector<AudioTrackPlan> PlanMergeAudioTracks(
    const FileAudioFrameList& input, const std::vector<FileAudioFrameInfo>& frameInfo, double duration90kHz) {
    const auto separated = PlanSeparateAudioTracks(input, frameInfo, duration90kHz);
    std::vector<AudioTrackPlan> output;
    for (size_t source = 0; source < input.size(); source++) {
        for (int logical = 0; logical < 2; logical++) {
            const AudioTrackPlan* longest = nullptr;
            size_t longestCount = 0;
            for (const auto& plan : separated) {
                if (plan.sourceTrack != static_cast<int>(source) || plan.logicalTrack != logical) continue;
                const size_t count = std::count_if(plan.frames.begin(), plan.frames.end(), [](const AudioTrackReference& ref) {
                    return ref.operation == AudioTrackOperation::COPY;
                });
                if (!longest || count > longestCount || (count == longestCount &&
                    GetAudioAdtsChannelConfiguration(plan.layout) > GetAudioAdtsChannelConfiguration(longest->layout))) {
                    longest = &plan;
                    longestCount = count;
                }
            }
            if (!longest) continue;
            AudioTrackPlan merged = *longest;
            merged.name = StringFormat(_T("Audio%d"), static_cast<int>(output.size()));
            for (const auto& plan : separated) {
                if (plan.sourceTrack != static_cast<int>(source) || plan.logicalTrack != logical || plan.layout == merged.layout) continue;
                for (size_t index = 0; index < plan.frames.size(); index++) {
                    const auto& sourceRef = plan.frames[index];
                    if (sourceRef.operation != AudioTrackOperation::COPY) continue;
                    auto& targetRef = merged.frames[index];
                    targetRef = sourceRef;
                    targetRef.operation = AudioTrackOperation::CONVERT;
                    targetRef.dstLayout = merged.layout;
                }
            }
            output.push_back(std::move(merged));
        }
    }
    ValidateAudioTrackPlans(output, duration90kHz);
    return output;
}

namespace {
AudioTrackPlan ToPlan(const AudioTrackPlanForTest& src, const AudioTrackReferenceForTest* references) {
    AudioTrackPlan plan;
    plan.sourceTrack = src.sourceTrack;
    plan.logicalTrack = src.logicalTrack;
    plan.layout = (AUDIO_CHANNELS)src.layout;
    plan.sampleRate = src.sampleRate;
    plan.samplingFrequencyIndex = src.samplingFrequencyIndex;
    plan.name = src.name;
    for (size_t i = 0; i < src.frameCount; i++) {
        AudioTrackReference ref;
        ref.operation = (AudioTrackOperation)references[i].operation;
        ref.frameIndex = references[i].frameIndex;
        ref.dualMonoChannel = references[i].dualMonoChannel;
        ref.srcLayout = (AUDIO_CHANNELS)references[i].srcLayout;
        ref.dstLayout = (AUDIO_CHANNELS)references[i].dstLayout;
        plan.frames.push_back(ref);
    }
    return plan;
}
}

extern "C" int PlanAudioTracksForTest(int merge,
    const int* frameIndices, const size_t* trackLengths, size_t trackCount,
    const AudioTrackFrameInfoForTest* frameInfo, size_t frameInfoCount, double duration90kHz,
    AudioTrackPlanForTest* plans, size_t planCapacity, size_t* planCount,
    AudioTrackReferenceForTest* references, size_t referenceCapacity, size_t* referenceCount) {
    if (planCount == nullptr || referenceCount == nullptr || (trackCount > 0 && trackLengths == nullptr) ||
        (frameInfoCount > 0 && frameInfo == nullptr)) {
        return AUDIO_TRACK_PLAN_FOR_TEST_INVALID_ARGUMENT;
    }
    *planCount = 0;
    *referenceCount = 0;
    try {
        FileAudioFrameList input(trackCount);
        size_t offset = 0;
        for (size_t i = 0; i < trackCount; i++) {
            if (trackLengths[i] > 0 && frameIndices == nullptr) return AUDIO_TRACK_PLAN_FOR_TEST_INVALID_ARGUMENT;
            input[i].assign(frameIndices + offset, frameIndices + offset + trackLengths[i]);
            offset += trackLengths[i];
        }
        std::vector<FileAudioFrameInfo> info(frameInfoCount);
        for (size_t i = 0; i < frameInfoCount; i++) {
            info[i].format = { (AUDIO_CHANNELS)frameInfo[i].channels, frameInfo[i].sampleRate };
            info[i].numSamples = frameInfo[i].numSamples;
        }
        const auto result = merge ? PlanMergeAudioTracks(input, info, duration90kHz)
                                  : PlanSeparateAudioTracks(input, info, duration90kHz);
        size_t totalReferences = 0;
        for (const auto& plan : result) {
            if (plan.name.size() >= AUDIO_TRACK_PLAN_FOR_TEST_NAME_LENGTH) return AUDIO_TRACK_PLAN_FOR_TEST_INVALID_ARGUMENT;
            totalReferences += plan.frames.size();
        }
        *planCount = result.size();
        *referenceCount = totalReferences;
        if ((!result.empty() && (plans == nullptr || planCapacity < result.size())) || (totalReferences > 0 && (references == nullptr || referenceCapacity < totalReferences))) {
            return AUDIO_TRACK_PLAN_FOR_TEST_BUFFER_TOO_SMALL;
        }
        size_t refIndex = 0;
        for (size_t i = 0; i < result.size(); i++) {
            const auto& plan = result[i];
            auto& dst = plans[i];
            dst = {};
            dst.sourceTrack = plan.sourceTrack;
            dst.logicalTrack = plan.logicalTrack;
            dst.layout = plan.layout;
            dst.sampleRate = plan.sampleRate;
            dst.samplingFrequencyIndex = plan.samplingFrequencyIndex;
            std::copy(plan.name.begin(), plan.name.end(), dst.name);
            dst.frameCount = plan.frames.size();
            for (const auto& ref : plan.frames) {
                references[refIndex++] = { (int)ref.operation, ref.frameIndex, ref.dualMonoChannel, (int)ref.srcLayout, (int)ref.dstLayout };
            }
        }
        return AUDIO_TRACK_PLAN_FOR_TEST_SUCCESS;
    } catch (const FormatException&) {
        return AUDIO_TRACK_PLAN_FOR_TEST_REJECTED;
    } catch (...) {
        return AUDIO_TRACK_PLAN_FOR_TEST_INVALID_ARGUMENT;
    }
}

extern "C" int ValidateAudioTrackPlansForTest(
    const AudioTrackPlanForTest* plans, size_t planCount,
    const AudioTrackReferenceForTest* references, double duration90kHz) {
    if ((planCount > 0 && plans == nullptr) || references == nullptr) return AUDIO_TRACK_PLAN_FOR_TEST_INVALID_ARGUMENT;
    try {
        std::vector<AudioTrackPlan> input;
        for (size_t i = 0; i < planCount; i++) {
            input.push_back(ToPlan(plans[i], references));
            references += plans[i].frameCount;
        }
        ValidateAudioTrackPlans(input, duration90kHz);
        return AUDIO_TRACK_PLAN_FOR_TEST_SUCCESS;
    } catch (const FormatException&) {
        return AUDIO_TRACK_PLAN_FOR_TEST_REJECTED;
    } catch (...) {
        return AUDIO_TRACK_PLAN_FOR_TEST_INVALID_ARGUMENT;
    }
}
