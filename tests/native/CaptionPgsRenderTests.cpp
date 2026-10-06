// 未使用の統合生成器はセクション単位で除去し、描画補正の実装を直接検証する。
#include "../../Amatsukaze/CaptionPgs.cpp"
#include <cstdio>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

aribcaption::Caption MakeCaption() {
    using namespace aribcaption;
    Caption caption;
    caption.iso6392_language_code = ThreeCC("jpn");
    caption.plane_width = 960;
    caption.plane_height = 540;
    caption.wait_duration = 1000;
    CaptionRegion region;
    region.x = 100;
    region.y = 100;
    region.width = 64;
    region.height = 64;
    CaptionChar character;
    character.codepoint = 0x65e5;
    std::memcpy(character.u8str, u8"日", sizeof(u8"日"));
    character.x = region.x;
    character.y = region.y;
    character.char_width = 48;
    character.char_height = 48;
    character.char_horizontal_spacing = 16;
    character.char_vertical_spacing = 16;
    character.char_horizontal_scale = 1.0f;
    character.char_vertical_scale = 1.0f;
    character.text_color = ColorRGBA(0, 255, 0, 128);
    character.back_color = ColorRGBA(255, 0, 0, 128);
    character.stroke_color = ColorRGBA(0, 0, 255, 128);
    region.chars.push_back(character);
    caption.regions.push_back(region);
    return caption;
}

aribcaption::RenderResult Render(aribcaption::Context& context, aribcaption::Caption caption) {
    using namespace aribcaption;
    Renderer renderer(context);
    Require(renderer.Initialize(CaptionType::kCaption, FontProviderType::kFontconfig,
        TextRendererType::kFreetype), "レンダラー初期化失敗");
    Require(renderer.SetFrameSize(1920, 1080), "キャンバス設定失敗");
    renderer.SetStrokeWidth(0);
    renderer.SetMergeRegionImages(false);
    Require(renderer.AppendCaption(std::move(caption)), "字幕登録失敗");
    RenderResult result;
    Require(renderer.Render(0, result) == RenderStatus::kGotImage && result.images.size() == 1,
        "字幕描画失敗");
    return result;
}

void CheckAlpha() {
    using namespace aribcaption;
    Context context;
    auto caption = MakeCaption();
    const auto raw = Render(context, caption);
    Require(raw.images[0].bitmap[0] == 255 && raw.images[0].bitmap[3] == 128,
        "上流の背景straight直書きを再現できない");
    PremultiplyCaptionBackgrounds(caption);
    const auto fixed = Render(context, caption);
    Require(fixed.images[0].bitmap[0] == 128 && fixed.images[0].bitmap[3] == 128,
        "半透明背景のpremult補正失敗");
    const auto converted = ConvertCaptionImages(fixed.images, 1920, 1080);
    Require(converted[0].pixels[0].r == 255 && converted[0].pixels[0].a == 128,
        "背景のstraight復元失敗");
    size_t blended = 0;
    for (size_t index = 0; index < fixed.images[0].bitmap.size(); index += 4) {
        const auto* pixel = fixed.images[0].bitmap.data() + index;
        Require(pixel[0] <= pixel[3] && pixel[1] <= pixel[3] && pixel[2] <= pixel[3],
            "合成画像のRGBがpremult範囲を超えた");
        if (pixel[1] && pixel[3] > 128) {
            ++blended;
            Require(pixel[0] < 128, "緑字形と赤背景のsource-over合成失敗");
        }
    }
    Require(blended > 0, "有彩色の半透明字形合成がない");
    std::printf("半透明赤背景＋緑字形: 合成画素=%zu、背景straight復元成功\n", blended);

    auto antialiased = MakeCaption();
    antialiased.regions[0].chars[0].back_color = ColorRGBA(0, 0, 0, 0);
    const auto antialiasedImage = Render(context, antialiased);
    const auto antialiasedStraight = ConvertCaptionImages(antialiasedImage.images, 1920, 1080);
    size_t edgePixels = 0;
    const auto& bitmap = antialiasedImage.images[0];
    for (int y = 0; y < bitmap.height; ++y) {
        for (int x = 0; x < bitmap.width; ++x) {
            const auto* premult = bitmap.bitmap.data() + static_cast<size_t>(y) * bitmap.stride + x * 4;
            const auto straight = antialiasedStraight[0].pixels[static_cast<size_t>(y) * bitmap.width + x];
            if (!premult[3]) continue;
            if (premult[3] < 127) ++edgePixels;
            const unsigned channels[] = {straight.r, straight.g, straight.b};
            for (int channel = 0; channel < 3; ++channel) {
                const unsigned restored = (channels[channel] * straight.a + 127) / 255;
                Require(std::abs(static_cast<int>(restored) - premult[channel]) <= 1,
                    "AA縁のstraight/premult往復で暗い縁が生じた");
                const unsigned overWhite = (channels[channel] * straight.a + 255 * (255 - straight.a) + 127) / 255;
                Require(std::abs(static_cast<int>(overWhite) - (premult[channel] + 255 - premult[3])) <= 1,
                    "AA縁の白背景合成がstraight/premultで一致しない");
            }
        }
    }
    Require(edgePixels > 0, "AA縁の画素を検証できない");
    std::printf("AA縁%zu画素のstraight/premult往復・黒白背景合成が誤差1以内\n", edgePixels);

    caption = MakeCaption();
    caption.regions[0].chars[0].enclosure_style = static_cast<EnclosureStyle>(
        kEnclosureStyleTop | kEnclosureStyleBottom | kEnclosureStyleLeft | kEnclosureStyleRight);
    caption.regions[0].chars[0].style = kCharStyleUnderline;
    Require(NeedsEnclosureCorrection(caption), "半透明囲み線を検出できない");
    const auto enclosureFonts = ResolveCaptionFonts({0x65e5}, "").families;
    const auto enclosed = RenderEnclosedCaption(context, caption, 1920, 1080, "", enclosureFonts);
    Require(enclosed.images.size() == 1 && enclosed.images[0].bitmap[0] == 0 &&
        enclosed.images[0].bitmap[1] == 128 && enclosed.images[0].bitmap[3] == 128,
        "半透明囲み線のpremult補正失敗");
    const auto enclosureStraight = ConvertCaptionImages(enclosed.images, 1920, 1080);
    Require(enclosureStraight[0].pixels[0].g == 255 && enclosureStraight[0].pixels[0].a == 128,
        "半透明囲み線のstraight復元失敗");
    // 同じ時刻の複数領域とルビ属性も文字単位補正で保持する。
    auto second = caption.regions[0];
    second.x += 100;
    second.is_ruby = true;
    second.chars[0].x += 100;
    caption.regions.push_back(second);
    const auto two = RenderEnclosedCaption(context, caption, 1920, 1080, "", enclosureFonts);
    Require(two.images.size() == 2 && two.images[1].dst_x - two.images[0].dst_x == 200 &&
        two.pts == caption.pts && two.duration == caption.wait_duration,
        "囲み線補正で領域・ルビ・時刻が失われた");
    std::printf("半透明囲み線・下線・2領域・ルビ・表示時刻の確認成功\n");
}

