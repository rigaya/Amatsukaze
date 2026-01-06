/**
* Amtasukaze Avisynth Source Plugin
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include "FilteredSource.h"
#include <algorithm>
#include <cmath>
#include <string>

namespace {

struct PtsGapOffsetFunc {
    double tickMs = 0.0;                 // nominal ms per frame (CFR timeline)
    std::vector<double> offsets;         // offsets[k] in ms at t = k * tickMs

    bool valid() const {
        return tickMs > 0.0 && std::isfinite(tickMs) && !offsets.empty();
    }

    // time t is in the same timeline as timeCodes (ms, starting at 0)
    double at(const double t) const {
        if (!valid()) return 0.0;
        if (!(t > 0.0) || !std::isfinite(t)) return offsets.front();
        const double maxT = (offsets.size() - 1) * tickMs;
        if (t >= maxT) return offsets.back();
        const double pos = t / tickMs;
        const int idx = std::max(0, std::min((int)offsets.size() - 2, (int)std::floor(pos)));
        const double frac = std::max(0.0, std::min(1.0, pos - idx));
        return offsets[idx] + (offsets[idx + 1] - offsets[idx]) * frac;
    }
};

static int inferVfrTimingFps(const std::vector<double>& timeCodes) {
    if (timeCodes.empty()) return 0;
    double minDiff = timeCodes.back();
    int best = 0;
    double epsilon = timeCodes.size() * 10e-10;
    for (auto fps : { 60, 120, 240 }) {
        double mult = fps / 1001.0;
        double inv = 1.0 / mult;
        double diff = 0;
        for (auto ts : timeCodes) {
            diff += std::abs(inv * std::round(ts * mult) - ts);
        }
        if (diff < minDiff - epsilon) {
            best = fps;
            minDiff = diff;
        }
    }
    return best;
}

static void writeTimecodeV2(const tstring& path, const std::vector<double>& timeCodes) {
    if (path.size() == 0 || timeCodes.size() < 2) return;
    File file(path, _T("wb"));
    const char header[] = "# timecode format v2\n";
    file.write(MemoryChunk((uint8_t*)header, sizeof(header) - 1));
    // timecode v2: 1行=各フレーム開始時刻(ms). 最終フレーム末尾は含めない。
    const int n = (int)timeCodes.size() - 1;
    for (int i = 0; i < n; i++) {
        const int64_t value = (int64_t)std::llround(timeCodes[i]);
        std::string line = std::to_string((long long)value);
        line.push_back('\n');
        file.write(MemoryChunk((uint8_t*)line.data(), (int)line.size()));
    }
}

static PtsGapOffsetFunc buildPtsGapOffsetFunc(const StreamReformInfo& reformInfo, EncodeFileKey key, const double tickMs, AMTContext& ctx) {
    PtsGapOffsetFunc func;
    func.tickMs = tickMs;
    if (!(tickMs > 0.0) || !std::isfinite(tickMs)) {
        return func;
    }
    const auto& outFrames = reformInfo.getEncodeFile(key).videoFrames;
    const auto& srcFrames = reformInfo.getFilterSourceFrames(key.video);
    if (outFrames.empty() || srcFrames.empty()) {
        return func;
    }
    const int firstIndex = outFrames.front();
    if (firstIndex < 0 || firstIndex >= (int)srcFrames.size()) {
        return func;
    }
    const double basePts = srcFrames[firstIndex].pts;
    func.offsets.resize(outFrames.size(), 0.0);
    double prev = 0.0;
    double maxGap = 0.0;
    const double LOG_EPS_MS = 1.0; // 1ms以上の増分のみログ（細かな丸め誤差は無視）
    for (int k = 0; k < (int)outFrames.size(); k++) {
        const int idx = outFrames[k];
        if (idx < 0 || idx >= (int)srcFrames.size()) {
            func.offsets[k] = prev;
            continue;
        }
        const double tPtsMs = (srcFrames[idx].pts - basePts) * 1000.0 / MPEG_CLOCK_HZ;
        const double tCfrMs = k * tickMs;
        double off = tPtsMs - tCfrMs;
        if (!std::isfinite(off)) {
            off = prev;
        }
        // drop補正は「時間の穴」を足す方向なので、単調非減少に丸める
        off = std::max(off, prev);
        const double delta = off - prev;
        if (delta >= LOG_EPS_MS) {
            ctx.infoF("tsreplace drop補正: +%.3f ms (累積 %.3f ms) at frame=%d (src=%d, tCFR=%.3f ms, tPTS=%.3f ms)",
                delta, off, k, idx, tCfrMs, tPtsMs);
        }
        func.offsets[k] = off;
        prev = off;
        maxGap = std::max(maxGap, off);
    }
    if (maxGap > 0.5) {
        ctx.infoF("tsreplace drop補正: 推定ギャップ合計 %.3f ms", maxGap);
    }
    return func;
}

} // namespace

void RFFExtractor::clear() {
    prevFrame_ = nullptr;
}

void RFFExtractor::inputFrame(av::EncodeWriter& encoder, std::unique_ptr<av::Frame>&& frame, PICTURE_TYPE pic) {

    // PTSはinputFrameで再定義されるので修正しないでそのまま渡す
    switch (pic) {
    case PIC_FRAME:
    case PIC_TFF:
    case PIC_TFF_RFF:
        encoder.inputFrame(*frame);
        break;
    case PIC_FRAME_DOUBLING:
        encoder.inputFrame(*frame);
        encoder.inputFrame(*frame);
        break;
    case PIC_FRAME_TRIPLING:
        encoder.inputFrame(*frame);
        encoder.inputFrame(*frame);
        encoder.inputFrame(*frame);
        break;
    case PIC_BFF:
        encoder.inputFrame(*mixFields(
            (prevFrame_ != nullptr) ? *prevFrame_ : *frame, *frame));
        break;
    case PIC_BFF_RFF:
        encoder.inputFrame(*mixFields(
            (prevFrame_ != nullptr) ? *prevFrame_ : *frame, *frame));
        encoder.inputFrame(*frame);
        break;
    default:
        break;
    }

    prevFrame_ = std::move(frame);
}

// 2つのフレームのトップフィールド、ボトムフィールドを合成
/* static */ std::unique_ptr<av::Frame> RFFExtractor::mixFields(av::Frame& topframe, av::Frame& bottomframe) {
    auto dstframe = std::unique_ptr<av::Frame>(new av::Frame());

    AVFrame* top = topframe();
    AVFrame* bottom = bottomframe();
    AVFrame* dst = (*dstframe)();

    // フレームのプロパティをコピー
    av_frame_copy_props(dst, top);

    // メモリサイズに関する情報をコピー
    dst->format = top->format;
    dst->width = top->width;
    dst->height = top->height;

    // メモリ確保
    if (av_frame_get_buffer(dst, 64) != 0) {
        THROW(RuntimeException, "failed to allocate frame buffer");
    }

    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get((AVPixelFormat)(dst->format));
    int pixel_shift = (desc->comp[0].depth > 8) ? 1 : 0;
    int nplanes = (dst->format != AV_PIX_FMT_NV12) ? 3 : 2;

    for (int i = 0; i < nplanes; i++) {
        int hshift = (i > 0 && dst->format != AV_PIX_FMT_NV12) ? desc->log2_chroma_w : 0;
        int vshift = (i > 0) ? desc->log2_chroma_h : 0;
        int wbytes = (dst->width >> hshift) << pixel_shift;
        int height = dst->height >> vshift;

        for (int y = 0; y < height; y += 2) {
            uint8_t* dst0 = dst->data[i] + dst->linesize[i] * (y + 0);
            uint8_t* dst1 = dst->data[i] + dst->linesize[i] * (y + 1);
            uint8_t* src0 = top->data[i] + top->linesize[i] * (y + 0);
            uint8_t* src1 = bottom->data[i] + bottom->linesize[i] * (y + 1);
            memcpy(dst0, src0, wbytes);
            memcpy(dst1, src1, wbytes);
        }
    }

    return dstframe;
}

