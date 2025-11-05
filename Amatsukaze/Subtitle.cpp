
#include "Subtitle.h"
#include "rgy_filesystem.h"

bool exeIsWhisperCpp(const tstring& whisperPath) {
    const tstring whisperFileName = PathGetFilename(whisperPath);
    return (whisperFileName.find(_T("whisper-cli")) != tstring::npos);
}

SubtitleGenerator::SubtitleGenerator(AMTContext& ctx)
    : AMTObject(ctx) {}

void SubtitleGenerator::runWhisper(const tstring& whisperPath,
                                   const tstring& audioPath,
                                   const tstring& outDir,
                                   const tstring& outFileWithoutExt,
                                   const tstring& extraOptions,
                                   bool enableVtt,
                                   bool isUtf8Log) {
    const bool isWhisperCpp = exeIsWhisperCpp(whisperPath);

    StringBuilderT sb;
    sb.append(_T("\"%s\""), whisperPath);
    if (isWhisperCpp) {
        sb.append(_T(" -f \"%s\""), audioPath);
        sb.append(_T(" --output-file \"%s\""), outFileWithoutExt);
        sb.append(_T(" -osrt"));
        if (enableVtt) {
            sb.append(_T(" -ovtt"));
        }
        if (!rgy_directory_exists(outDir)) {
            CreateDirectoryRecursive(outDir.c_str());
        }
    } else {
        sb.append(_T(" \"%s\""), audioPath);
        sb.append(_T(" --output_dir \"%s\""), outDir);
        sb.append(_T(" -f srt"));
        if (enableVtt) {
            sb.append(_T(" vtt"));
        }
    }
    if (extraOptions.size() > 0) {
        sb.append(_T(" %s"), extraOptions);
    }

    const tstring cmd = sb.str();
    ctx.info("[Whisper起動]");
    ctx.infoF("Whisper type: %s", isWhisperCpp ? _T("whisper-cpp") : _T("faster-whisper"));
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
