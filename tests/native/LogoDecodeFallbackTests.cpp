#include "LogoScan.h"
#include "LogoGopDecoder.h"
#include "cpu_info.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#if defined(_WIN32) || defined(_WIN64)
#include <io.h>
#else
#include <unistd.h>
#endif
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/opt.h>

AMATSUKAZE_API int ScanLogo(AMTContext*, const tchar*, int, const tchar*, const tchar*, const tchar*,
    int, int, int, int, int, int, logo::LOGO_ANALYZE_CB);
AMATSUKAZE_API void* LogoDecodeSession_Create(AMTContext*, const tchar*, int);
AMATSUKAZE_API void LogoDecodeSession_Delete(void*);
AMATSUKAZE_API int AutoDetectLogoRect(AMTContext*, const tchar*, int,
    int, int, int, int, int, int, int, int,
    int*, int*, int*, int*, int*, int*, double*, double*, double*,
    int*, int*, int*, int*, int*, int*, int*, int*, int*, int*,
    const tchar*, const tchar*, const tchar*, const tchar*, const tchar*, const tchar*, const tchar*,
    const tchar*, const tchar*, const tchar*, const tchar*, const tchar*, const tchar*, const tchar*,
    int, logo::LOGO_AUTODETECT_CB);
}

namespace {
using namespace logo::gopdecode;
constexpr int kWidth = 320, kHeight = 180, kFrames = 720, kService = 100;
constexpr int kLogoX = 228, kLogoY = 16, kLogoW = 56, kLogoH = 48;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class TemporaryDirectory {
    std::filesystem::path directory;
public:
    TemporaryDirectory() {
        directory = std::filesystem::temp_directory_path() / ("amt_logo_fallback_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        Require(std::filesystem::create_directory(directory), "一時ディレクトリを作成できない");
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
    std::filesystem::path file(const char* name) const { return directory / name; }
};

// テスト自身の標準エラーだけを一時ファイルへ向け、実際の公開APIの切り替えログを検査する。
class ErrorCapture {
    FILE* file = nullptr;
    int saved = -1;
    static int duplicate(int fd) {
#if defined(_WIN32) || defined(_WIN64)
        return _dup(fd);
#else
        return dup(fd);
#endif
    }
    static int replace(int from, int to) {
#if defined(_WIN32) || defined(_WIN64)
        return _dup2(from, to);
#else
        return dup2(from, to);
#endif
    }
    static void closeDescriptor(int fd) {
#if defined(_WIN32) || defined(_WIN64)
        _close(fd);
#else
        close(fd);
#endif
    }
public:
    ErrorCapture() {
        std::fflush(stderr);
        file = std::tmpfile();
        Require(file != nullptr, "ログ保存用の一時ファイルを作成できない");
        saved = duplicate(fileno(stderr));
        Require(saved >= 0 && replace(fileno(file), fileno(stderr)) >= 0, "標準エラーを切り替えられない");
    }
    ~ErrorCapture() {
        std::fflush(stderr);
        if (saved >= 0) { replace(saved, fileno(stderr)); closeDescriptor(saved); }
        if (file) std::fclose(file);
    }
    std::string text() {
        std::fflush(stderr);
        std::rewind(file);
        std::string result;
        char block[4096];
        for (size_t count; (count = std::fread(block, 1, sizeof(block), file)) != 0;) result.append(block, count);
        std::fseek(file, 0, SEEK_END);
        return result;
    }
};

struct ParametersDeleter { void operator()(AVCodecParameters* value) const { avcodec_parameters_free(&value); } };
struct Source {
    std::unique_ptr<AVCodecParameters, ParametersDeleter> parameters{avcodec_parameters_alloc()};
    std::vector<Packet> packets;
};

Source Encode() {
    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
    Require(encoder != nullptr, "MPEG-2エンコーダーがない");
    Codec codec(avcodec_alloc_context3(encoder));
    codec->width = kWidth; codec->height = kHeight; codec->pix_fmt = AV_PIX_FMT_YUV420P;
    codec->time_base = AVRational{1, 30}; codec->framerate = AVRational{30, 1};
    codec->gop_size = 12; codec->max_b_frames = 2; codec->thread_count = 1; codec->bit_rate = 3000000;
    codec->flags |= AV_CODEC_FLAG_BITEXACT;
    Check(av_opt_set_int(codec->priv_data, "sc_threshold", 1000000000, 0), "シーンチェンジ設定失敗");
    Check(avcodec_open2(codec.get(), encoder, nullptr), "エンコーダー初期化失敗");
    Source source;
    Check(avcodec_parameters_from_context(source.parameters.get(), codec.get()), "映像情報の複製失敗");
    Frame frame(av_frame_alloc());
    frame->format = codec->pix_fmt; frame->width = kWidth; frame->height = kHeight;
    Check(av_frame_get_buffer(frame.get(), 32), "画像領域確保失敗");
    auto receive = [&]() {
        for (;;) {
            Packet packet(av_packet_alloc(), PacketDeleter());
            const int result = avcodec_receive_packet(codec.get(), packet.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
            Check(result, "符号化パケット受信失敗");
            source.packets.push_back(std::move(packet));
        }
    };
    for (int i = 0; i < kFrames; i++) {
        Check(av_frame_make_writable(frame.get()), "画像の書き込み準備失敗");
        const int background = 32 + (i * 17) % 160;
        for (int y = 0; y < kHeight; y++) for (int x = 0; x < kWidth; x++) {
            // 時変の単色背景へ半透明のL字を重ね、回帰解析に十分な背景の変化を与える。
            const bool logo = (x >= 236 && x < 244 && y >= 24 && y < 56)
                || (x >= 236 && x < 276 && y >= 24 && y < 32);
            frame->data[0][y * frame->linesize[0] + x] = uint8_t(logo ? (background * 2 + 220 * 3) / 5 : background);
        }
        for (int plane = 1; plane < 3; plane++) for (int y = 0; y < kHeight / 2; y++)
            std::memset(frame->data[plane] + y * frame->linesize[plane], 128, kWidth / 2);
        frame->pts = i;
        Check(avcodec_send_frame(codec.get(), frame.get()), "符号化画像送信失敗"); receive();
    }
    Check(avcodec_send_frame(codec.get(), nullptr), "符号化終端失敗"); receive();
    return source;
}

void WriteTs(const Source& source, const std::filesystem::path& path, int updatePacket) {
    AVFormatContext* format = nullptr;
    const auto filename = path.u8string();
    Check(avformat_alloc_output_context2(&format, nullptr, "mpegts", filename.c_str()), "TS出力準備失敗");
    try {
        AVStream* stream = avformat_new_stream(format, nullptr);
        Require(stream != nullptr, "映像ストリーム作成失敗");
        Check(avcodec_parameters_copy(stream->codecpar, source.parameters.get()), "出力映像情報の複製失敗");
        stream->time_base = AVRational{1, 30};
        Check(av_opt_set_int(format->priv_data, "mpegts_service_id", kService, 0), "サービスID設定失敗");
        Check(avio_open(&format->pb, filename.c_str(), AVIO_FLAG_WRITE), "TS出力を開けない");
        Check(avformat_write_header(format, nullptr), "TSヘッダー出力失敗");
        for (size_t i = 0; i < source.packets.size(); i++) {
            Packet packet(av_packet_clone(source.packets[i].get()), PacketDeleter());
            if (int(i) == updatePacket) {
                const auto header = Inspect(*packet);
                Require(header.pictureOffset >= 0, "更新挿入先にピクチャーヘッダーがない");
                int offset = -1;
                for (int n = header.pictureOffset + 4; n + 4 < packet->size; n++)
                    if (packet->data[n] == 0 && packet->data[n + 1] == 0 && packet->data[n + 2] == 1) { offset = n; break; }
                Require(offset >= 0, "行列更新の挿入位置が見つからない");
                // 4個のloadフラグがすべて0の更新なので、通常復号の画像は一切変わらない。
                const uint8_t update[] = {0, 0, 1, 0xb5, 0x30};
                const int oldSize = packet->size;
                Check(av_grow_packet(packet.get(), sizeof(update)), "更新挿入領域の確保失敗");
                std::memmove(packet->data + offset + sizeof(update), packet->data + offset, oldSize - offset);
                std::memcpy(packet->data + offset, update, sizeof(update));
                Require(Inspect(*packet).matrixUpdate, "挿入した更新を並列復号の制限として検出できない");
            }
            packet->stream_index = stream->index;
            av_packet_rescale_ts(packet.get(), AVRational{1, 30}, stream->time_base);
            Check(av_interleaved_write_frame(format, packet.get()), "TSパケット出力失敗");
        }
        Check(av_write_trailer(format), "TS終端出力失敗");
    } catch (...) {
        if (format) { if (format->pb) avio_closep(&format->pb); avformat_free_context(format); }
        throw;
    }
    if (format->pb) avio_closep(&format->pb);
    avformat_free_context(format);
}

struct Progress {
    std::mutex mutex;
    int maxRead = 0, lastRead = 0, restartNotifications = 0, readBeforeRestart = 0;
    int cancelAt = -1;
    std::filesystem::path removeOnRetry;
};
Progress* activeProgress = nullptr;

bool AutoProgress(int stage, float, float, int read, int) {
    std::lock_guard<std::mutex> lock(activeProgress->mutex);
    activeProgress->maxRead = std::max(activeProgress->maxRead, read);
    if (stage == 1 && read == 0 && activeProgress->lastRead > 0 && activeProgress->restartNotifications++ == 0) {
        activeProgress->readBeforeRestart = activeProgress->maxRead;
        if (!activeProgress->removeOnRetry.empty()) {
            std::error_code error;
            Require(std::filesystem::remove(activeProgress->removeOnRetry, error), "再試行失敗検査用の入力を取り除けない");
        }
    }
    activeProgress->lastRead = read;
    return activeProgress->cancelAt < 0 || read < activeProgress->cancelAt;
}

bool GenerateProgress(float, int read, int, int) {
    std::lock_guard<std::mutex> lock(activeProgress->mutex);
    activeProgress->maxRead = std::max(activeProgress->maxRead, read);
    return activeProgress->cancelAt < 0 || read < activeProgress->cancelAt;
}

struct Detection {
    int ok = 0;
    int x = 0, y = 0, w = 0, h = 0, rectFail = 0, logoFail = 0;
    double scores[3]{};
    int details[10]{};
    std::string error, logs;
};

Detection Detect(AMTContext& context, const std::filesystem::path& source, int threads, int frames, Progress& progress) {
    activeProgress = &progress;
    Detection result;
    ErrorCapture capture;
    result.ok = AutoDetectLogoRect(&context, source.c_str(), kService, 2, 2, frames, 16, 12, 6, 6, threads,
        &result.x, &result.y, &result.w, &result.h, &result.rectFail, &result.logoFail,
        &result.scores[0], &result.scores[1], &result.scores[2],
        &result.details[0], &result.details[1], &result.details[2], &result.details[3], &result.details[4],
        &result.details[5], &result.details[6], &result.details[7], &result.details[8], &result.details[9],
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, AutoProgress);
    result.error = context.getError(); result.logs = capture.text();
    return result;
}

struct Generation { int ok = 0; std::string error, logs; std::vector<uint8_t> bytes; };
Generation Generate(AMTContext& context, const std::filesystem::path& source, const std::filesystem::path& output, Progress& progress) {
    activeProgress = &progress;
    Generation result;
    auto work = output;
    work += ".work";
    ErrorCapture capture;
    result.ok = ScanLogo(&context, source.c_str(), kService, work.c_str(), output.c_str(), nullptr,
        kLogoX, kLogoY, kLogoW, kLogoH, 12, 650, GenerateProgress);
    result.error = context.getError(); result.logs = capture.text();
    if (result.ok) {
        std::ifstream input(output, std::ios::binary);
        result.bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    return result;
}

size_t Count(const std::string& text, const std::string& needle) {
    size_t count = 0, offset = 0;
    while ((offset = text.find(needle, offset)) != std::string::npos) { count++; offset += needle.size(); }
    return count;
}

class Session {
    void* value;
public:
    Session(AMTContext& context, const std::filesystem::path& source) : value(LogoDecodeSession_Create(&context, source.c_str(), kService)) {
        Require(value != nullptr, "復号セッションを作成できない");
    }
    ~Session() { LogoDecodeSession_Delete(value); }
};

void SetSmallGopJobs() {
#if defined(_WIN32) || defined(_WIN64)
    _putenv_s("AMT_LOGO_GOPS_PER_JOB", "1");
    _putenv_s("AMT_LOGO_GOP_BUFFER_FRAMES", "1");
#else
    setenv("AMT_LOGO_GOPS_PER_JOB", "1", 1);
    setenv("AMT_LOGO_GOP_BUFFER_FRAMES", "1", 1);
#endif
}

void CheckDetectionEqual(const Detection& expected, const Detection& actual) {
    if (expected.ok != actual.ok || expected.x != actual.x || expected.y != actual.y || expected.w != actual.w || expected.h != actual.h
        || expected.rectFail != actual.rectFail || expected.logoFail != actual.logoFail) {
        std::cerr << expected.logs << actual.logs;
        throw std::runtime_error("再試行した枠検出結果が通常復号と異なる");
    }
    for (size_t i = 0; i < std::size(expected.scores); i++)
        Require(std::abs(expected.scores[i] - actual.scores[i]) < 0.0001, "再試行した枠検出スコアが通常復号と異なる");
}

void Test() {
    SetSmallGopJobs();
    TemporaryDirectory directory;
    const auto source = Encode();
    const auto clean = directory.file("clean.ts"), early = directory.file("early.ts"), late = directory.file("late.ts");
    WriteTs(source, clean, -1); WriteTs(source, early, 300); WriteTs(source, late, 600);
    AMTContext context;
    Progress baselineProgress;
    const auto baseline = Detect(context, clean, 1, 500, baselineProgress);
    Require(baseline.ok, "基準入力の枠検出に失敗した");
    Progress fallbackProgress;
    const auto fallback = Detect(context, early, 2, 500, fallbackProgress);
    CheckDetectionEqual(baseline, fallback);
    Require(fallbackProgress.readBeforeRestart >= 200, "途中まで走査してから失敗する条件を通っていない");
    Require(Count(fallback.logs, "GOP decode fallback:") == 1, "枠検出で通常復号への再試行が記録されていない");

    Progress cleanGenProgress;
    const auto cleanGeneration = Generate(context, clean, directory.file("clean.lgd"), cleanGenProgress);
    if (!cleanGeneration.ok) std::cerr << cleanGeneration.logs;
    Require(cleanGeneration.ok && !cleanGeneration.bytes.empty(), "基準入力のロゴ生成に失敗した");
    Progress generationProgress;
    const auto generation = Generate(context, late, directory.file("retry.lgd"), generationProgress);
    if (!generation.ok) std::cerr << generation.logs;
    Require(generation.ok && generation.bytes == cleanGeneration.bytes, "通常復号で作り直したLGDが基準と異なる");
    Require(Count(generation.logs, "GOP decode fallback:") == 1, "ロゴ生成で通常復号への再試行が記録されていない");

    // 枠検出の再試行後はセッションの通常復号設定を生成へ引き継ぎ、同じ更新で再び失敗しない。
    {
        Session session(context, early);
        Progress detectProgress;
        const auto detected = Detect(context, early, 2, 500, detectProgress);
        CheckDetectionEqual(baseline, detected);
        Require(Count(detected.logs, "GOP decode fallback:") == 1,
            "セッションありの枠検出を再試行していない");
        Progress cachedProgress;
        const auto cached = Generate(context, early, directory.file("cached_after_detect_retry.lgd"), cachedProgress);
        if (!cached.ok) std::cerr << cached.logs;
        Require(cached.ok && cached.bytes == cleanGeneration.bytes, "枠検出再試行後のセッションで生成したLGDが基準と異なる");
        Require(Count(cached.logs, "GOP decode fallback:") == 0,
            "枠検出で通常復号へ切り替えたセッションを再び並列復号している");
    }

    // 枠検出で保留したデコーダーとキャッシュを使い切った後の失敗も、生成全体からやり直す。
    {
        Session session(context, late);
        Progress detectProgress;
        const auto detected = Detect(context, late, 2, 100, detectProgress);
        Require(detected.ok, "生成引き継ぎ用の枠検出に失敗した");
        Progress cachedProgress;
        const auto cached = Generate(context, late, directory.file("cached.lgd"), cachedProgress);
        if (!cached.ok) std::cerr << cached.logs;
        Require(cached.ok && cached.bytes == cleanGeneration.bytes, "セッションを破棄した再生成のLGDが基準と異なる");
        Require(Count(cached.logs, "GOP decode fallback:") == 1, "キャッシュ利用後の再試行が記録されていない");
    }

    Progress cancelledAuto; cancelledAuto.cancelAt = 100;
    const auto autoCancel = Detect(context, early, 2, 500, cancelledAuto);
    Require(!autoCancel.ok && autoCancel.error.find("Cancel requested") != std::string::npos,
        "枠検出のキャンセルを失敗として返していない");
    Require(Count(autoCancel.logs, "GOP decode fallback:") == 0, "枠検出のキャンセルを再試行している");
    Progress cancelledGen; cancelledGen.cancelAt = 200;
    const auto genCancel = Generate(context, late, directory.file("cancelled.lgd"), cancelledGen);
    Require(!genCancel.ok && genCancel.error.find("Cancel requested") != std::string::npos,
        "ロゴ生成のキャンセルを失敗として返していない");
    Require(Count(genCancel.logs, "GOP decode fallback:") == 0, "ロゴ生成のキャンセルを再試行している");

    // 再試行開始の通知直後にテスト用入力を除き、通常復号側の失敗が無限再試行にならないことを確認する。
    const auto missing = directory.file("missing_on_retry.ts"); WriteTs(source, missing, 300);
    Progress missingProgress; missingProgress.removeOnRetry = missing;
    const auto failedRetry = Detect(context, missing, 2, 500, missingProgress);
    Require(!failedRetry.ok && missingProgress.restartNotifications == 1, "通常復号側の失敗を1回で終了していない");
    Require(Count(failedRetry.logs, "GOP decode fallback:") == 1, "通常復号側の失敗を検査できていない");
    Require(failedRetry.error.find(u8"GOP並列復号の失敗:") != std::string::npos
        && failedRetry.error.find(u8"通常復号での再試行の失敗:") != std::string::npos,
        "再試行失敗で両方の原因を保持していない");
    std::puts("ロゴ復号の再試行: 枠検出、生成、キャッシュ破棄、キャンセル、再試行失敗の検証成功");
}
}

int main() {
    // 1CPUではGOP並列復号へ入れないため、この専用テストを省略する。
    if (get_cpu_info().logical_cores == 1) {
        std::puts("1CPU環境のためGOP並列復号の再試行テストを省略します");
        return 77;
    }
    try { Test(); return 0; }
    catch (const std::exception& exception) { std::fprintf(stderr, "%s\n", exception.what()); return 1; }
}
