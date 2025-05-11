/**
* Amtasukaze Compile Target
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/
#define _USE_MATH_DEFINES
#include "AmatsukazeCLI.hpp"
#include "LogoGUISupport.hpp"

// Avisynthフィルタデバッグ用
#include "TextOut.cpp"

#if defined(_WIN32) || defined(_WIN64)
HMODULE g_DllHandle;
#else
#include <dlfcn.h>
void* g_DllHandle = nullptr;
#endif

bool g_av_initialized = false;

extern "C" AMATSUKAZE_API int AmatsukazeCLI(int argc, const tchar* argv[]) {
    return RunAmatsukazeCLI(argc, argv);
}

extern "C" AMATSUKAZE_API void InitAmatsukazeDLL() {
    // FFMPEGライブラリ初期化
#if LIBAVFORMAT_VERSION_MAJOR < 59
    av_register_all();
#endif
#if ENABLE_FFMPEG_FILTER
    //avfilter_register_all();
#endif
}

// CM解析用（＋デバッグ用）インターフェース
#if defined(_WIN32) || defined(_WIN64)
extern "C" AMATSUKAZE_API const char* __stdcall AvisynthPluginInit3(IScriptEnvironment* env, const AVS_Linkage* const vectors) {
#else
extern "C" AMATSUKAZE_API const char* AvisynthPluginInit3(IScriptEnvironment* env, const AVS_Linkage* const vectors) {
#endif
    // 直接リンクしているのでvectorsを格納する必要はない

    if (g_av_initialized == false) {
        // FFMPEGライブラリ初期化
#if LIBAVFORMAT_VERSION_MAJOR < 59
        av_register_all();
#endif
#if ENABLE_FFMPEG_FILTER
        //avfilter_register_all();
#endif
        g_av_initialized = true;
    }

    env->AddFunction("AMTSource", "s[filter]s[outqp]b[threads]i", av::CreateAMTSource, 0);

    env->AddFunction("AMTAnalyzeLogo", "cs[maskratio]i", logo::AMTAnalyzeLogo::Create, 0);
    env->AddFunction("AMTEraseLogo", "ccs[logof]s[mode]i[maxfade]i", logo::AMTEraseLogo::Create, 0);

    env->AddFunction("AMTDecimate", "c[duration]s", AMTDecimate::Create, 0);

    env->AddFunction("AMTExec", "cs", AMTExec, 0);
    env->AddFunction("AMTOrderedParallel", "c+", AMTOrderedParallel::Create, 0);

    return "Amatsukaze plugin";
}


#if defined(_WIN32) || defined(_WIN64)
BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved) {
    if (dwReason == DLL_PROCESS_ATTACH) g_DllHandle = hModule;
    return TRUE;
}
#else
// Linux用の共有ライブラリ初期化関数
__attribute__((constructor))
static void on_load(void) {
    g_DllHandle = (void *)&AmatsukazeCLI;
}

__attribute__((destructor))
static void on_unload(void) {
    g_DllHandle = nullptr;
}
#endif