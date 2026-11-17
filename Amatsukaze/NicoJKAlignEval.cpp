#include "NicoJKAlign.h"
#include "NicoJKAlignAudio.h"
#include "TranscodeSetting.h"
#include <chrono>

// 開発用入口。製品の対象音声は再読込せず、1パス目のPCMを使う。
int NicoJKAlignEval(AMTContext& ctx, const ConfigWrapper& setting) {
    const auto begin = std::chrono::steady_clock::now();
    // 保存したPCMならデコードを省き、同じ入口で照合だけを再評価する。
    const bool pcmInput = setting.getSrcFilePath().size() >= 4 &&
                          setting.getSrcFilePath().substr(setting.getSrcFilePath().size() - 4) == _T(".f32");
    auto read = [&](const tstring& path) {
        if (!pcmInput)
            return ReadNicoJKReferenceAudio(ctx, path);
        File file(path, _T("rb"));
        if (file.size() % sizeof(float))
            THROW(FormatException, "評価用PCMの長さが不正です");
        NicoJKAlignAudio audio;
        audio.samples.resize(file.size() / sizeof(float));
        const size_t bytes = audio.samples.size() * sizeof(float);
        if (file.read(MemoryChunk((uint8_t*)audio.samples.data(), bytes)) != bytes)
            THROW(FormatException, "評価用PCMが途中で終わりました");
        audio.duration = audio.samples.size() / 8000.0;
        return audio;
    };
    const auto target = read(setting.getSrcFilePath());
    const auto reference = read(setting.getModeArgs());
    const double readSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    const auto result = AlignNicoJKAudio(target.samples, reference.samples);
    const auto base = setting.getOutFileBaseWithoutPrefix();
    File json(base + _T(".json"), _T("wb"));
    const auto text = "{\"result\":" + NicoJKAlignResultJSON(result, target.duration, reference.duration) +
                      StringFormat(",\"read_seconds\":%.6f,\"target_start\":%.6f,\"reference_start\":%.6f,"
                                   "\"target_start_exact\":%.6f,\"reference_start_exact\":%.6f,\"target_"
                                   "sid\":%d,\"reference_sid\":%d}",
                                   readSeconds, target.firstFrameTime, reference.firstFrameTime,
                                   target.firstFrameTimeExact, reference.firstFrameTimeExact, target.serviceId,
                                   reference.serviceId);
    json.write(MemoryChunk((uint8_t*)text.data(), text.size()));
    if (!pcmInput) {
        File targetRaw(base + _T(".target.f32"), _T("wb"));
        targetRaw.write(MemoryChunk((uint8_t*)target.samples.data(), target.samples.size() * sizeof(float)));
        File referenceRaw(base + _T(".reference.f32"), _T("wb"));
        referenceRaw.write(MemoryChunk((uint8_t*)reference.samples.data(), reference.samples.size() * sizeof(float)));
    }
    ctx.infoF(_T("音声照合: 読込%.3f秒、照合%.3f秒、区間%zu、%s"), readSeconds, result.seconds, result.segments.size(),
              result.success ? _T("成功") : _T("失敗"));
    return result.success ? 0 : 1;
}
