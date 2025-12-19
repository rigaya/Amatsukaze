/**
* Amtasukaze Avisynth Source Plugin
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include "common.h"
#include "AudioEncoder.h"


void wave::set4(int8_t dst[4], const char* src) {
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
    dst[3] = src[3];
} // namespace wave {

void EncodeAudio(AMTContext& ctx, const tstring& encoder_args,
    const tstring& audiopath, const AudioFormat& afmt,
    const std::vector<FilterAudioFrame>& audioFrames) {
    using namespace wave;

    ctx.info("[音声エンコーダ起動]");
    ctx.infoF("%s", encoder_args);

    auto process = std::unique_ptr<StdRedirectedSubProcess>(
        new StdRedirectedSubProcess(encoder_args, 5));

    int nchannels = 2;
    int bytesPerSample = 2;
    Header header;
    set4(header.chunkID, "RIFF");
    header.chunkSize = 0; // サイズ未定
    set4(header.format, "WAVE");
    set4(header.subchunk1ID, "fmt ");
    header.subchunk1Size = 16;
    header.audioFormat = 1;
    header.numChannels = nchannels;
    header.sampleRate = afmt.sampleRate;
    header.byteRate = afmt.sampleRate * bytesPerSample * nchannels;
    header.blockAlign = bytesPerSample * nchannels;
    header.bitsPerSample = bytesPerSample * 8;
    set4(header.subchunk2ID, "data");
    header.subchunk2Size = 0; // サイズ未定

    process->write(MemoryChunk((uint8_t*)&header, sizeof(header)));

    int audioSamplesPerFrame = 1024;
    // waveLengthはゼロのこともあるので注意
    for (int i = 0; i < (int)audioFrames.size(); i++) {
        if (audioFrames[i].waveLength != 0) {
            audioSamplesPerFrame = audioFrames[i].waveLength / 4; // 16bitステレオ前提
            break;
        }
    }

    File srcFile(audiopath, _T("rb"));
    AutoBuffer buffer;
    int frameWaveLength = audioSamplesPerFrame * bytesPerSample * nchannels;
    MemoryChunk mc = buffer.space(frameWaveLength);
    mc.length = frameWaveLength;

    for (size_t i = 0; i < audioFrames.size(); i++) {
        if (audioFrames[i].waveLength != 0) {
            // waveがあるなら読む
            srcFile.seek(audioFrames[i].waveOffset, SEEK_SET);
            srcFile.read(mc);
        } else {
            // ない場合はゼロ埋めする
            memset(mc.data, 0x00, mc.length);
        }
        process->write(mc);
    }

    process->finishWrite();
    int ret = process->join();
    if (ret != 0) {
        ctx.error("↓↓↓↓↓↓音声エンコーダ最後の出力↓↓↓↓↓↓");
        for (auto v : process->getLastLines()) {
            v.push_back(0); // null terminate
            ctx.errorF("%s", v.data());
        }
        ctx.error("↑↑↑↑↑↑音声エンコーダ最後の出力↑↑↑↑↑↑");
        THROWF(RuntimeException, "音声エンコーダ終了コード: 0x%x", ret);
    }
}
