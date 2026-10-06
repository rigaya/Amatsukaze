#include "CaptionPgs.h"
#include "PgsEncoder.h"
#include "StreamReform.h"

#include <aribcaption/aribcaption.hpp>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <limits>
#include <map>
#include <set>
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

namespace {

struct CaptionFontResolution {
    std::vector<std::string> families;
    // CP単位の探索結果を字幕全体で再利用する。空文字列は未解決。
    std::map<uint32_t, std::string> codepointFamilies;
    size_t initialFamilyCount = 0;
    uint32_t blankCodepoint = 0x3000;
};

CaptionFontResolution ResolveCaptionFonts(std::set<uint32_t> codepoints,
    const std::string& preferredFamily) {
    CaptionFontResolution resolution;
    auto& families = resolution.families;
    if (!preferredFamily.empty()) {
        families.push_back(preferredFamily);
    } else {
        // libaribcaption1.1.2の日本語既定候補を優先し、存在を必須条件にはしない。
        families = {"Noto Sans CJK JP", "Noto Sans CJK", "Source Han Sans JP", "sans-serif"};
    }
    resolution.initialFamilyCount = families.size();
#if defined(ARIBCC_USE_FONTCONFIG) && !defined(_WIN32)
    // 空白代替と日本語フォントの有無も同じCPキャッシュで一度だけ解決する。
    codepoints.insert(0x3000);
    codepoints.insert(0x20);
    codepoints.insert(0x65e5);
    std::unique_ptr<FcConfig, decltype(&FcConfigDestroy)> config(FcInitLoadConfigAndFonts(), &FcConfigDestroy);
    if (!config) {
        throw std::runtime_error("字幕の補助フォント探索を初期化できない");
    }
    for (uint32_t codepoint : codepoints) {
        if (!codepoint) {
            continue;
        }
        std::unique_ptr<FcPattern, decltype(&FcPatternDestroy)> pattern(FcPatternCreate(), &FcPatternDestroy);
        std::unique_ptr<FcCharSet, decltype(&FcCharSetDestroy)> charset(FcCharSetCreate(), &FcCharSetDestroy);
        if (!pattern || !charset || !FcCharSetAddChar(charset.get(), codepoint) ||
            !FcPatternAddString(pattern.get(), FC_FAMILY, reinterpret_cast<const FcChar8*>(families[0].c_str())) ||
            !FcPatternAddString(pattern.get(), FC_LANG, reinterpret_cast<const FcChar8*>("ja")) ||
            !FcPatternAddCharSet(pattern.get(), FC_CHARSET, charset.get()) ||
            !FcConfigSubstitute(config.get(), pattern.get(), FcMatchPattern)) {
            throw std::runtime_error("字幕の補助フォント探索条件を設定できない");
        }
        FcDefaultSubstitute(pattern.get());
        FcResult result = FcResultNoMatch;
        std::unique_ptr<FcPattern, decltype(&FcPatternDestroy)> match(
            FcFontMatch(config.get(), pattern.get(), &result), &FcPatternDestroy);
        FcCharSet* matchedCharset = nullptr;
        FcChar8* family = nullptr;
        if (!match || result != FcResultMatch ||
            FcPatternGetCharSet(match.get(), FC_CHARSET, 0, &matchedCharset) != FcResultMatch ||
            !FcCharSetHasChar(matchedCharset, codepoint) ||
            FcPatternGetString(match.get(), FC_FAMILY, 0, &family) != FcResultMatch) {
            resolution.codepointFamilies.emplace(codepoint, std::string());
            continue;
        }
        const std::string resolved = reinterpret_cast<const char*>(family);
        resolution.codepointFamilies.emplace(codepoint, resolved);
        if (std::find(families.begin(), families.end(), resolved) == families.end()) {
            families.push_back(resolved);
        }
    }
    if (resolution.codepointFamilies.at(0x65e5).empty()) {
        throw std::runtime_error("日本語グリフを描画できるフォントがない");
    }
    if (resolution.codepointFamilies.at(0x3000).empty()) {
        resolution.blankCodepoint = resolution.codepointFamilies.at(0x20).empty() ? 0 : 0x20;
    }
#else
    (void)codepoints;
#endif
    return resolution;
}

void CollectCaptionCodepoints(const aribcaption::Caption& caption, std::set<uint32_t>& codepoints) {
    for (const auto& region : caption.regions) {
        for (const auto& character : region.chars) {
            // DRCSビットマップだけの文字はフォント探索不要。
            if (character.type != aribcaption::CaptionCharType::kDRCS) {
                codepoints.insert(character.codepoint);
                codepoints.insert(character.pua_codepoint);
            }
        }
    }
}

std::string CodepointText(uint32_t codepoint) {
    char text[16];
    std::snprintf(text, sizeof(text), "U+%04X", static_cast<unsigned>(codepoint));
    return text;
}

void ReplaceMissingGlyphs(aribcaption::Caption& caption, const CaptionFontResolution& fonts,
    double sourcePTS, const CaptionPgsDiagnostic& diagnostic) {
#if defined(ARIBCC_USE_FONTCONFIG) && !defined(_WIN32)
    const auto missing = [&fonts](uint32_t codepoint) {
        const auto found = fonts.codepointFamilies.find(codepoint);
        return codepoint && found != fonts.codepointFamilies.end() && found->second.empty();
    };
    for (auto& region : caption.regions) {
        for (auto& character : region.chars) {
            if (character.type == aribcaption::CaptionCharType::kDRCS || !missing(character.codepoint)) {
                continue;
            }
            const auto original = character.codepoint;
            std::string replacement;
            if (character.pua_codepoint && !missing(character.pua_codepoint)) {
                character.codepoint = character.pua_codepoint;
                character.pua_codepoint = 0;
                replacement = "私用領域グリフを使用";
            } else if (character.type == aribcaption::CaptionCharType::kDRCSReplaced &&
                caption.drcs_map.count(character.drcs_code)) {
                character.type = aribcaption::CaptionCharType::kDRCS;
                character.codepoint = 0;
                character.pua_codepoint = 0;
                replacement = "元のDRCSビットマップを使用";
            } else {
                // 字形だけを空白へ置換し、文字区画・背景・下線・囲み線は保持する。
                character.pua_codepoint = 0;
                std::memset(character.u8str, 0, sizeof(character.u8str));
                if (fonts.blankCodepoint) {
                    character.type = aribcaption::CaptionCharType::kText;
                    character.codepoint = fonts.blankCodepoint;
                    if (fonts.blankCodepoint == 0x3000) {
                        std::memcpy(character.u8str, u8"　", sizeof(u8"　"));
                    } else {
                        character.u8str[0] = ' ';
                    }
                } else {
                    // 空白グリフもなければ透明なDRCSで字形だけを省略する。
                    uint32_t code = std::numeric_limits<uint32_t>::max();
                    while (caption.drcs_map.count(code)) --code;
                    aribcaption::DRCS blank;
                    blank.width = blank.height = blank.depth_bits = 1;
                    blank.depth = 2;
                    blank.pixels = {0};
                    caption.drcs_map.emplace(code, std::move(blank));
                    character.type = aribcaption::CaptionCharType::kDRCS;
                    character.codepoint = 0;
                    character.drcs_code = code;
                }
                replacement = "空白へ置換";
            }
            if (diagnostic) {
                diagnostic(true, "字幕グリフ未解決: " + CodepointText(original) +
                    "、元PTS90k=" + std::to_string(sourcePTS) + "、" + replacement);
            }
        }
    }
#else
    (void)caption;
    (void)fonts;
    (void)sourcePTS;
    (void)diagnostic;
#endif
}

std::string DescribeCaption(const aribcaption::Caption& caption, double sourcePTS) {
    std::set<uint32_t> codepoints;
    CollectCaptionCodepoints(caption, codepoints);
    std::string description = "元PTS90k=" + std::to_string(sourcePTS) + "、CP=";
    for (auto codepoint : codepoints) {
        if (codepoint) description += CodepointText(codepoint) + ",";
    }
    return description;
}

void SetCaptionFontFamilies(aribcaption::Renderer& renderer, const std::vector<std::string>& families,
    const std::string& preferredFamily, const std::set<uint32_t>& languages) {
#if defined(ARIBCC_USE_FONTCONFIG) && !defined(_WIN32)
    if (!renderer.SetDefaultFontFamily(families, !preferredFamily.empty())) {
        throw std::runtime_error("字幕フォントの候補を設定できない");
    }
    if (preferredFamily.empty()) {
        for (const auto language : languages) {
            if (!renderer.SetLanguageSpecificFontFamily(language, families)) {
                throw std::runtime_error("言語ごとの字幕フォント候補を設定できない");
            }
        }
    }
#else
    (void)families;
    (void)languages;
    if (!preferredFamily.empty() && !renderer.SetDefaultFontFamily({preferredFamily}, true)) {
        throw std::runtime_error("指定字幕フォントを設定できない");
    }
#endif
}

aribcaption::ColorRGBA Premultiply(aribcaption::ColorRGBA color) {
    const auto channel = [color](unsigned value) -> uint8_t {
        return static_cast<uint8_t>((value * color.a + 127u) / 255u);
    };
    return aribcaption::ColorRGBA(channel(color.r), channel(color.g), channel(color.b), color.a);
}

bool NeedsEnclosureCorrection(const aribcaption::Caption& caption) {
    for (const auto& region : caption.regions) {
        for (const auto& character : region.chars) {
            if (character.enclosure_style && character.text_color.a < 255) {
                return true;
            }
        }
    }
    return false;
}

void PremultiplyCaptionBackgrounds(aribcaption::Caption& caption) {
    for (auto& region : caption.regions) {
        for (auto& character : region.chars) {
            character.back_color = Premultiply(character.back_color);
        }
    }
}

aribcaption::RenderResult RenderEnclosedCaption(aribcaption::Context& context,
    const aribcaption::Caption& caption, int canvasWidth, int canvasHeight, const std::string& fontFamily,
    const std::vector<std::string>& fontFamilies) {
    using namespace aribcaption;
    Renderer renderer(context);
#if defined(ARIBCC_USE_FONTCONFIG) && !defined(_WIN32)
    const bool initialized = renderer.Initialize(CaptionType::kCaption,
        FontProviderType::kFontconfig, TextRendererType::kFreetype);
#else
    const bool initialized = renderer.Initialize();
#endif
    if (!initialized || !renderer.SetFrameSize(canvasWidth, canvasHeight)) {
        throw std::runtime_error("囲み字幕の補正レンダラーを初期化できない");
    }
    // 生成器全体で解決した候補を再利用し、文字単位描画でFcFontMatchを繰り返さない。
    SetCaptionFontFamilies(renderer, fontFamilies, fontFamily, {caption.iso6392_language_code});
    renderer.SetForceNoBackground(true);
    renderer.SetMergeRegionImages(false);
    // 本来のRendererと同じfloat演算とfloorでレターボックス・文字区画を求める。
    const float magnification = std::min(static_cast<float>(canvasWidth) / caption.plane_width,
        static_cast<float>(canvasHeight) / caption.plane_height);
    const int areaWidth = static_cast<int>(std::floor(caption.plane_width * magnification));
    const int areaHeight = static_cast<int>(std::floor(caption.plane_height * magnification));
    const float scaleX = static_cast<float>(areaWidth) / caption.plane_width;
    const float scaleY = static_cast<float>(areaHeight) / caption.plane_height;
    const auto sx = [scaleX](int value) { return static_cast<int>(std::floor(value * scaleX)); };
    const auto sy = [scaleY](int value) { return static_cast<int>(std::floor(value * scaleY)); };
    RenderResult output;
    output.pts = caption.pts;
    output.duration = caption.wait_duration;
    for (const auto& region : caption.regions) {
        const int width = sx(region.x + region.width) - sx(region.x);
        const int height = sy(region.y + region.height) - sy(region.y);
        if (width < 3 || height < 3) {
            continue;
        }
        if (width > 4096 || height > 4096) {
            throw std::runtime_error("囲み字幕の領域寸法が上限を超えた");
        }
        Image target;
        target.width = width;
        target.height = height;
        target.stride = width * 4;
        target.dst_x = (canvasWidth - areaWidth) / 2 + sx(region.x);
        target.dst_y = (canvasHeight - areaHeight) / 2 + sy(region.y);
        target.bitmap.resize(static_cast<size_t>(target.stride) * height);
        const auto fill = [&target](int left, int top, int right, int bottom, ColorRGBA color) {
            color = Premultiply(color);
            for (int y = std::max(0, top); y < std::min(target.height, bottom); ++y) {
                for (int x = std::max(0, left); x < std::min(target.width, right); ++x) {
                    auto* pixel = target.bitmap.data() + static_cast<size_t>(y) * target.stride + x * 4;
                    pixel[0] = color.r;
                    pixel[1] = color.g;
                    pixel[2] = color.b;
                    pixel[3] = color.a;
                }
            }
        };
        for (const auto& character : region.chars) {
            const int left = sx(character.x) - sx(region.x);
            const int top = sy(character.y) - sy(region.y);
            const int right = left + sx(character.x + character.section_width()) - sx(character.x);
            const int bottom = top + sy(character.y + character.section_height()) - sy(character.y);
            if (right - left < 3 || bottom - top < 3) {
                continue;
            }
            // 元実装と同じ文字順で背景と囲み線を上書きし、字形をその上へ合成する。
            fill(left, top, right, bottom, character.back_color);
            const int lineWidth = std::max(sx(1), 1);
            const int lineHeight = std::max(sy(1), 1);
            if (character.enclosure_style & kEnclosureStyleTop) {
                fill(left, top, right, top + lineHeight, character.text_color);
            }
            if (character.enclosure_style & kEnclosureStyleBottom) {
                fill(left, bottom - lineHeight, right, bottom, character.text_color);
            }
            if (character.enclosure_style & kEnclosureStyleLeft) {
                fill(left, top, left + lineWidth, bottom, character.text_color);
            }
            if (character.enclosure_style & kEnclosureStyleRight) {
                fill(right - lineWidth, top, right, bottom, character.text_color);
            }
            if (std::floor(character.char_width * character.char_horizontal_scale * scaleX) < 2 ||
                std::floor(character.char_height * character.char_vertical_scale * scaleY) < 2) {
                continue;
            }
            Caption single = caption;
            single.regions.clear();
            CaptionRegion oneRegion = region;
            oneRegion.chars = {character};
            oneRegion.chars[0].enclosure_style = kEnclosureStyleNone;
            single.regions.push_back(std::move(oneRegion));
            renderer.Flush();
            if (!renderer.AppendCaption(std::move(single))) {
                throw std::runtime_error("囲み字幕の文字登録に失敗した");
            }
            RenderResult glyph;
            const auto status = renderer.Render(caption.pts, glyph);
            if (status == RenderStatus::kError) {
                throw std::runtime_error("囲み字幕の文字描画に失敗した");
            }
            for (const auto& image : glyph.images) {
                if (image.width != target.width || image.height != target.height ||
                    image.dst_x != target.dst_x || image.dst_y != target.dst_y) {
                    throw std::runtime_error("囲み字幕の描画座標が一致しない");
                }
                for (int y = 0; y < image.height; ++y) {
                    for (int x = 0; x < image.width; ++x) {
                        const auto* source = image.bitmap.data() + static_cast<size_t>(y) * image.stride + x * 4;
                        auto* dest = target.bitmap.data() + static_cast<size_t>(y) * target.stride + x * 4;
                        const unsigned inverse = 255u - source[3];
                        for (int channel = 0; channel < 4; ++channel) {
                            dest[channel] = static_cast<uint8_t>(std::min(255u,
                                source[channel] + (dest[channel] * inverse + 127u) / 255u));
                        }
                    }
                }
            }
        }
        output.images.push_back(std::move(target));
    }
    return output;
}

std::vector<amatsukaze::pgs::Region> ConvertCaptionImages(
    const std::vector<aribcaption::Image>& images, int canvasWidth, int canvasHeight) {
    std::vector<amatsukaze::pgs::Region> regions;
    for (const auto& image : images) {
        if (image.width <= 0 || image.height <= 0 || image.width > 4096 || image.height > 4096 ||
            image.stride < image.width * 4 || image.pixel_format != aribcaption::PixelFormat::kRGBA8888 ||
            image.bitmap.size() < static_cast<size_t>(image.stride) * image.height) {
            throw std::runtime_error("字幕レンダラーが不正なRGBA画像を返した");
        }
        // libaribcaptionの描画領域には画面外の部分があり得るため、キャンバスへ切り詰める。
        const int left = std::max(0, image.dst_x);
        const int top = std::max(0, image.dst_y);
        const int right = static_cast<int>(std::min<int64_t>(canvasWidth,
            static_cast<int64_t>(image.dst_x) + image.width));
        const int bottom = static_cast<int>(std::min<int64_t>(canvasHeight,
            static_cast<int64_t>(image.dst_y) + image.height));
        if (left >= right || top >= bottom) {
            continue;
        }
        amatsukaze::pgs::Region region;
        region.x = left;
        region.y = top;
        region.width = right - left;
        region.height = bottom - top;
        region.pixels.reserve(static_cast<size_t>(region.width) * region.height);
        for (int y = top; y < bottom; ++y) {
            const auto* row = image.bitmap.data() + static_cast<size_t>(y - image.dst_y) * image.stride;
            for (int x = left; x < right; ++x) {
                const auto* pixel = row + static_cast<size_t>(x - image.dst_x) * 4;
                const unsigned alpha = pixel[3];
                const auto straight = [alpha](unsigned value) -> uint8_t {
                    return alpha ? static_cast<uint8_t>(std::min(255u, (value * 255u + alpha / 2u) / alpha)) : 0;
                };
                // Canvasの字形合成結果はpremultiplied。PGSパレットにはstraightで渡す。
                region.pixels.push_back({straight(pixel[0]), straight(pixel[1]), straight(pixel[2]), pixel[3]});
            }
        }
        regions.push_back(std::move(region));
    }
    return regions;
}

int64_t MillisecondsFromTicks(double ticks) {
    const double milliseconds = ticks / 90.0;
    // int64境界をdoubleにすると最大値が切り上がるため、両端を厳密不等号で除く。
    if (!std::isfinite(milliseconds) || milliseconds <= static_cast<double>(std::numeric_limits<int64_t>::min()) ||
        milliseconds >= static_cast<double>(std::numeric_limits<int64_t>::max())) {
        throw std::invalid_argument("字幕時刻がミリ秒の範囲を超えた");
    }
    return static_cast<int64_t>(std::llround(milliseconds));
}

int64_t OutputTicks(double seconds) {
    const double ticks = seconds * 90000.0;
    if (!std::isfinite(ticks) || ticks < 0 || ticks >= static_cast<double>(std::numeric_limits<int64_t>::max())) {
        throw std::invalid_argument("PGS出力時刻が不正");
    }
    return static_cast<int64_t>(std::llround(ticks));
}

}

