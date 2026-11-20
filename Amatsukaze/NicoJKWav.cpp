#include "NicoJKAlignAudio.h"
#include "TranscodeSetting.h"

// nicojk_assから呼ばれるデコード用のモード。TSの主音声を16bit・8kHz・モノラルのWAVに書き、
// 先頭の時刻などを<WAV>.jsonに書く。サンプル0は映像の先頭で、音声のない所は無音で埋める。
int NicoJKWavMain(AMTContext& ctx, const ConfigWrapper& setting) {
    const auto wavPath = setting.getOutFileBaseWithoutPrefix();
    const auto audio = ReadNicoJKReferenceAudio(ctx, setting.getSrcFilePath(), setting.getServiceId());
    WriteNicoJKAlignWav(wavPath, audio.samples);

    const auto json = StringFormat("{\"start\": %.6f, \"start_rounded\": %.0f, \"end\": %.6f, \"duration\": %.6f, "
                                   "\"has_time\": %s, \"sid\": %d}\n",
                                   audio.firstFrameTimeExact, audio.firstFrameTime,
                                   audio.firstFrameTimeExact + audio.duration, audio.duration,
                                   audio.hasTime ? "true" : "false", audio.serviceId);
    File info(wavPath + _T(".json"), _T("wb"));
    info.write(MemoryChunk((uint8_t*)json.data(), json.size()));
    ctx.infoF(_T("実況用の音声を書きました: %.3f秒 / sid %d"), audio.duration, audio.serviceId);
    return 0;
}