std::vector<uint8_t> Pes(uint8_t group, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> pes = {0x80, 0xff, 0xf0, static_cast<uint8_t>(group << 2), 0, 0,
        static_cast<uint8_t>(body.size() >> 8), static_cast<uint8_t>(body.size())};
    pes.insert(pes.end(), body.begin(), body.end());
    return pes;
}

void CheckMissingGlyphs() {
    using namespace aribcaption;
    Context context;
    auto caption = MakeCaption();
    caption.regions[0].chars[0].codepoint = 0x10ffff;
    auto visible = caption.regions[0];
    visible.x += 100;
    visible.chars[0].x += 100;
    visible.chars[0].codepoint = 0x65e5;
    caption.regions.push_back(visible);
    std::set<uint32_t> codepoints;
    CollectCaptionCodepoints(caption, codepoints);
    const auto fonts = ResolveCaptionFonts(codepoints, "");
    Require(fonts.codepointFamilies.count(0x10ffff) && fonts.codepointFamilies.at(0x10ffff).empty(),
        "未解決CPがキャッシュされていない");
    std::vector<std::string> warnings;
    ReplaceMissingGlyphs(caption, fonts, 1234567.0, [&warnings](bool warning, const std::string& message) {
        Require(warning, "未解決CPの診断が警告ではない");
        warnings.push_back(message);
    });
    Require(warnings.size() == 1 && warnings[0].find("U+10FFFF") != std::string::npos &&
        warnings[0].find("元PTS90k=1234567") != std::string::npos,
        "未解決CPと元PTSの警告がない");
    const auto& missing = caption.regions[0].chars[0];
    Require(missing.codepoint == 0x3000 && missing.x == 100 && missing.char_width == 48 &&
        missing.back_color.r == 255 && missing.back_color.a == 128,
        "欠落文字の空白代替で位置・区画・背景が変わった");
    Require(caption.regions[1].chars[0].codepoint == 0x65e5, "欠落文字以外が失われた");
    Renderer renderer(context);
    Require(renderer.Initialize(CaptionType::kCaption, FontProviderType::kFontconfig,
        TextRendererType::kFreetype) && renderer.SetFrameSize(1920, 1080), "空白代替レンダラー初期化失敗");
    SetCaptionFontFamilies(renderer, fonts.families, "", {ThreeCC("jpn")});
    PremultiplyCaptionBackgrounds(caption);
    Require(renderer.AppendCaption(caption), "空白代替字幕登録失敗");
    RenderResult result;
    Require(renderer.Render(0, result) == RenderStatus::kGotImage && result.images.size() == 2,
        "1文字欠落で他の字幕画像も失われた");
    caption.regions[0].chars[0].enclosure_style = kEnclosureStyleTop;
    const auto cacheBefore = fonts.codepointFamilies;
    const auto enclosed = RenderEnclosedCaption(context, caption, 1920, 1080, "", fonts.families);
    Require(enclosed.images.size() == 2 && fonts.codepointFamilies == cacheBefore,
        "囲み線補正でCPキャッシュが変わった");

    auto noBlankCaption = MakeCaption();
    noBlankCaption.regions[0].chars[0].codepoint = 0x10ffff;
    auto noBlankFonts = fonts;
    noBlankFonts.blankCodepoint = 0;
    ReplaceMissingGlyphs(noBlankCaption, noBlankFonts, 1500000, {});
    Require(noBlankCaption.regions[0].chars[0].type == CaptionCharType::kDRCS &&
        noBlankCaption.drcs_map.count(noBlankCaption.regions[0].chars[0].drcs_code),
        "空白フォントも欠落した場合に透明DRCSへ代替できない");
    Renderer noBlankRenderer(context);
    Require(noBlankRenderer.Initialize(CaptionType::kCaption, FontProviderType::kFontconfig,
        TextRendererType::kFreetype) && noBlankRenderer.SetFrameSize(1920, 1080),
        "空白無し代替レンダラー初期化失敗");
    PremultiplyCaptionBackgrounds(noBlankCaption);
    Require(noBlankRenderer.AppendCaption(std::move(noBlankCaption)) &&
        noBlankRenderer.Render(0, result) == RenderStatus::kGotImage,
        "空白フォントも欠落した代替字幕を描画できない");

    auto drcsCaption = MakeCaption();
    drcsCaption.regions[0].chars[0].type = CaptionCharType::kDRCS;
    drcsCaption.regions[0].chars[0].codepoint = 0;
    drcsCaption.regions[0].chars[0].drcs_code = 100;
    drcsCaption.drcs_map[100].alternative_ucs4 = 0x10ffff;
    codepoints.clear();
    CollectCaptionCodepoints(drcsCaption, codepoints);
    Require(!codepoints.count(0x10ffff), "DRCSビットマップだけの文字で補助フォントを探索した");
    drcsCaption.regions[0].chars[0].type = CaptionCharType::kDRCSReplaced;
    drcsCaption.regions[0].chars[0].codepoint = 0x10ffff;
    ReplaceMissingGlyphs(drcsCaption, fonts, 2000000, {});
    Require(drcsCaption.regions[0].chars[0].type == CaptionCharType::kDRCS,
        "欠落したDRCS代替Unicodeを元ビットマップへ戻せない");
    std::printf("未解決CPのキャッシュ・空白代替・CP/PTS警告・他字幕保持・囲み線候補再利用・DRCS保持成功\n");
}