std::pair<int, int> CaptionPgsCanvasSize(int width, int height, int sarWidth, int sarHeight,
    int userSarWidth, int userSarHeight) {
    const bool useUserSAR = userSarWidth > 0 && userSarHeight > 0;
    const int effectiveSarWidth = useUserSAR ? userSarWidth : sarWidth;
    const int effectiveSarHeight = useUserSAR ? userSarHeight : sarHeight;
    double displayWidth = width;
    double displayHeight = height;
    if (effectiveSarWidth > 0 && effectiveSarHeight > 0) {
        if (effectiveSarWidth >= effectiveSarHeight) {
            displayWidth *= static_cast<double>(effectiveSarWidth) / effectiveSarHeight;
        } else {
            displayHeight *= static_cast<double>(effectiveSarHeight) / effectiveSarWidth;
        }
    }
    const double scale = std::max(1.0, std::max(displayWidth, displayHeight) / CAPTION_PGS_CANVAS_MAX);
    return { static_cast<int>(std::lround(displayWidth / scale)), static_cast<int>(std::lround(displayHeight / scale)) };
}

extern "C" AMATSUKAZE_API int CaptionPgsCanvasSizeForTest(int width, int height, int sarWidth, int sarHeight,
    int userSarWidth, int userSarHeight, int* canvasWidth, int* canvasHeight) noexcept {
    if (width <= 0 || height <= 0 || !canvasWidth || !canvasHeight) {
        return 0;
    }
    const auto canvas = CaptionPgsCanvasSize(width, height, sarWidth, sarHeight, userSarWidth, userSarHeight);
    *canvasWidth = canvas.first;
    *canvasHeight = canvas.second;
    return 1;
}