/* static */ PICTURE_TYPE getPictureTypeFromAVFrame(AVFrame* frame) {
#if LIBAVUTIL_VERSION_MAJOR >= 58
    const bool interlaced = (frame->flags & AV_FRAME_FLAG_INTERLACED) != 0;
    const bool tff = (frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) != 0;
#else
    const bool interlaced = frame->interlaced_frame != 0;
    const bool tff = frame->top_field_first != 0;
#endif
    const int repeat = frame->repeat_pict;
    if (interlaced == false) {
        switch (repeat) {
        case 0: return PIC_FRAME;
        case 1: return tff ? PIC_TFF_RFF : PIC_BFF_RFF;
        case 2: return PIC_FRAME_DOUBLING;
        case 4: return PIC_FRAME_TRIPLING;
        default: THROWF(FormatException, "Unknown repeat count: %d", repeat);
        }
        return PIC_FRAME;
    } else {
        if (repeat) {
            THROW(FormatException, "interlaced and repeat ???");
        }
        return tff ? PIC_TFF : PIC_BFF;
    }
}
StringBuilder& AMTFilterSource::AvsScript::Get() { return append; }
void AMTFilterSource::AvsScript::Apply(IScriptEnvironment* env) {
    auto str = append.str();
    if (str.size() > 0) {
        append.clear();
        script += str;
        // 最後の結果は自分でlastに入れなければならないことに注意
        //（これをしないと最後のフィルタ呼び出しの直前がlastになってしまう）
        env->SetVar("last", env->Invoke("Eval", str.c_str()));
    }
}
void AMTFilterSource::AvsScript::Clear() {
    script.clear();
    append.clear();
}
const std::string& AMTFilterSource::AvsScript::Str() const {
    return script;
}

void AMTFilterSource::readTimecodeFile(const tstring& filepath) {
    File file(filepath, _T("r"));
    std::regex re("#\\s*total:\\s*([+-]?([0-9]*[.])?[0-9]+).*");
    std::string str;
    timeCodes_.clear();
    while (file.getline(str)) {
        if (str.size()) {
            std::smatch m;
            if (std::regex_search(str, m, re)) {
                timeCodes_.push_back(std::atof(m[1].str().c_str()) * 1000);
                return;
            } else if (str[0] != '#') {
                timeCodes_.push_back(std::atoi(str.c_str()));
            }
        }
    }
    // 合計時間を推測
    size_t numFrames = timeCodes_.size();
    if (numFrames >= 2) {
        timeCodes_.push_back(timeCodes_[numFrames - 1] * 2 - timeCodes_[numFrames - 2]);
    } else if (numFrames == 1) {
        timeCodes_.push_back(timeCodes_[0] + 1000.0 / 60.0);
    }
}