void CheckFontFallback() {
    using namespace aribcaption;
    const auto families = ResolveCaptionFonts({0x65e5, 0x269e, 0x269f}, "").families;
    Require(families.size() > 4 && families[0] == "Noto Sans CJK JP" && families[3] == "sans-serif",
        "日本語既定候補または記号用の補助フォントが失われた");
    const auto alias = ResolveCaptionFonts({0x269e}, "sans-serif");
    Require(alias.families[0] == "sans-serif", "指定フォントの別名を優先できない");
    // 指定フォントの有無: 総称名と実在のfamilyは有効、存在しないfamilyは代替として警告対象になる
    Require(alias.preferredFamilyAvailable, "総称名の指定を存在しないフォントと判定した");
    Require(ResolveCaptionFonts({}, "Noto Sans CJK JP").preferredFamilyAvailable, "実在するフォントを存在しないと判定した");
    Require(ResolveCaptionFonts({}, "noto sans cjk jp").preferredFamilyAvailable, "family名の大文字小文字を区別した");
    const auto missing = ResolveCaptionFonts({}, "存在しないフォントAmatsukazeTest");
    Require(!missing.preferredFamilyAvailable && !missing.codepointFamilies.at(0x65e5).empty(),
        "存在しないフォントを検出できないか、代替フォントを解決できない");
    Require(ResolveCaptionFonts({}, "").preferredFamilyAvailable, "フォント指定なしを存在しないフォントと判定した");
    Context context;
    std::string errors;
    context.SetLogcatCallback([&errors](LogLevel level, const char* text) {
        if (level == LogLevel::kError) errors += text;
    });
    Renderer renderer(context);
    Require(renderer.Initialize(CaptionType::kCaption, FontProviderType::kFontconfig,
        TextRendererType::kFreetype) && renderer.SetFrameSize(1920, 1080), "記号レンダラー初期化失敗");
    SetCaptionFontFamilies(renderer, families, "", {ThreeCC("jpn")});
    auto caption = MakeCaption();
    caption.regions[0].chars[0].codepoint = 0x269e;
    Require(renderer.AppendCaption(std::move(caption)), "記号字幕登録失敗");
    RenderResult result;
    Require(renderer.Render(0, result) == RenderStatus::kGotImage && errors.empty(),
        "fontconfig補助familyによる記号のfallback描画失敗");
    std::printf("日本語既定候補・指定font別名・指定fontの有無・不足記号glyphのfallback確認成功\n");
}