std::vector<uint8_t> GenerateCaptionPgs(const StreamReformInfo& reform, EncodeFileKey key,
    int language, int canvasWidth, int canvasHeight, const std::string& fontFamily,
    CaptionPgsDiagnostic diagnostic) {
    using namespace aribcaption;
    using amatsukaze::pgs::Event;
    if (language < 1 || language > 2 || canvasWidth <= 0 || canvasHeight <= 0 ||
        canvasWidth > CAPTION_PGS_CANVAS_MAX || canvasHeight > CAPTION_PGS_CANVAS_MAX) {
        throw std::invalid_argument("PGS字幕の言語またはキャンバス寸法が不正");
    }
    const auto& pesItems = reform.getCaptionPesList();
    const auto& pesPTS = reform.getModifiedCaptionPesPTS();
    if (pesItems.size() != pesPTS.size()) {
        throw std::runtime_error("字幕PESと補正PTSの個数が一致しない");
    }
    if (pesItems.empty()) {
        return {};
    }
    for (size_t index = 0; index < pesPTS.size(); ++index) {
        if (!std::isfinite(pesPTS[index])) {
            throw std::invalid_argument("補正字幕PTSが不正: index=" + std::to_string(index) +
                "、rawPTS=" + std::to_string(pesItems[index].PTS) +
                "、modifiedPTS=" + std::to_string(pesPTS[index]) +
                "、length=" + std::to_string(pesItems[index].data.size()));
        }
    }
    const double origin = *std::min_element(pesPTS.begin(), pesPTS.end());
    const double sourceEnd = reform.getLastCaptionSourcePTS(key);
    if (!std::isfinite(origin) || !std::isfinite(sourceEnd)) {
        throw std::invalid_argument("字幕の元PTSまたは映像終端が不正");
    }
    std::string errors;
    Context context;
    context.SetLogcatCallback([&errors](LogLevel level, const char* message) {
        if (level == LogLevel::kError) {
            errors += message;
            errors += '\n';
        }
    });
    Decoder decoder(context);
    if (!decoder.Initialize(EncodingScheme::kAuto, CaptionType::kCaption, Profile::kProfileA,
        language == 1 ? LanguageId::kFirst : LanguageId::kSecond)) {
        throw std::runtime_error("ARIB字幕デコーダーを初期化できない: " + errors);
    }
    Renderer renderer(context);
#if defined(ARIBCC_USE_FONTCONFIG) && !defined(_WIN32)
    const bool initialized = renderer.Initialize(CaptionType::kCaption,
        FontProviderType::kFontconfig, TextRendererType::kFreetype);
#else
    const bool initialized = renderer.Initialize();
#endif
    if (!initialized || !renderer.SetFrameSize(canvasWidth, canvasHeight)) {
        throw std::runtime_error("ARIB字幕レンダラーを初期化できない: " + errors);
    }
    renderer.SetStoragePolicy(CaptionStoragePolicy::kUnlimited);
    renderer.SetMergeRegionImages(false);
    if (!fontFamily.empty() && !renderer.SetDefaultFontFamily({fontFamily}, true)) {
        throw std::runtime_error("指定字幕フォントを設定できない");
    }
    // 描画APIはミリ秒だが、写像には元PESの正確な90kHz時刻を用いる。
    std::map<int64_t, double> changes;
    std::map<int64_t, Caption> enclosedCaptions;
    std::map<int64_t, std::pair<Caption, double>> decodedCaptions;
    for (size_t index = 0; index < pesItems.size(); ++index) {
        const std::string pesDiagnostic = "index=" + std::to_string(index) +
            "、rawPTS=" + std::to_string(pesItems[index].PTS) +
            "、modifiedPTS=" + std::to_string(pesPTS[index]) +
            "、prevPTS=" + (index ? std::to_string(pesPTS[index - 1]) : std::string("なし")) +
            "、length=" + std::to_string(pesItems[index].data.size());
        if (pesItems[index].data.empty()) {
            throw std::invalid_argument("字幕PESが空: " + pesDiagnostic);
        }
        const int64_t pts = MillisecondsFromTicks(pesPTS[index] - origin);
        DecodeResult result;
        const auto status = decoder.Decode(pesItems[index].data.data(), pesItems[index].data.size(), pts, result);
        if (status == DecodeStatus::kError) {
            throw std::runtime_error("ARIB字幕PESのデコードに失敗した: " + pesDiagnostic + "\n" + errors);
        }
        if (status != DecodeStatus::kGotCaption) {
            continue;
        }
        if (!result.caption || result.caption->pts != pts || result.caption->wait_duration < 0) {
            throw std::runtime_error("ARIB字幕デコーダーが不正な時刻を返した: " + pesDiagnostic);
        }
        // 管理PESと本文PESは異なる時刻基準なので、受信順PTSの逆行を許容する。
        // デコードは受信順を保ち、Rendererへの投入だけ表示時刻順にする。
        decodedCaptions.insert_or_assign(pts, std::make_pair(std::move(*result.caption), pesPTS[index]));
    }
    std::set<uint32_t> codepoints;
    std::set<uint32_t> languages;
    for (const auto& decoded : decodedCaptions) {
        CollectCaptionCodepoints(decoded.second.first, codepoints);
        languages.insert(decoded.second.first.iso6392_language_code);
    }
    // 上流fontconfig実装は要求glyphを検索条件に含めないため、記号用の補助familyを補う。
    if (decodedCaptions.empty()) {
        return {};
    }
    const auto fonts = ResolveCaptionFonts(codepoints, fontFamily);
    SetCaptionFontFamilies(renderer, fonts.families, fontFamily, languages);
    if (diagnostic && fonts.families.size() > fonts.initialFamilyCount) {
        std::string added;
        for (size_t index = fonts.initialFamilyCount; index < fonts.families.size(); ++index) {
            if (!added.empty()) added += ", ";
            added += fonts.families[index];
        }
        diagnostic(false, "PGS字幕の補助フォント: " + added);
    }
    std::map<int64_t, std::string> captionDescriptions;
    for (auto& decoded : decodedCaptions) {
        auto& caption = decoded.second.first;
        const double sourcePTS = decoded.second.second;
        captionDescriptions[caption.pts] = DescribeCaption(caption, sourcePTS);
        ReplaceMissingGlyphs(caption, fonts, sourcePTS, diagnostic);
        changes[caption.pts] = sourcePTS;
        if (caption.wait_duration != DURATION_INDEFINITE) {
            if (caption.pts > std::numeric_limits<int64_t>::max() - caption.wait_duration) {
                throw std::invalid_argument("ARIB字幕の表示終了時刻が範囲を超えた");
            }
            changes[caption.pts + caption.wait_duration] = sourcePTS + caption.wait_duration * 90.0;
        }
        if (NeedsEnclosureCorrection(caption)) {
            enclosedCaptions[caption.pts] = caption;
        } else {
            enclosedCaptions.erase(caption.pts);
        }
        PremultiplyCaptionBackgrounds(caption);
        if (!renderer.AppendCaption(std::move(caption))) {
            throw std::runtime_error("ARIB字幕の登録に失敗した: " + errors);
        }
    }
    std::vector<std::pair<int64_t, double>> points;
    for (const auto& change : changes) {
        if (change.second < sourceEnd) {
            points.push_back(change);
        }
    }
    if (points.empty()) {
        return {};
    }
    points.emplace_back(MillisecondsFromTicks(sourceEnd - origin), sourceEnd);
    std::vector<Event> events;
    std::set<int64_t> failedCaptions;
    std::set<int64_t> warnedCaptions;
    for (size_t index = 0; index + 1 < points.size(); ++index) {
        const double sourceStart = points[index].second;
        const double sourceStop = points[index + 1].second;
        if (sourceStart >= sourceStop) {
            continue;
        }
        double outputStart = 0;
        double outputEnd = 0;
        if (!reform.mapCaptionInterval(key, sourceStart, sourceStop, outputStart, outputEnd)) {
            continue;
        }
        const int64_t start90k = OutputTicks(outputStart);
        const int64_t end90k = OutputTicks(outputEnd);
        if (start90k >= end90k) {
            continue;
        }
        auto currentCaption = captionDescriptions.upper_bound(points[index].first);
        if (currentCaption == captionDescriptions.begin()) {
            continue;
        }
        --currentCaption;
        const int64_t captionPTS = currentCaption->first;
        if (failedCaptions.count(captionPTS)) {
            continue;
        }
        errors.clear();
        RenderResult result;
        const auto status = renderer.Render(points[index].first, result);
        if (status == RenderStatus::kError) {
            failedCaptions.insert(captionPTS);
            if (diagnostic) {
                diagnostic(true, "ARIB字幕の描画に失敗したため該当字幕を省略: " +
                    currentCaption->second + "\n" + errors);
            }
            continue;
        }
        if (!errors.empty() && warnedCaptions.insert(captionPTS).second && diagnostic) {
            diagnostic(true, "ARIB字幕の一部文字を描画できない: " + currentCaption->second + "\n" + errors);
        }
        if (status == RenderStatus::kNoImage) {
            continue;
        }
        if (status == RenderStatus::kGotImageUnchanged && !events.empty() && events.back().end90k == start90k) {
            events.back().end90k = end90k;
            continue;
        }
        const auto enclosed = enclosedCaptions.find(result.pts);
        if (enclosed != enclosedCaptions.end()) {
            try {
                result = RenderEnclosedCaption(context, enclosed->second, canvasWidth, canvasHeight,
                    fontFamily, fonts.families);
            } catch (const std::runtime_error& error) {
                failedCaptions.insert(captionPTS);
                if (diagnostic) {
                    diagnostic(true, "囲み字幕の描画に失敗したため該当字幕を省略: " +
                        currentCaption->second + "\n" + error.what() + "\n" + errors);
                }
                continue;
            }
            if (!errors.empty() && warnedCaptions.insert(captionPTS).second && diagnostic) {
                diagnostic(true, "囲み字幕の一部文字を描画できない: " + currentCaption->second + "\n" + errors);
            }
        }
        auto regions = ConvertCaptionImages(result.images, canvasWidth, canvasHeight);
        if (!regions.empty()) {
            events.push_back({start90k, end90k, std::move(regions)});
        }
    }
    return amatsukaze::pgs::PgsEncoder::Encode(canvasWidth, canvasHeight, events);
}