void AMTFilterSource::readTimecode(EncodeFileKey key) {
    auto timecodepath = setting_.getAvsTimecodePath(key);
    // timecodeファイルがあったら読み込む
    if (File::exists(timecodepath)) {
        readTimecodeFile(timecodepath);
        // ベースFPSを推測
        // フレームタイミングとの差の和が最も小さいFPSをベースFPSとする
        double minDiff = timeCodes_.back();
        double epsilon = timeCodes_.size() * 10e-10;
        for (auto fps : { 60, 120, 240 }) {
            double mult = fps / 1001.0;
            double inv = 1.0 / mult;
            double diff = 0;
            for (auto ts : timeCodes_) {
                diff += std::abs(inv * std::round(ts * mult) - ts);
            }
            if (diff < minDiff - epsilon) {
                vfrTimingFps_ = fps;
                minDiff = diff;
            }
        }
    }
}
// Main (+ Post)
AMTFilterSource::AMTFilterSource(AMTContext&ctx,
    const ConfigWrapper& setting,
    const StreamReformInfo& reformInfo,
    const std::vector<EncoderZone>& zones,
    const tstring& logopath,
    EncodeFileKey key,
    const ResourceManger& rm)
    : AMTObject(ctx)
    , setting_(setting)
    , env_(make_unique_ptr((IScriptEnvironment2*)nullptr))
    , vfrTimingFps_(0) {
    try {
        // フィルタ前処理用リソース確保
        auto res = rm.wait(HOST_CMD_Filter);

        int pass = 0;
        for (; pass < 4; pass++) {
            if (!FilterPass(pass, res.gpuIndex, key, reformInfo, logopath)) {
                break;
            }
            ReadAllFrames(pass);
        }

        // エンコード用リソース確保
        auto encodeRes = rm.request(HOST_CMD_Encode);
        if (encodeRes.IsFailed() || encodeRes.gpuIndex != res.gpuIndex) {
            // 確保できなかった or GPUが変更されたら 一旦解放する
            env_ = nullptr;
            if (encodeRes.IsFailed()) {
                // リソースが確保できていなかったら確保できるまで待つ
                encodeRes = rm.wait(HOST_CMD_Encode);
            }
        }

        // エンコード用リソースでアフィニティを設定
        res = encodeRes;
        SetCPUAffinity(res.group, res.mask);
        if (env_ == nullptr) {
            FilterPass(pass, res.gpuIndex, key, reformInfo, logopath);
        }

        auto& sb = script_.Get();
        tstring postpath = setting.getPostFilterScriptPath();
        if (postpath.size()) {
            sb.append("AMT_SOURCE = last\n");
            sb.append("Import(\"%s\")\n", postpath);
        }

        auto durationpath = setting_.getAvsDurationPath(key);
        // durationファイルがAMTDecimateを挟む
        if (File::exists(durationpath)) {
            sb.append("AMTDecimate(\"%s\")\n", durationpath);
        }

        readTimecode(key);

        if (setting_.isDumpFilter()) {
            sb.append("DumpFilterGraph(\"%s\", 1)\n",
                setting_.getFilterGraphDumpPath(key));
            // メモリデバッグ用 2000フレームごとにグラフダンプ
            //sb.append("DumpFilterGraph(\"%s\", 2, 2000, true)\n",
            //	setting_.getFilterGraphDumpPath(fileId, encoderId, cmtype));
        }

        script_.Apply(env_.get());
        filter_ = env_->GetVar("last").AsClip();
        writeScriptFile(key);

        MakeZones(key, zones, reformInfo);

        MakeOutFormat(reformInfo.getFormat(key).videoFormat);

        // tsreplace時は、入力TS(PCR/PTS)時間軸に同期させるため、
        // 既存のVFR timecode(フィルタ生成)を保持しつつdropギャップ分を加算補正する。
        if (setting_.getFormat() == FORMAT_TSREPLACE && filter_ != nullptr) {
            const auto vi = filter_->GetVideoInfo();
            const int nframes = vi.num_frames;
            if (nframes > 0) {
                // 入力側の nominal tick (CFR timeline) はフィルタ入力フォーマットに合わせる
                const auto& infmt = reformInfo.getFormat(key).videoFormat;
                const double tickMs = (infmt.frameRateNum > 0)
                    ? (infmt.frameRateDenom * 1000.0 / (double)infmt.frameRateNum)
                    : (1000.0 / 30.0);
                const auto offsetFunc = buildPtsGapOffsetFunc(reformInfo, key, tickMs, ctx);

                // drop補正が不要（ギャップが実質0）の場合は、timecodeの生成/上書きを行わない
                // - フィルタが既にtimecodeを生成している場合は、そのまま利用する（調整はしない）
                // - timecodeが無い場合は、tsreplaceのために新規生成もしない（元動作維持）
                const double DROP_CORR_EPS_MS = 1.0;
                const bool needDropCorrection = offsetFunc.valid() && (offsetFunc.offsets.back() >= DROP_CORR_EPS_MS);
                if (!needDropCorrection) {
                    // 既存 timeCodes_ を変更しない
                } else {
                    // timeCodes_ が無い/不正なら、まずCFRとして生成しておく
                    if (timeCodes_.empty() || (int)timeCodes_.size() != nframes + 1) {
                        const double tick = (vi.fps_numerator > 0)
                            ? (vi.fps_denominator * 1000.0 / (double)vi.fps_numerator)
                            : (1000.0 / 30.0);
                        timeCodes_.resize(nframes + 1);
                        for (int i = 0; i <= nframes; i++) {
                            timeCodes_[i] = i * tick;
                        }
                    }
                    // 0起点に正規化
                    const double base = timeCodes_.front();
                    if (base != 0.0) {
                        for (auto& t : timeCodes_) t -= base;
                    }

                    std::vector<double> corrected(timeCodes_.size(), 0.0);
                    double prev = 0.0;
                    for (size_t i = 0; i < timeCodes_.size(); i++) {
                        const double t = timeCodes_[i];
                        double c = t + offsetFunc.at(t);
                        if (!std::isfinite(c)) c = prev;
                        c = std::max(c, prev);
                        corrected[i] = c;
                        prev = c;
                    }
                    // 0起点に再正規化（安全のため）
                    const double cbase = corrected.front();
                    if (cbase != 0.0) {
                        for (auto& t : corrected) t -= cbase;
                    }
                    timeCodes_ = std::move(corrected);
                    if (vfrTimingFps_ == 0) {
                        vfrTimingFps_ = inferVfrTimingFps(timeCodes_);
                    }
                    // timecodeファイルを上書き出力（timelineeditor / tsreplaceへ渡す）
                    writeTimecodeV2(setting_.getAvsTimecodePath(key), timeCodes_);
                }
            }
        }
    } catch (const AvisynthError& avserror) {
        // デバッグ用にスクリプトは保存しておく
        writeScriptFile(key);
        // AvisynthErrorはScriptEnvironmentに依存しているので
        // AviSynthExceptionに変換する
        THROWF(AviSynthException, "%s", avserror.msg);
    }
}