void CheckImageConversion() {
    aribcaption::Image image;
    image.width = 2;
    image.height = 1;
    image.stride = 12;
    image.dst_x = -1;
    image.bitmap = {0, 0, 0, 0, 8, 16, 24, 32, 99, 99, 99, 99};
    const auto regions = ConvertCaptionImages({image}, 1920, 1080);
    Require(regions.size() == 1 && regions[0].x == 0 && regions[0].width == 1 &&
        regions[0].pixels[0].r == 64 && regions[0].pixels[0].g == 128 &&
        regions[0].pixels[0].b == 191 && regions[0].pixels[0].a == 32,
        "画面外clip・stride・straight復元失敗");
    image.stride = 1;
    bool rejected = false;
    try {
        ConvertCaptionImages({image}, 1920, 1080);
    } catch (const std::exception&) {
        rejected = true;
    }
    Require(rejected, "不正なstrideを成功扱いした");
    Require(MillisecondsFromTicks(90009) == 1000 && OutputTicks(1.0001) == 90009,
        "ミリ秒丸め・出力90kHz変換失敗");
    std::printf("画像clip・stride・straight復元・不正画像・時刻変換の確認成功\n");
}

void CheckLanguages() {
    using namespace aribcaption;
    Context context;
    Decoder first(context), second(context);
    Require(first.Initialize(EncodingScheme::kAuto, CaptionType::kCaption, Profile::kProfileA,
        LanguageId::kFirst) && second.Initialize(EncodingScheme::kAuto, CaptionType::kCaption,
        Profile::kProfileA, LanguageId::kSecond), "2言語デコーダー初期化失敗");
    const auto management = Pes(0, {0, 2, 0, 'j', 'p', 'n', 0x80, 0x20, 'e', 'n', 'g', 0x80, 0, 0, 0});
    DecodeResult result;
    for (auto* decoder : {&first, &second}) {
        Require(decoder->Decode(management.data(), management.size(), 0, result) == DecodeStatus::kNoCaption,
            "字幕なしの管理PESがデコードできない");
        Require(decoder->QueryISO6392LanguageCode(LanguageId::kFirst) == ThreeCC("jpn") &&
            decoder->QueryISO6392LanguageCode(LanguageId::kSecond) == ThreeCC("eng"),
            "管理PESの2言語情報が失われた");
    }
    const auto language1 = Pes(1, {0, 0, 0, 7, 0x1f, 0x20, 0, 0, 2, 0x0e, 'A'});
    const auto language2 = Pes(2, {0, 0, 0, 7, 0x1f, 0x20, 0, 0, 2, 0x0e, 'B'});
    Require(first.Decode(language1.data(), language1.size(), 100, result) == DecodeStatus::kGotCaption &&
        result.caption && result.caption->pts == 100 && result.caption->text == "Ａ",
        "第1言語字幕のデコード失敗");
    Require(second.Decode(language1.data(), language1.size(), 100, result) == DecodeStatus::kNoCaption,
        "第1言語字幕が第2言語へ混入した");
    Require(second.Decode(language2.data(), language2.size(), 50, result) == DecodeStatus::kGotCaption &&
        result.caption && result.caption->pts == 50 && result.caption->text == "Ｂ",
        "第2言語字幕のデコード失敗");
    Require(first.Decode(language2.data(), language2.size(), 200, result) == DecodeStatus::kNoCaption,
        "第2言語字幕が第1言語へ混入した");
    const auto invalid = Pes(1, {0});
    Require(first.Decode(invalid.data(), invalid.size(), 0, result) == DecodeStatus::kError,
        "不正PESを成功扱いした");
    std::printf("管理PES・2言語分離・不正PESの確認成功\n");
}

}

int main() {
    try {
        CheckAlpha();
        CheckLanguages();
        CheckImageConversion();
        CheckFontFallback();
        CheckMissingGlyphs();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "失敗: %s\n", error.what());
        return 1;
    }
}
