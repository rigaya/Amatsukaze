
#include "Subtitle.h"

SubtitleGenerator::SubtitleGenerator(AMTContext& ctx)
    : AMTObject(ctx) {}

void SubtitleGenerator::runWhisper(const tstring& whisperPath,
                                   const tstring& audioPath,
                                   const tstring& outDir,
                                   const tstring& extraOptions,
                                   bool isUtf8Log) {
    StringBuilderT sb;
    // 実行ファイルパスはConfigWrapperから取得したものをそのまま用いる
    sb.append(_T("\"%s\""), whisperPath);
    sb.append(_T(" \"%s\""), audioPath);
    sb.append(_T(" --output_dir \"%s\""), outDir);
    sb.append(_T(" -f srt vtt"));
    if (extraOptions.size() > 0) {
        sb.append(_T(" %s"), extraOptions);
    }

    const tstring cmd = sb.str();
    ctx.info("[Whisper起動]");
    ctx.infoF("%s", cmd);

    // ログ取りつつ起動
    auto process = std::unique_ptr<StdRedirectedSubProcess>(
        new StdRedirectedSubProcess(cmd, 10, isUtf8Log));

    // 実行完了待ち
    int ret = process->join();
    if (ret != 0) {
        ctx.error("↓↓↓↓↓↓Whisper最後の出力↓↓↓↓↓↓");
        for (auto v : process->getLastLines()) {
            v.push_back(0);
            ctx.errorF("%s", v.data());
        }
        ctx.error("↑↑↑↑↑↑Whisper最後の出力↑↑↑↑↑↑");
        THROWF(RuntimeException, "Whisper終了コード: 0x%x", ret);
    }
}
