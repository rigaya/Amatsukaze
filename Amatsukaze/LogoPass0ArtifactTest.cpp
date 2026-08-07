#include "LogoPass0Artifact.h"
#include "TrimAvs.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#if !defined(_WIN32) && !defined(_WIN64)
#include <unistd.h>
#else
#include <windows.h>
#endif

namespace {

namespace fs = std::filesystem;

std::atomic<unsigned int> testDirectorySerial { 0 };

bool Expect(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "失敗: " << message << std::endl;
    }
    return condition;
}

class TestDirectory {
public:
    TestDirectory() {
#if !defined(_WIN32) && !defined(_WIN64)
        char pattern[] = "/tmp/amatsukaze-logo-pass0-artifact-XXXXXX";
        char* created = mkdtemp(pattern);
        if (created != nullptr) {
            path_ = fs::path(created);
            owns_ = true;
        }
#else
        std::error_code ec;
        const auto temporaryRoot = fs::temp_directory_path(ec);
        if (ec) return;
        for (unsigned int attempt = 0; attempt < 128; attempt++) {
            const auto suffix = std::to_wstring((unsigned long long)GetCurrentProcessId()) + L"-"
                + std::to_wstring((unsigned long long)testDirectorySerial.fetch_add(1));
            const auto candidate = temporaryRoot / (std::wstring(L"amatsukaze-logo-pass0-artifact-") + suffix);
            if (CreateDirectoryW(candidate.c_str(), nullptr) != 0) {
                path_ = candidate;
                owns_ = true;
                break;
            }
            if (GetLastError() != ERROR_ALREADY_EXISTS) break;
        }
#endif
    }
    ~TestDirectory() {
        if (owns_ && !path_.empty()) {
            // このテスト自身が作成した明示的な一時ディレクトリだけを削除する。
            std::error_code ec;
            fs::remove_all(path_, ec);
        }
    }
    const fs::path& path() const { return path_; }
    bool valid() const { return !path_.empty() && fs::is_directory(path_); }

private:
    fs::path path_;
    bool owns_ = false;
};

bool WriteFile(const fs::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary);
    output.write(text.data(), (std::streamsize)text.size());
    return (bool)output;
}

std::string ReadFile(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool WriteLiteral(FILE* file, const char* text, tstring& error) {
    if (fwrite(text, 1, strlen(text), file) != strlen(text)) {
        error = _T("テスト書き込み失敗");
        return false;
    }
    return true;
}

} // namespace

