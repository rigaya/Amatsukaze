#pragma once
#include "FilterPreview.h"
#include "AviSynthWrapper.h"
#include <mutex>

// avsを評価し、フレームとプロパティを取得するための環境を保持する。
class FilterPreview : public AMTObject {
    IScriptEnvironment2* env = nullptr;
    PClip clip;
    PVideoFrame frame;
    int numFrames = 0;
    std::mutex mutex;

    bool getIntProp(const PVideoFrame& f, const char* key, int& value) {
#if AVISYNTH_MODE == AVISYNTH_NEO
        const auto prop = f->GetProperty(key);
        if (!prop) return false;
        value = static_cast<int>(prop->GetInt());
        return true;
#else
        int error = 0;
        const auto result = env->propGetInt(env->getFramePropsRO(f), key, 0, &error);
        if (error) return false;
        value = static_cast<int>(result);
        return true;
#endif
    }
    void clear() {
        frame = PVideoFrame();
        clip = PClip();
        if (env) { env->DeleteScriptEnvironment(); env = nullptr; }
    }
    PVideoFrame readFrame(int n) {
        if (n < 0 || n >= numFrames) THROW(RuntimeException, "フレーム番号が範囲外です");
        return clip->GetFrame(n, env);
    }
public:
    FilterPreview(AMTContext& context, const tchar* path, FilterPreviewInfo& out) : AMTObject(context) {
        try {
            env = CreateScriptEnvironment2();
            if (!env) THROW(RuntimeException, "AviSynth環境の作成に失敗しました");
            const auto scriptPath = tchar_to_string(path);
            AVSValue result = env->Invoke("Import", AVSValue(scriptPath.c_str()));
            if (!result.IsClip()) THROW(RuntimeException, "スクリプトの戻り値がクリップではありません");
            clip = result.AsClip();
            const VideoInfo vi = clip->GetVideoInfo();
            const int pixelType = vi.IsYUV() && vi.IsPlanar() && vi.NumComponents() == 3 ? (vi.Is420() ? FILTER_PREVIEW_YUV420 : vi.Is422() ? FILTER_PREVIEW_YUV422 : vi.Is444() ? FILTER_PREVIEW_YUV444 : 0) : 0;
            if (!pixelType || vi.BitsPerComponent() > 16 || vi.num_frames <= 0)
                THROW(RuntimeException, "対応していない映像形式です");
            numFrames = vi.num_frames;
            out = { vi.num_frames, static_cast<int>(vi.fps_numerator), static_cast<int>(vi.fps_denominator), vi.width, vi.height, 0, 0, vi.BitsPerComponent(), pixelType };
            // SARはクリップ共通情報ではなくフレームプロパティなので先頭を読む。
            frame = readFrame(0);
            int sarW = 0, sarH = 0;
            if (getIntProp(frame, "_SARNum", sarW) && getIntProp(frame, "_SARDen", sarH) && sarW > 0 && sarH > 0) {
                out.sarW = sarW; out.sarH = sarH;
            }
        } catch (const AvisynthError& e) {
            const std::string message = e.msg ? e.msg : "AviSynthエラー";
            clear();
            throw RuntimeException(message);
        } catch (...) { clear(); throw; }
    }
    ~FilterPreview() { clear(); }
    int getFrame(int n, const uint8_t** planes, int* pitches) {
        std::lock_guard<std::mutex> guard(mutex);
        ctx.setError(Exception());
        if (!planes || !pitches) { ctx.setError(RuntimeException("出力先が指定されていません")); return 0; }
        for (int i = 0; i < 3; i++) { planes[i] = nullptr; pitches[i] = 0; }
        try {
            frame = readFrame(n);
            if (frame->GetDevice().GetType() == DEV_TYPE_CUDA)
                THROW(RuntimeException, "CUDAフレームはOnCPUでCPUへ転送してください");
            const int ids[] = { PLANAR_Y, PLANAR_U, PLANAR_V };
            for (int i = 0; i < 3; i++) { planes[i] = frame->GetReadPtr(ids[i]); pitches[i] = frame->GetPitch(ids[i]); }
            return 1;
        } catch (const AvisynthError& e) { ctx.setError(RuntimeException(e.msg)); }
          catch (const Exception& e) { ctx.setError(e); }
          catch (...) { ctx.setError(RuntimeException("フレーム取得で不明な例外が発生しました")); }
        return 0;
    }
    int getProps(int n, int* type, int* duration) {
        std::lock_guard<std::mutex> guard(mutex);
        ctx.setError(Exception());
        if (!type || !duration) { ctx.setError(RuntimeException("出力先が指定されていません")); return -1; }
        *type = *duration = 0;
        try {
            // KFMの属性だけを参照し、取得したフレームの画素は使用しない。
            frame = readFrame(n);
            int t = 0, d = 0;
            if (!getIntProp(frame, "KFM_FrameType", t) || !getIntProp(frame, "KFM_Duration", d)) return 0;
            *type = t; *duration = d; return 1;
        } catch (const AvisynthError& e) { ctx.setError(RuntimeException(e.msg)); }
          catch (const Exception& e) { ctx.setError(e); }
          catch (...) { ctx.setError(RuntimeException("プロパティ取得で不明な例外が発生しました")); }
        return -1;
    }
};
extern "C" AMATSUKAZE_API void* FilterPreview_Open(AMTContext* ctx, const tchar* path, FilterPreviewInfo* info) {
    if (!ctx) return nullptr;
    ctx->setError(Exception());
    if (!path || !info) { ctx->setError(RuntimeException("入力または出力先が指定されていません")); return nullptr; }
    *info = {};
    try { return new FilterPreview(*ctx, path, *info); }
    catch (const AvisynthError& e) { ctx->setError(RuntimeException(e.msg)); }
    catch (const Exception& e) { ctx->setError(e); }
    catch (...) { ctx->setError(RuntimeException("プレビューを開く際に不明な例外が発生しました")); }
    return nullptr;
}
extern "C" AMATSUKAZE_API int FilterPreview_GetFrame(void* p, int n, const uint8_t** planes, int* pitches) {
    return p ? static_cast<FilterPreview*>(p)->getFrame(n, planes, pitches) : 0;
}
extern "C" AMATSUKAZE_API int FilterPreview_GetKFMProps(void* p, int n, int* type, int* duration) {
    return p ? static_cast<FilterPreview*>(p)->getProps(n, type, duration) : -1;
}
extern "C" AMATSUKAZE_API void FilterPreview_Close(void* p) { delete static_cast<FilterPreview*>(p); }