AMTFilterSource::~AMTFilterSource() {
    filter_ = nullptr;
    env_ = nullptr;
}

AMTFilterSource::AMTFilterSource(AMTContext& ctx, const AMTFilterSource& source)
    : AMTObject(ctx)
    , setting_(source.setting_)
    , env_(make_unique_ptr(CreateScriptEnvironment2()))
    , vfrTimingFps_(source.vfrTimingFps_) {
    // 既存インスタンスのスクリプト全文を新しい環境で再評価してフィルタグラフを再構築
    script_.Clear();
    auto& sb = script_.Get();
    sb.append("%s", source.getScript().c_str());
    script_.Apply(env_.get());
    filter_ = env_->GetVar("last").AsClip();

    // メタデータをコピー（スレッドごとに環境は別）
    outfmt_ = source.outfmt_;
    outZones_ = source.outZones_;
    timeCodes_ = source.timeCodes_;
}

const PClip& AMTFilterSource::getClip() const {
    return filter_;
}

std::string AMTFilterSource::getScript() const {
    return script_.Str();
}

const VideoFormat& AMTFilterSource::getFormat() const {
    return outfmt_;
}

// 入力ゾーンのtrim後のゾーンを返す
const std::vector<EncoderZone> AMTFilterSource::getZones() const {
    return outZones_;
}

// 各フレームの時間ms(最後のフレームの表示時間を定義するため要素数はフレーム数+1)
const std::vector<double>& AMTFilterSource::getTimeCodes() const {
    return timeCodes_;
}

int AMTFilterSource::getVfrTimingFps() const {
    return vfrTimingFps_;
}

IScriptEnvironment2* AMTFilterSource::getEnv() const {
    return env_.get();
}

void AMTFilterSource::writeScriptFile(EncodeFileKey key) {
    auto& str = script_.Str();
    File avsfile(setting_.getFilterAvsPath(key), _T("w"));
    avsfile.write(MemoryChunk((uint8_t*)str.c_str(), str.size()));
}

std::vector<tstring> AMTFilterSource::GetSuitablePlugins(const tstring& basepath) {
    struct Plugin {
        tstring FileName;
        tstring BaseName;
    };
    if (rgy_directory_exists(basepath) == false) return std::vector<tstring>();
    std::vector<tstring> categories = { _T("_avx2.dll"), _T("_avx.dll"), _T(".dll") };
    std::vector<std::vector<Plugin>> categoryList(categories.size());
    for (tstring filename : GetDirectoryFiles(basepath, _T("*.dll"))) {
        std::transform(filename.begin(), filename.end(), filename.begin(), ::tolower);
        for (int i = 0; i < (int)categories.size(); i++) {
            const auto& category = categories[i];
            if (ends_with(filename, category)) {
                auto baseName = filename.substr(0, filename.size() - category.size());
                Plugin plugin = { filename, baseName };
                categoryList[i].push_back(plugin);
                break;
            }
        }
    }
    int support = 2;
    if (IsAVX2Available()) {
        support = 0;
    } else if (IsAVXAvailable()) {
        support = 1;
    }
    // BaseName -> FileName
    std::map<tstring, tstring> pluginMap;
    for (int i = (int)categories.size() - 1; i >= support; --i) {
        for (auto& plugin : categoryList[i]) {
            pluginMap[plugin.BaseName] = plugin.FileName;
        }
    }
    std::vector<tstring> result(pluginMap.size());
    std::transform(pluginMap.begin(), pluginMap.end(), result.begin(),
        [&](const std::pair<tstring, tstring>& entry) { return PathCombineS(basepath, entry.second); });
    return result;
}