int main() {
    bool ok = true;
    TestDirectory temporary;
    ok &= Expect(temporary.valid(), "テスト用一時ディレクトリ作成");
    if (!ok) return EXIT_FAILURE;

    const auto root = temporary.path();
    std::error_code ec;
    fs::create_directories(root / "sub", ec);

    logopass0::ArtifactPaths relativePaths;
    tstring error;
    ok &= Expect(logopass0::ResolveArtifactPaths(root.native(), _T("pass0"), relativePaths, error),
        "相対baseをresume-dir直下へ正規化");
    ok &= Expect(fs::path(relativePaths.base) == fs::canonical(root) / "pass0", "相対baseの正規化結果");

    logopass0::ArtifactPaths fullPaths;
    ok &= Expect(logopass0::ResolveArtifactPaths(root.native(), (root / "fullpass0").native(), fullPaths, error),
        "full pathのbaseを許可");
    ok &= Expect(logopass0::ResolveArtifactPaths(root.native(), (root / "sub" / ".." / "same-parent").native(), fullPaths, error),
        "正規化後にresume-dirと同一実体となるfull pathを許可");
#if !defined(_WIN32) && !defined(_WIN64)
    fs::create_directory_symlink(root, root / "root-alias", ec);
    if (!ec) {
        ok &= Expect(logopass0::ResolveArtifactPaths(root.native(), (root / "root-alias" / "symlink-parent").native(), fullPaths, error),
            "symlink経由でresume-dirと同一実体のfull pathを許可");
    }
    ec.clear();
#endif
    ok &= Expect(!logopass0::ResolveArtifactPaths(root.native(), _T("sub/pass0"), fullPaths, error),
        "相対subdirを拒否");
    ok &= Expect(!logopass0::ResolveArtifactPaths(root.native(), (root / "sub" / "pass0").native(), fullPaths, error),
        "full pathのsubdirを拒否");
    ok &= Expect(!logopass0::ResolveArtifactPaths(root.native(), _T("../pass0"), fullPaths, error),
        "resume-dir外への逸脱を拒否");
    ok &= Expect(!logopass0::ResolveArtifactPaths(root.native(), _T("pass0:stream"), fullPaths, error),
        "NTFS代替ストリームを拒否");
    ok &= Expect(!logopass0::ResolveArtifactPaths(root.native(), _T("pass0."), fullPaths, error),
        "末尾dotを拒否");

    ok &= Expect(WriteFile(fs::path(relativePaths.trimAvs), "old-trim"), "既存成果物の作成");
    ok &= Expect(logopass0::HasExistingArtifact(relativePaths), "既存final成果物を検出");
    fs::remove(fs::path(relativePaths.trimAvs), ec);

    ok &= Expect(logopass0::SelectLargestVideoIndex({ 100, 100, 50 }) == 0, "同数時は先頭映像を選択");
    ok &= Expect(logopass0::SelectLargestVideoIndex({ 0, 0 }) == 0, "空映像同士も先頭を選択");

    const auto finalCollision = root / "final-collision";
    ok &= Expect(WriteFile(finalCollision, "old"), "final競合用ファイルの作成");
    error.clear();
    ok &= Expect(!logopass0::WriteUtf8TextAtomically("new", finalCollision.native(), error),
        "既存finalは上書きしない");
    ok &= Expect(ReadFile(finalCollision) == "old", "既存finalの内容を保持");

    const auto tempCollision = root / "temp-collision";
    const auto fixedTemp = tempCollision.string() + ".tmp.fixed";
    ok &= Expect(WriteFile(fixedTemp, "occupied"), "一時名競合用ファイルの作成");
    bool writerCalled = false;
    error.clear();
    ok &= Expect(!logopass0::PublishFileAtomically(tempCollision.native(),
            [&](FILE* file, tstring& writeError) { writerCalled = true; return WriteLiteral(file, "new", writeError); },
            error, nullptr, [&](const tstring&, unsigned int) { return fs::path(fixedTemp).native(); }),
        "一時名競合時は公開しない");
    ok &= Expect(!writerCalled && !fs::exists(tempCollision), "一時名競合でwriter/finalを作らない");

    logopass0::ArtifactPaths failedPaths;
    ok &= Expect(logopass0::ResolveArtifactPaths(root.native(), _T("failed"), failedPaths, error), "失敗用baseの作成");
    std::vector<tstring> failureOrder;
    error.clear();
    ok &= Expect(!logopass0::PublishArtifactsWithWriters(failedPaths,
            [&](FILE* file, tstring& writeError) { return WriteLiteral(file, "amts", writeError); },
            [&](FILE*, tstring& writeError) { writeError = _T("途中失敗"); return false; }, error, &failureOrder),
        "trim書き込み途中失敗は全体失敗");
    ok &= Expect(fs::exists(fs::path(failedPaths.amtSource))
            && !fs::exists(fs::path(failedPaths.trimAvs)) && !fs::exists(fs::path(failedPaths.ready))
            && failureOrder.size() == 1,
        "途中失敗時はreadyを公開しない");

    const auto sourceAmt = root / "source.amts";
    ok &= Expect(WriteFile(sourceAmt, "amts-body"), "AMTSourceコピー元の作成");
    std::vector<trimavs::FrameRange> pmtAppliedRanges;
    std::string trimText;
    std::string trimError;
    ok &= Expect(trimavs::FrameRangesFromLegacyTrims({ 10, 30, 50, 80 }, 100, pmtAppliedRanges, trimError)
            && trimavs::FormatTrimAvs(pmtAppliedRanges, 100, trimText, trimError),
        "PMT適用後のTrim相当を再生成");
    logopass0::ArtifactPaths successPaths;
    ok &= Expect(logopass0::ResolveArtifactPaths(root.native(), _T("success"), successPaths, error), "成功用baseの作成");
    std::vector<tstring> successOrder;
    error.clear();
    ok &= Expect(logopass0::PublishArtifacts(successPaths, sourceAmt.native(), trimText, error, &successOrder),
        "3成果物の公開");
    ok &= Expect(ReadFile(fs::path(successPaths.amtSource)) == "amts-body"
            && ReadFile(fs::path(successPaths.trimAvs)) == "Trim(10, 29) ++ Trim(50, 79)"
            && ReadFile(fs::path(successPaths.ready)) == "1\n",
        "AMTS・PMT後Trim・readyの内容");
    ok &= Expect(successOrder.size() == 3 && successOrder[0] == successPaths.amtSource
            && successOrder[1] == successPaths.trimAvs && successOrder[2] == successPaths.ready,
        "readyを最後に公開");

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
