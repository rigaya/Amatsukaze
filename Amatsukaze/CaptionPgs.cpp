#include "CaptionPgs.h"

#include <aribcaption/aribcaption.hpp>
#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#if defined(ARIBCC_USE_FONTCONFIG) && !defined(_WIN32)
#include <fontconfig/fontconfig.h>
#endif

namespace {

void WriteDiagnostic(char* destination, size_t capacity, const char* text) noexcept {
    if (destination && capacity) {
        const size_t length = std::min(capacity - 1, std::strlen(text));
        std::memcpy(destination, text, length);
        destination[length] = '\0';
    }
}

#if defined(ARIBCC_USE_FONTCONFIG) && !defined(_WIN32)
std::string ResolveJapaneseFont(const char* family) {
    std::unique_ptr<FcConfig, decltype(&FcConfigDestroy)> config(
        FcInitLoadConfigAndFonts(), &FcConfigDestroy);
    std::unique_ptr<FcPattern, decltype(&FcPatternDestroy)> pattern(
        FcPatternCreate(), &FcPatternDestroy);
    if (!config || !pattern ||
        !FcPatternAddString(pattern.get(), FC_FAMILY, reinterpret_cast<const FcChar8*>(family)) ||
        !FcPatternAddString(pattern.get(), FC_LANG, reinterpret_cast<const FcChar8*>("ja")) ||
        !FcPatternAddBool(pattern.get(), FC_OUTLINE, FcTrue) ||
        !FcConfigSubstitute(config.get(), pattern.get(), FcMatchPattern)) {
        throw std::runtime_error("fontconfigの初期化に失敗した");
    }
    FcDefaultSubstitute(pattern.get());
    FcResult result = FcResultNoMatch;
    std::unique_ptr<FcPattern, decltype(&FcPatternDestroy)> matched(
        FcFontMatch(config.get(), pattern.get(), &result), &FcPatternDestroy);
    if (!matched || result != FcResultMatch) {
        throw std::runtime_error("日本語フォントが見つからない");
    }
    // fontconfigの代替フォントを指定フォントの存在確認として扱わない。
    bool sameFamily = false;
    FcChar8* value = nullptr;
    for (int index = 0; FcPatternGetString(matched.get(), FC_FAMILY, index, &value) == FcResultMatch; ++index) {
        if (FcStrCmpIgnoreCase(value, reinterpret_cast<const FcChar8*>(family)) == 0) {
            sameFamily = true;
            break;
        }
    }
    FcCharSet* charset = nullptr;
    if (!sameFamily || FcPatternGetCharSet(matched.get(), FC_CHARSET, 0, &charset) != FcResultMatch ||
        !FcCharSetHasChar(charset, 0x65e5)) {
        throw std::runtime_error("指定フォントが存在しないか、日本語グリフを含まない");
    }
    FcChar8* filename = nullptr;
    if (FcPatternGetString(matched.get(), FC_FILE, 0, &filename) != FcResultMatch) {
        throw std::runtime_error("日本語フォントのファイルを解決できない");
    }
    return reinterpret_cast<const char*>(filename);
}
#endif

void CheckRenderer(int width, int height, const char* fontFamily, std::string& diagnostic) {
    using namespace aribcaption;
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
        throw std::invalid_argument("キャンバス寸法は1から4096まで必要");
    }
    if (fontFamily && !*fontFamily) {
        throw std::invalid_argument("フォント名が空文字列");
    }
    std::string fontPath;
#if defined(ARIBCC_USE_FONTCONFIG) && !defined(_WIN32)
    if (!fontFamily) {
        fontFamily = "Noto Sans CJK JP";
    }
    fontPath = ResolveJapaneseFont(fontFamily);
#endif
    std::string rendererErrors;
    Context context;
    context.SetLogcatCallback([&rendererErrors](LogLevel level, const char* message) {
        if (level == LogLevel::kError || level == LogLevel::kWarning) {
            rendererErrors += message;
            rendererErrors += '\n';
        }
    });
    Renderer renderer(context);
#if defined(ARIBCC_USE_FONTCONFIG) && !defined(_WIN32)
    const bool initialized = renderer.Initialize(CaptionType::kCaption,
        FontProviderType::kFontconfig, TextRendererType::kFreetype);
#else
    const bool initialized = renderer.Initialize();
#endif
    if (!initialized || !renderer.SetFrameSize(width, height)) {
        throw std::runtime_error("字幕レンダラーの初期化に失敗した: " + rendererErrors);
    }
    if (fontFamily && !renderer.SetDefaultFontFamily({fontFamily}, true)) {
        throw std::runtime_error("字幕フォントの設定に失敗した");
    }
    renderer.SetForceNoBackground(true);
    renderer.SetStrokeWidth(0.0f);