void AMTFilterSource::InitEnv() {
    env_ = nullptr;
    env_ = make_unique_ptr(CreateScriptEnvironment2());

    script_.Clear();
    auto& sb = script_.Get();
    if (setting_.isDumpFilter()) {
        sb.append("SetGraphAnalysis(true)\n");
    }
    // システムのプラグインフォルダを無効化
    if (setting_.isSystemAvsPlugin() == false) {
        sb.append("ClearAutoloadDirs()\n");
    }
    auto moduleDir = GetModuleDirectory();
    // Amatsukaze用オートロードフォルダを追加
    sb.append("AddAutoloadDir(\"%s\")\n", PathCombineS(moduleDir, _T("plugins64")));
    // AutoSelectプラグインをロード
    for (auto& path : GetSuitablePlugins(PathCombineS(PathCombineS(moduleDir, _T("plugins64")), _T("AutoSelected")))) {
        sb.append("LoadPlugin(\"%s\")\n", path);
    }
    // メモリ節約オプションを有効にする
    sb.append("SetCacheMode(CACHE_OPTIMAL_SIZE)\n");
    // これはメモリ不足になると極端に性能が落ちるのでやめる
    //sb.append("SetDeviceOpt(DEV_FREE_THRESHOLD, 1000)\n");
    // Amatsukaze.dllをロード
    sb.append("LoadPlugin(\"%s\")\n", GetModulePath());
}

void AMTFilterSource::ReadAllFrames(int pass) {
    PClip clip = env_->GetVar("last").AsClip();
    const VideoInfo vi = clip->GetVideoInfo();

    ctx.infoF("フィルタパス%d 予定フレーム数: %d", pass + 1, vi.num_frames);
    Stopwatch sw;
    sw.start();
    int prevFrames = 0;

    for (int i = 0; i < vi.num_frames; i++) {
        PVideoFrame frame = clip->GetFrame(i, env_.get());
        double elapsed = sw.current();
        if (elapsed >= 1.0) {
            double fps = (i - prevFrames) / elapsed;
            ctx.progressF("%dフレーム完了 %.2ffps", i + 1, fps);

            prevFrames = i;
            sw.stop();
        }
    }

    ctx.infoF("フィルタパス%d 完了: %.2f秒", pass + 1, sw.getTotal());
}

void AMTFilterSource::defineMakeSource(
    EncodeFileKey key,
    const StreamReformInfo& reformInfo,
    const tstring& logopath) {
    auto& sb = script_.Get();
    sb.append("function MakeSource(bool \"mt\") {\n");
    sb.append("\tmt = default(mt, false)\n");
    sb.append("\tAMTSource(\"%s\")\n", setting_.getTmpAMTSourcePath(key.video));
    sb.append("\tif(mt) { Prefetch(1, 4) }\n");

    int numEraseLogo = 0;
    auto eraseLogo = [&](const tstring& logopath, const tstring& logoFramePath, bool forceEnable) {
        if (forceEnable || File::exists(logoFramePath)) {
            sb.append("\tlogo = \"%s\"\n", logopath);
            sb.append("\tAMTEraseLogo(AMTAnalyzeLogo(logo), logo, \"%s\", maxfade=%d)\n",
                logoFramePath, setting_.getMaxFadeLength());
            ++numEraseLogo;
        }
        };
    if (setting_.isNoDelogo() == false && logopath.size() > 0) {
        eraseLogo(logopath, setting_.getTmpLogoFramePath(key.video), true);
    }
    const auto& eraseLogoPath = setting_.getEraseLogoPath();
    for (int i = 0; i < (int)eraseLogoPath.size(); i++) {
        eraseLogo(eraseLogoPath[i], setting_.getTmpLogoFramePath(key.video, i), false);
    }
    if (numEraseLogo > 0) {
        sb.append("\tif(mt) { Prefetch(1, 4) }\n");
    }

    sb.append("\t");
    trimInput(key, reformInfo);
    sb.append("}\n");
}

void AMTFilterSource::trimInput(EncodeFileKey key,
    const StreamReformInfo& reformInfo) {
    // このencoderIndex+cmtype用の出力フレームリスト作成
    const auto& srcFrames = reformInfo.getFilterSourceAudioFrames(key.video);
    const auto& outFrames = reformInfo.getEncodeFile(key).videoFrames;
    int numSrcFrames = (int)outFrames.size();

    // 不連続点で区切る
    std::vector<EncoderZone> trimZones;
    EncoderZone zone;
    zone.startFrame = outFrames.front();
    for (int i = 1; i < (int)outFrames.size(); i++) {
        if (outFrames[i] != outFrames[i - 1] + 1) {
            zone.endFrame = outFrames[i - 1];
            trimZones.push_back(zone);
            zone.startFrame = outFrames[i];
        }
    }
    zone.endFrame = outFrames.back();
    trimZones.push_back(zone);

    auto& sb = script_.Get();
    if (trimZones.size() > 1 ||
        trimZones[0].startFrame != 0 ||
        trimZones[0].endFrame != (int)srcFrames.size() - 1) {
        // Trimが必要
        for (int i = 0; i < (int)trimZones.size(); i++) {
            if (i > 0) sb.append("++");
            // Trimのlast_frame==0は末尾まですべてという意味なので、0のときは例外処理
            // endFrame >= startFrameなので、endFrame==0のときはstartFrame==0
            if (trimZones[i].endFrame == 0)
                sb.append("Trim(0,-1)");
            else
                sb.append("Trim(%d,%d)", trimZones[i].startFrame, trimZones[i].endFrame);
        }
        sb.append("\n");
    }
}

