// プレビュー素材.datの既存バイナリ形式。
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "StreamReform.h"
#include "TrimAdjustAudio.h"

namespace amt_source_dat {
namespace detail {

struct SourceField {
    size_t offset;
    size_t size;
};

#define AMT_SOURCE_FIELD(type, field) { offsetof(type, field), sizeof(type::field) }

// 既存の.datは構造体サイズとフィールド位置を使う。値があるフィールドだけを同じ位置へ書く。
constexpr SourceField VIDEO_FORMAT_FIELDS[] = {
    AMT_SOURCE_FIELD(VideoFormat, format), AMT_SOURCE_FIELD(VideoFormat, width), AMT_SOURCE_FIELD(VideoFormat, height),
    AMT_SOURCE_FIELD(VideoFormat, displayWidth), AMT_SOURCE_FIELD(VideoFormat, displayHeight),
    AMT_SOURCE_FIELD(VideoFormat, sarWidth), AMT_SOURCE_FIELD(VideoFormat, sarHeight),
    AMT_SOURCE_FIELD(VideoFormat, frameRateNum), AMT_SOURCE_FIELD(VideoFormat, frameRateDenom),
    AMT_SOURCE_FIELD(VideoFormat, colorPrimaries), AMT_SOURCE_FIELD(VideoFormat, transferCharacteristics),
    AMT_SOURCE_FIELD(VideoFormat, colorSpace), AMT_SOURCE_FIELD(VideoFormat, progressive),
    AMT_SOURCE_FIELD(VideoFormat, fixedFrameRate), AMT_SOURCE_FIELD(VideoFormat, mpeg2ProfileAndLevelIndication),
    AMT_SOURCE_FIELD(VideoFormat, mpeg2BitRateValue), AMT_SOURCE_FIELD(VideoFormat, mpeg2VbvBufferSizeValue),
};
constexpr SourceField AUDIO_FORMAT_FIELDS[] = {
    AMT_SOURCE_FIELD(AudioFormat, channels), AMT_SOURCE_FIELD(AudioFormat, sampleRate),
};
constexpr SourceField VIDEO_FRAME_FIELDS[] = {
    AMT_SOURCE_FIELD(FilterSourceFrame, halfDelay), AMT_SOURCE_FIELD(FilterSourceFrame, frameIndex),
    AMT_SOURCE_FIELD(FilterSourceFrame, pts), AMT_SOURCE_FIELD(FilterSourceFrame, frameDuration),
    AMT_SOURCE_FIELD(FilterSourceFrame, originalFramePTS), AMT_SOURCE_FIELD(FilterSourceFrame, framePTS),
    AMT_SOURCE_FIELD(FilterSourceFrame, fileOffset), AMT_SOURCE_FIELD(FilterSourceFrame, keyFrame),
    AMT_SOURCE_FIELD(FilterSourceFrame, cmType),
};
constexpr SourceField AUDIO_FRAME_FIELDS[] = {
    AMT_SOURCE_FIELD(FilterAudioFrame, frameIndex), AMT_SOURCE_FIELD(FilterAudioFrame, waveOffset),
    AMT_SOURCE_FIELD(FilterAudioFrame, waveLength),
};
constexpr SourceField DECODER_FIELDS[] = {
    AMT_SOURCE_FIELD(DecoderSetting, mpeg2), AMT_SOURCE_FIELD(DecoderSetting, h264), AMT_SOURCE_FIELD(DecoderSetting, hevc),
};

#undef AMT_SOURCE_FIELD

template<typename Write, typename Value>
void WriteRaw(Write& write, const Value& value) {
    write(reinterpret_cast<const uint8_t*>(&value), sizeof(Value));
}

template<typename Write, typename Sequence>
void WriteRawArray(Write& write, const Sequence& values) {
    WriteRaw(write, static_cast<int64_t>(values.size()));
    if (!values.empty()) {
        write(reinterpret_cast<const uint8_t*>(values.data()), sizeof(typename Sequence::value_type) * values.size());
    }
}

template<typename Record, size_t FieldCount>
void CopySourceFields(uint8_t* destination, const Record& record, const SourceField (&fields)[FieldCount]) {
    const auto* source = reinterpret_cast<const uint8_t*>(&record);
    for (const auto& field : fields) {
        std::memcpy(destination + field.offset, source + field.offset, field.size);
    }
}

template<typename Write, typename Record, size_t FieldCount>
void WriteSourceValue(Write& write, const Record& record, const SourceField (&fields)[FieldCount]) {
    std::array<uint8_t, sizeof(Record)> bytes{};
    CopySourceFields(bytes.data(), record, fields);
    write(bytes.data(), bytes.size());
}

template<typename Write, typename Record, size_t FieldCount>
void WriteSourceArray(Write& write, const std::vector<Record>& records, const SourceField (&fields)[FieldCount]) {
    WriteRaw(write, static_cast<int64_t>(records.size()));
    constexpr size_t blockSize = 512;
    // 各レコードの未使用バイトをゼロにし、書き込みは複数レコードをまとめる。
    std::array<uint8_t, sizeof(Record) * blockSize> bytes{};
    for (size_t start = 0; start < records.size();) {
        const size_t count = std::min(blockSize, records.size() - start);
        for (size_t index = 0; index < count; ++index) {
            CopySourceFields(bytes.data() + index * sizeof(Record), records[start + index], fields);
        }
        write(bytes.data(), sizeof(Record) * count);
        start += count;
    }
}

} // namespace detail

template<typename Write>
void WriteMetadata(Write write, const tstring& videoPath, const tstring& audioPath,
    const VideoFormat& video, const AudioFormat& audio,
    const std::vector<FilterSourceFrame>& frames, const std::vector<FilterAudioFrame>& audioFrames,
    const DecoderSetting& decoder, const std::vector<uint8_t>& primaryAudioFrames) {
    detail::WriteRawArray(write, videoPath);
    detail::WriteRawArray(write, audioPath);
    detail::WriteSourceValue(write, video, detail::VIDEO_FORMAT_FIELDS);
    detail::WriteSourceValue(write, audio, detail::AUDIO_FORMAT_FIELDS);
    detail::WriteSourceArray(write, frames, detail::VIDEO_FRAME_FIELDS);
    detail::WriteSourceArray(write, audioFrames, detail::AUDIO_FRAME_FIELDS);
    detail::WriteSourceValue(write, decoder, detail::DECODER_FIELDS);
    // 既存の読込経路は末尾の音声拡張を無視する。
    if (!primaryAudioFrames.empty()) {
        detail::WriteRaw(write, trimadjust::AUDIO_PRIMARY_METADATA_MAGIC);
        detail::WriteRawArray(write, primaryAudioFrames);
    }
}

} // namespace amt_source_dat
