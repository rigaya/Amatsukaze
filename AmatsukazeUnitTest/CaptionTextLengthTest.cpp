#include "gtest/gtest.h"

__declspec(dllimport) int AmatsukazeCLI(int argc, const wchar_t* argv[]);

TEST(CaptionText, UnicodeCharacterFormatting) {
    const wchar_t* args[] = {
        L"AmatsukazeTest.exe",
        L"--mode",
        L"test_caption_text_length",
    };
    EXPECT_EQ(AmatsukazeCLI(static_cast<int>(sizeof(args) / sizeof(args[0])), args), 0);
}