// 戻り値: 前処理？
bool AMTFilterSource::FilterPass(int pass, int gpuIndex,
    EncodeFileKey key,
    const StreamReformInfo& reformInfo,
    const tstring& logopath) {
    InitEnv();

    auto tmppath = setting_.getAvsTmpPath(key);

    defineMakeSource(key, reformInfo, logopath);

    auto& sb = script_.Get();
    sb.append("AMT_SOURCE = MakeSource(true)\n");
    sb.append("AMT_TMP = \"%s\"\n", pathToOS(tmppath));
    sb.append("AMT_PASS = %d\n", pass);
    sb.append("AMT_DEV = %d\n", gpuIndex);
    sb.append("AMT_SOURCE\n");

    tstring mainpath = setting_.getFilterScriptPath();
    if (mainpath.size()) {
        sb.append("Import(\"%s\")\n", mainpath);
    }

    script_.Apply(env_.get());
    return env_->GetVarDef("AMT_PRE_PROC", false).AsBool();
}

void AMTFilterSource::MakeZones(
    EncodeFileKey key,
    const std::vector<EncoderZone>& zones,
    const StreamReformInfo& reformInfo) {
    const auto& outFrames = reformInfo.getEncodeFile(key).videoFrames;

    // このencoderIndex用のゾーンを作成
    outZones_.clear();
    for (int i = 0; i < (int)zones.size(); i++) {
        EncoderZone newZone = {
          (int)(std::lower_bound(outFrames.begin(), outFrames.end(), zones[i].startFrame) - outFrames.begin()),
          (int)(std::lower_bound(outFrames.begin(), outFrames.end(), zones[i].endFrame) - outFrames.begin())
        };
        // 短すぎる場合はゾーンを捨てる
        if (newZone.endFrame - newZone.startFrame > 30) {
            outZones_.push_back(newZone);
        }
    }

    int numSrcFrames = (int)outFrames.size();

    VideoInfo outvi = filter_->GetVideoInfo();
    int numOutFrames = outvi.num_frames;

    const VideoFormat& infmt = reformInfo.getFormat(key).videoFormat;
    double srcDuration = (double)numSrcFrames * infmt.frameRateDenom / infmt.frameRateNum;
    double clipDuration = timeCodes_.size()
        ? timeCodes_.back() / 1000.0
        : (double)numOutFrames * outvi.fps_denominator / outvi.fps_numerator;
    bool outParity = filter_->GetParity(0);

    ctx.infoF("フィルタ入力: %dフレーム %d/%dfps (%s)",
        numSrcFrames, infmt.frameRateNum, infmt.frameRateDenom,
        infmt.progressive ? "プログレッシブ" : "インターレース");

    if (timeCodes_.size()) {
        ctx.infoF("フィルタ出力: %dフレーム VFR (ベース %d/%d fps)",
            numOutFrames, outvi.fps_numerator, outvi.fps_denominator);
    } else {
        ctx.infoF("フィルタ出力: %dフレーム %d/%dfps (%s)",
            numOutFrames, outvi.fps_numerator, outvi.fps_denominator,
            outParity ? "インターレース" : "プログレッシブ");
    }

    if (std::abs(srcDuration - clipDuration) > 0.1f) {
        THROWF(RuntimeException, "フィルタ出力映像の時間が入力と一致しません（入力: %.3f秒 出力: %.3f秒）", srcDuration, clipDuration);
    }

    if (numSrcFrames != numOutFrames && outParity) {
        ctx.warn("フレーム数が変わっていますがインターレースのままです。プログレッシブ出力が目的ならAssumeBFF()をavsファイルの最後に追加してください。");
    }

    if (timeCodes_.size()) {
        // VFRタイムスタンプをoutZonesに反映させる
        double tick = (double)infmt.frameRateDenom / infmt.frameRateNum;
        for (int i = 0; i < (int)outZones_.size(); i++) {
            outZones_[i].startFrame = (int)(std::lower_bound(timeCodes_.begin(), timeCodes_.end(), outZones_[i].startFrame * tick * 1000) - timeCodes_.begin());
            outZones_[i].endFrame = (int)(std::lower_bound(timeCodes_.begin(), timeCodes_.end(), outZones_[i].endFrame * tick * 1000) - timeCodes_.begin());
        }
    } else if (numSrcFrames != numOutFrames) {
        // フレーム数が変わっている場合はゾーンを引き伸ばす
        double scale = (double)numOutFrames / numSrcFrames;
        for (int i = 0; i < (int)outZones_.size(); i++) {
            outZones_[i].startFrame = std::max(0, std::min(numOutFrames, (int)std::round(outZones_[i].startFrame * scale)));
            outZones_[i].endFrame = std::max(0, std::min(numOutFrames, (int)std::round(outZones_[i].endFrame * scale)));
        }
    }
}

