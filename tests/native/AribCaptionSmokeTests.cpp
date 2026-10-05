#include "CaptionPgs.h"

#include <cstring>
#include <cstdio>
#include <initializer_list>

int main() {
    char diagnostic[2048] = {};
    if (CaptionPgsCheckRenderer(1920, 1080, nullptr, diagnostic, sizeof(diagnostic)) != 1) {
        std::fprintf(stderr, "失敗: %s\n", diagnostic);
        return 1;
    }
    std::printf("%s\n", diagnostic);
#if !defined(_WIN32)
    if (!std::strstr(diagnostic, "Noto Sans CJK JP") ||
        CaptionPgsCheckRenderer(1920, 1080, "AmatsukazeMissingFont_75437", diagnostic, sizeof(diagnostic)) != 0 ||
        !std::strstr(diagnostic, "指定フォント")) {
        std::fprintf(stderr, "指定フォントの存在確認に失敗した: %s\n", diagnostic);
        return 1;
    }
#endif
    for (const auto width : {0, -1, 4097}) {
        if (CaptionPgsCheckRenderer(width, 1080, nullptr, diagnostic, sizeof(diagnostic)) != 0 ||
            !std::strstr(diagnostic, "キャンバス寸法")) {
            std::fprintf(stderr, "キャンバスの入力検証に失敗した\n");
            return 1;
        }
    }
    if (CaptionPgsCheckRenderer(1920, 0, nullptr, diagnostic, sizeof(diagnostic)) != 0 ||
        CaptionPgsCheckRenderer(1920, 1080, "", diagnostic, sizeof(diagnostic)) != 0 ||
        CaptionPgsCheckRenderer(0, 0, nullptr, nullptr, 0) != 0) {
        std::fprintf(stderr, "不正入力の処理に失敗した\n");
        return 1;
    }
    char small[2] = {'x', 'x'};
    if (CaptionPgsCheckRenderer(0, 0, nullptr, small, sizeof(small)) != 0 || small[1] != '\0') {
        std::fprintf(stderr, "診断バッファの終端処理に失敗した\n");
        return 1;
    }
    std::printf("日本語描画・フォント欠落・不正入力・診断バッファの確認成功\n");
    return 0;
}