    Caption caption;
    caption.iso6392_language_code = ThreeCC("jpn");
    caption.text = u8"日";
    caption.pts = 0;
    caption.wait_duration = 1000;
    caption.plane_width = 960;
    caption.plane_height = 540;
    CaptionRegion region;
    region.x = 100;
    region.y = 100;
    region.width = 48;
    region.height = 48;
    CaptionChar character;
    character.codepoint = 0x65e5;
    character.x = region.x;
    character.y = region.y;
    character.char_width = 48;
    character.char_height = 48;
    character.char_horizontal_scale = 1.0f;
    character.char_vertical_scale = 1.0f;
    character.text_color = ColorRGBA(255, 255, 255, 255);
    character.back_color = ColorRGBA(0, 0, 0, 0);
    character.stroke_color = ColorRGBA(0, 0, 0, 0);
    std::memcpy(character.u8str, u8"日", sizeof(u8"日"));
    region.chars.push_back(character);
    caption.regions.push_back(std::move(region));
    if (!renderer.AppendCaption(std::move(caption))) {
        throw std::runtime_error("確認用字幕の登録に失敗した");
    }
    RenderResult result;
    const auto status = renderer.Render(0, result);
    // Windowsの既定フォント列では、候補が欠落しても次の日本語フォントへ進める。
    // Linuxは存在を確認した単一フォントを使うため、描画中の警告も失敗とする。
    bool failed = status != RenderStatus::kGotImage;
#if defined(ARIBCC_USE_FONTCONFIG) && !defined(_WIN32)
    failed = failed || !rendererErrors.empty();
#endif
    if (failed) {
        throw std::runtime_error("日本語字幕の描画に失敗した: " + rendererErrors);
    }
    size_t paintedPixels = 0;
    for (const auto& image : result.images) {
        for (int y = 0; y < image.height; ++y) {
            for (int x = 0; x < image.width; ++x) {
                if (image.bitmap[static_cast<size_t>(y) * image.stride + x * 4 + 3]) {
                    ++paintedPixels;
                }
            }
        }
    }
    if (!paintedPixels) {
        throw std::runtime_error("日本語字幕の画像に非透明画素がない");
    }
    diagnostic = "日本語グリフ描画成功: 非透明画素=" + std::to_string(paintedPixels);
    if (!fontPath.empty()) {
        diagnostic += "、フォント=" + std::string(fontFamily) + "、ファイル=" + fontPath;
    }
}

}

extern "C" AMATSUKAZE_API int CaptionPgsCheckRenderer(
    int width, int height, const char* fontFamily, char* diagnostic, size_t diagnosticSize) noexcept {
    try {
        std::string message;
        CheckRenderer(width, height, fontFamily, message);
        WriteDiagnostic(diagnostic, diagnosticSize, message.c_str());
        return 1;
    } catch (const std::exception& error) {
        WriteDiagnostic(diagnostic, diagnosticSize, error.what());
    } catch (...) {
        WriteDiagnostic(diagnostic, diagnosticSize, "字幕レンダラーで不明な例外が発生した");
    }
    return 0;
}