void AMTFilterSource::MakeOutFormat(const VideoFormat& infmt) {
    auto vi = filter_->GetVideoInfo();
    // vi_からエンコーダ入力用VideoFormatを生成する
    outfmt_ = infmt;
    if (outfmt_.width != vi.width || outfmt_.height != vi.height) {
        // リサイズされた
        outfmt_.width = vi.width;
        outfmt_.height = vi.height;
        // リサイズされた場合はアスペクト比を1:1にする
        outfmt_.sarHeight = outfmt_.sarWidth = 1;
    }
    outfmt_.frameRateDenom = vi.fps_denominator;
    outfmt_.frameRateNum = vi.fps_numerator;
    // インターレースかどうかは取得できないのでパリティがfalse(BFF?)だったらプログレッシブと仮定
    outfmt_.progressive = (filter_->GetParity(0) == false);
}
AMTDecimate::AMTDecimate(PClip source, const std::string& duration, IScriptEnvironment* env)
    : GenericVideoFilter(source) {
    File file(char_to_tstring(duration), _T("r"));
    std::string str;
    while (file.getline(str)) {
        durations.push_back(std::atoi(str.c_str()));
    }
    int numSourceFrames = std::accumulate(durations.begin(), durations.end(), 0);
    if (vi.num_frames != numSourceFrames) {
        env->ThrowError("[AMTDecimate] # of frames does not match. %d(%s) vs %d(source clip)",
            (int)numSourceFrames, duration.c_str(), vi.num_frames);
    }
    vi.num_frames = (int)durations.size();
    framesMap.resize(durations.size());
    framesMap[0] = 0;
    for (int i = 0; i < (int)durations.size() - 1; i++) {
        framesMap[i + 1] = framesMap[i] + durations[i];
    }
}

PVideoFrame __stdcall AMTDecimate::GetFrame(int n, IScriptEnvironment* env) {
    return child->GetFrame(framesMap[std::max(0, std::min(n, vi.num_frames - 1))], env);
}

/* static */ AVSValue __cdecl AMTDecimate::Create(AVSValue args, void* user_data, IScriptEnvironment* env) {
    return new AMTDecimate(
        args[0].AsClip(),       // source
        args[1].AsString(),       // analyzeclip
        env
    );
}

// VFRでだいたいのレートコントロールを実現する
// VFRタイミングとCMゾーンからゾーンとビットレートを作成
std::vector<BitrateZone> MakeVFRBitrateZones(const std::vector<double>& timeCodes,
    const std::vector<EncoderZone>& cmzones, double bitrateCM,
    int fpsNum, int fpsDenom, double timeFactor, double costLimit) {
    enum {
        UNIT_FRAMES = 8,
        HARD_ZONE_LIMIT = 1000, // ゾーン数上限は1000
        TARGET_ZONES_PER_HOUR = 30 // 目標ゾーン数は1時間あたり30個
    };
    struct Block {
        int index;   // ブロック先頭のUNITアドレス
        int next;    // 後ろのブロックの先頭ブロックアドレス（このブロックが存在しない場合は-1）
        double avg;  // このブロックの平均ビットレート
        double cost; // 後ろのブロックと結合したときの追加コスト
    };

    if (timeCodes.size() == 0) {
        return std::vector<BitrateZone>();
    }
    int numFrames = (int)timeCodes.size() - 1;
    // 8フレームごとの平均ビットレートを計算
    std::vector<double> units(nblocks(numFrames, UNIT_FRAMES));
    for (int i = 0; i < (int)units.size(); i++) {
        auto start = timeCodes.begin() + i * UNIT_FRAMES;
        auto end = ((i + 1) * UNIT_FRAMES < (int)timeCodes.size()) ? start + UNIT_FRAMES : timeCodes.end() - 1;
        double sum = (*end - *start) / 1000.0 * fpsNum / fpsDenom;
        double invfps = sum / (int)(end - start);
        units[i] = (invfps - 1.0) * timeFactor + 1.0;
    }
    // cmzonesを適用
    for (int i = 0; i < (int)cmzones.size(); i++) {
        // 半端部分はCMゾーンを小さくる方向に丸める
        int start = nblocks(cmzones[i].startFrame, UNIT_FRAMES);
        int end = cmzones[i].endFrame / UNIT_FRAMES;
        for (int k = start; k < end; k++) {
            units[k] *= bitrateCM;
        }
    }
    // ここでのunitsは各フレームに適用すべきビットレート
    // だが、そのままzonesにすると数が多すぎて
    // コマンドライン引数にできないのである程度まとめる
    std::vector<Block> blocks;
    double cur = units[0];
    blocks.push_back(Block{ 0, 1, cur, 0 });
    // 同じビットレートの連続はまとめる
    for (int i = 1; i < (int)units.size(); i++) {
        if (units[i] != cur) {
            cur = units[i];
            blocks.push_back(Block{ i, (int)blocks.size() + 1, cur, 0 });
        }
    }
    // 最後に番兵を置く
    blocks.push_back(Block{ (int)units.size(), -1, 0, 0 });

    auto sumDiff = [&](int start, int end, double avg) {
        double diff = 0;
        for (int i = start; i < end; i++) {
            diff += std::abs(units[i] - avg);
        }
        return diff;
        };

    auto calcCost = [&](Block& cur, const Block&  next) {
        int start = cur.index;
        int mid = next.index;
        int end = blocks[next.next].index;
        // 現在のコスト

        double cur_cost = sumDiff(start, mid, cur.avg);
        double next_cost = sumDiff(mid, end, next.avg);
        // 連結後の平均ビットレート
        double avg2 = (cur.avg * (mid - start) + next.avg * (end - mid)) / (end - start);
        // 連結後のコスト
        double cost2 = sumDiff(start, end, avg2);
        // 追加コスト
        cur.cost = cost2 - (cur_cost + next_cost);
        };

    // 連結時追加コスト計算
    for (int i = 0; blocks[i].index < (int)units.size(); i = blocks[i].next) {
        auto& cur = blocks[i];
        auto& next = blocks[cur.next];
        // 次のブロックが存在すれば
        if (next.index < (int)units.size()) {
            calcCost(cur, next);
        }
    }

    // 最大ブロック数
    auto totalHours = timeCodes.back() / 1000.0 / 3600.0;
    int targetNumZones = std::max(1, (int)(TARGET_ZONES_PER_HOUR * totalHours));
    double totalCostLimit = units.size() * costLimit;

    // ヒープ作成
    auto comp = [&](int b0, int b1) {
        return blocks[b0].cost > blocks[b1].cost;
        };
    // 最後のブロックと番兵は連結できないので除く
    int heapSize = (int)blocks.size() - 2;
    int numZones = heapSize;
    std::vector<int> indices(heapSize);
    for (int i = 0; i < heapSize; i++) indices[i] = i;
    std::make_heap(indices.begin(), indices.begin() + heapSize, comp);
    double totalCost = 0;
    while ((totalCost < totalCostLimit && numZones > targetNumZones) ||
        numZones > HARD_ZONE_LIMIT) {
        // 追加コスト最小ブロック
        int idx = indices.front();
        std::pop_heap(indices.begin(), indices.begin() + (heapSize--), comp);
        auto& cur = blocks[idx];
        // このブロックが既に連結済みでなければ
        if (cur.next != -1) {
            auto& next = blocks[cur.next];
            int start = cur.index;
            int mid = next.index;
            int end = blocks[next.next].index;
            totalCost += cur.cost;
            // 連結後の平均ビットレートに更新
            cur.avg = (cur.avg * (mid - start) + next.avg * (end - mid)) / (end - start);
            // 連結後のnextに更新
            cur.next = next.next;
            // 連結されるブロックは無効化
            next.next = -1;
            --numZones;
            // 更に次のブロックがあれば
            auto& nextnext = blocks[cur.next];
            if (nextnext.index < (int)units.size()) {
                // 連結時の追加コストを計算
                calcCost(cur, nextnext);
                // 再度ヒープに追加
                indices[heapSize] = idx;
                std::push_heap(indices.begin(), indices.begin() + (++heapSize), comp);
            }
        }
    }

    // 結果を生成
    std::vector<BitrateZone> zones;
    for (int i = 0; blocks[i].index < (int)units.size(); i = blocks[i].next) {
        const auto& cur = blocks[i];
        BitrateZone zone = BitrateZone();
        zone.startFrame = cur.index * UNIT_FRAMES;
        zone.endFrame = std::min(numFrames, blocks[cur.next].index * UNIT_FRAMES);
        zone.bitrate = cur.avg;
        zones.push_back(zone);
    }

    return zones;
}

// VFRに対応していないエンコーダでビットレート指定を行うとき用の
// 平均フレームレートを考慮したビットレートを計算する
double AdjustVFRBitrate(const std::vector<double>& timeCodes, int fpsNum, int fpsDenom) {
    if (timeCodes.size() == 0) {
        return 1.0;
    }
    return (timeCodes.back() / 1000.0) / (timeCodes.size() - 1) * fpsNum / fpsDenom;
}

AVSValue __cdecl AMTExec(AVSValue args, void* user_data, IScriptEnvironment* env) {
    auto cmd = StringFormat(_T("%s"), args[1].AsString());
    PRINTF("%s", StringFormat("AMTExec: %s\n", cmd).c_str());
    StdRedirectedSubProcess proc(cmd);
    proc.join();
    return args[0];
}
AMTOrderedParallel::AMTOrderedParallel(AVSValue clips, IScriptEnvironment* env)
    : GenericVideoFilter(clips[0].AsClip())
    , clips_(clips.ArraySize()) {
    int maxFrames = 0;
    for (int i = 0; i < clips.ArraySize(); i++) {
        clips_[i].clip = clips[i].AsClip();
        clips_[i].numFrames = clips_[i].clip->GetVideoInfo().num_frames;
        maxFrames = std::max(maxFrames, clips_[i].numFrames);
    }
    vi.num_frames = maxFrames * clips.ArraySize();
}

PVideoFrame __stdcall AMTOrderedParallel::GetFrame(int n, IScriptEnvironment* env) {
    int nclips = (int)clips_.size();
    int clipidx = n % nclips;
    auto& data = clips_[clipidx];
    int frameidx = std::min(data.numFrames - 1, n / nclips);
    std::unique_lock<std::mutex> lock(data.mutex);
    for (; data.current <= frameidx; data.current++) {
        data.clip->GetFrame(data.current, env);
    }
    return env->NewVideoFrame(vi);
}

int __stdcall AMTOrderedParallel::SetCacheHints(int cachehints, int frame_range) {
    if (cachehints == CACHE_GET_MTMODE) {
        return MT_NICE_FILTER;
    }
    return 0;
}

/* static */ AVSValue __cdecl AMTOrderedParallel::Create(AVSValue args, void* user_data, IScriptEnvironment* env) {
    return new AMTOrderedParallel(
        args[0],       // clips
        env
    );
}
