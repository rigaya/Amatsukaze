#include "Pass0Support.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using genlogo::pass0::BuildInvocationArgs;
using genlogo::pass0::CleanupExpiredJobs;
using genlogo::pass0::InvocationOptions;
using genlogo::pass0::IsArtifactReady;
using genlogo::pass0::OwnedJobDirectory;
using genlogo::pass0::QuoteWindowsArgument;
using genlogo::pass0::ResolveToolPaths;
using genlogo::pass0::ShouldFallbackToLegacy;
using genlogo::pass0::ShouldRunPass0;
using genlogo::pass0::ToolPaths;

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool ContainsPair(const std::vector<tstring>& args, const tstring& key, const tstring& value) {
    for (size_t i = 0; i + 1 < args.size(); i++) {
        if (args[i] == key && args[i + 1] == value) {
            return true;
        }
    }
    return false;
}

bool Contains(const std::vector<tstring>& args, const tstring& value) {
    for (const auto& arg : args) {
        if (arg == value) {
            return true;
        }
    }
    return false;
}

void WriteFile(const fs::path& path, const char* content = "test") {
    std::ofstream file(path, std::ios::binary);
    file << content;
}

} // namespace

int main() {
    try {
    std::error_code ec;
    tstring rootError;
    auto rootOwner = OwnedJobDirectory::Create(fs::temp_directory_path(), rootError);
    Require(rootOwner.has_value(), "テスト用一時ディレクトリを作成できません");
    const fs::path root = rootOwner->path();

    const fs::path dist = root / "配布 dir";
    fs::create_directories(dist, ec);
    WriteFile(dist / _T("AmatsukazeCLI"));
    WriteFile(dist / _T("chapter_exe"));
    WriteFile(dist / _T("join_logo_scp"));
    WriteFile(dist / _T("JL_標準.txt"));
    const ToolPaths tools = ResolveToolPaths(dist, std::nullopt, std::nullopt, std::nullopt, std::nullopt);
#if defined(_WIN32) || defined(_WIN64)
    Require(tools.cli == dist / _T("AmatsukazeCLI.exe"), "CLIの配布ディレクトリ優先が不正です");
    Require(tools.chapterExe == dist / _T("chapter_exe.exe"), "chapter_exeの配布ディレクトリ優先が不正です");
    Require(tools.jls == dist / _T("join_logo_scp.exe"), "JLSの配布ディレクトリ優先が不正です");
#else
    Require(tools.cli == dist / _T("AmatsukazeCLI"), "CLIの配布ディレクトリ優先が不正です");
    Require(tools.chapterExe == dist / _T("chapter_exe"), "chapter_exeの配布ディレクトリ優先が不正です");
    Require(tools.jls == dist / _T("join_logo_scp"), "JLSの配布ディレクトリ優先が不正です");
#endif
    Require(tools.jlsCmd == dist / _T("JL_標準.txt"), "JLコマンドの配布ディレクトリ優先が不正です");

    const fs::path job = root / "job";
    const fs::path base = job / "pass0";
    InvocationOptions options{
        _T("/tmp/input with space.ts"), 1234, tools, _T("-foo bar"), _T("--silent"), job, base
    };
    const auto args = BuildInvocationArgs(options);
    Require(args.size() >= 25, "pass0引数が不足しています");
    Require(ContainsPair(args, _T("--mode"), _T("cm")), "CMモードがありません");
    Require(ContainsPair(args, _T("-s"), _T("1234")), "service idがありません");
    Require(ContainsPair(args, _T("--auto-logo-detect"), _T("0")), "再帰防止引数がありません");
    Require(ContainsPair(args, _T("--resume-dir"), job.native()), "resume-dirがありません");
    Require(ContainsPair(args, _T("--logo-pass0-output"), base.native()), "成果物出力先がありません");
    Require(ContainsPair(args, _T("--jls-option"), _T("-foo bar")), "JLSオプションがありません");
    Require(ContainsPair(args, _T("--chapter-exe-options"), _T("--silent")), "chapterオプションがありません");
    Require(Contains(args, _T("--chapter")), "chapter指定がありません");
    Require(Contains(args, _T("--no-logo-in-cm")), "ロゴなしCM解析指定がありません");
    Require(Contains(args, _T("--no-delogo")), "ロゴ消し無効指定がありません");
    Require(Contains(args, _T("--no-remove-tmp")), "一時ファイル保持指定がありません");
    Require(ShouldRunPass0(true, true), "自動矩形でpass0を有効化できません");
    Require(!ShouldRunPass0(false, true), "手動矩形でpass0を誤って有効化しました");
    Require(!ShouldRunPass0(true, false), "無効指定でpass0を誤って有効化しました");
    Require(ShouldFallbackToLegacy(false, true, true), "CLI失敗時のフォールバック判定が不正です");
    Require(ShouldFallbackToLegacy(true, false, true), "成果物不完全時のフォールバック判定が不正です");
    Require(ShouldFallbackToLegacy(true, true, false), "旧DLL時のフォールバック判定が不正です");
    Require(!ShouldFallbackToLegacy(true, true, true), "pass0新APIの採用判定が不正です");

    Require(QuoteWindowsArgument(_T("")) == _T("\"\""), "Windows空引数の引用が不正です");
    Require(QuoteWindowsArgument(_T("a b")) == _T("\"a b\""), "Windows空白引数の引用が不正です");
    Require(QuoteWindowsArgument(_T("日本語")) == _T("\"日本語\""), "Windows日本語引数の引用が不正です");
    Require(QuoteWindowsArgument(_T("a\"b")) == _T("\"a\\\"b\""), "Windows内部引用符の引用が不正です");
    Require(QuoteWindowsArgument(_T("C:\\tail\\")) == _T("\"C:\\tail\\\\\""), "Windows末尾backslashの引用が不正です");
    Require(QuoteWindowsArgument(_T("a\\\\\"b")) == _T("\"a\\\\\\\\\\\"b\""), "Windows複数backslashと引用符の引用が不正です");

    fs::create_directories(job, ec);
    WriteFile(fs::path(base.native() + _T(".amts")));
    WriteFile(fs::path(base.native() + _T(".trim.avs")));
    WriteFile(fs::path(base.native() + _T(".ready")), "1\n");
    Require(IsArtifactReady(base), "完全なpass0成果物を認識できません");
    WriteFile(fs::path(base.native() + _T(".ready")), "2\n");
    Require(!IsArtifactReady(base), "未知のreadyバージョンを誤認識しました");
    WriteFile(fs::path(base.native() + _T(".ready")), "1\nextra\n");
    Require(!IsArtifactReady(base), "余分なready内容を誤認識しました");
    WriteFile(fs::path(base.native() + _T(".ready")), "1\n");
    fs::remove(fs::path(base.native() + _T(".trim.avs")), ec);
    Require(!IsArtifactReady(base), "不完全なpass0成果物を誤認識しました");

    tstring error;
    auto owned1 = OwnedJobDirectory::Create(root, error);
    auto owned2 = OwnedJobDirectory::Create(root, error);
    Require(owned1.has_value() && owned2.has_value(), "所有pass0ディレクトリを作成できません");
    Require(owned1->path() != owned2->path(), "所有pass0ディレクトリが衝突しました");
    const fs::path owned1Path = owned1->path();
    Require(owned1->Cleanup(), "所有pass0ディレクトリを削除できません");
    Require(!fs::exists(owned1Path), "所有pass0ディレクトリを削除できません");
    owned2.reset();
    Require(!fs::exists(root / "job" / "not-owned"), "所有外パスを誤って作成しました");

    const fs::path oldJob = root / "pass0-genlogo-old";
    const fs::path newJob = root / "pass0-genlogo-new";
    const fs::path unrelated = root / "pass0-other";
    const fs::path symlinkTarget = root / "symlink-target";
    const fs::path symlinkJob = root / "pass0-genlogo-symlink";
    fs::create_directory(oldJob, ec);
    fs::create_directory(newJob, ec);
    fs::create_directory(unrelated, ec);
    fs::create_directory(symlinkTarget, ec);
    WriteFile(symlinkTarget / "marker");
    ec.clear();
    fs::create_directory_symlink(symlinkTarget, symlinkJob, ec);
    const bool symlinkCreated = !ec;
    fs::last_write_time(oldJob, fs::file_time_type::clock::now() - std::chrono::hours(48), ec);
    Require(!ec, "期限切れテスト時刻を設定できません");
    tstring cleanupError;
    const auto cleanup = CleanupExpiredJobs(root, std::chrono::hours(24), cleanupError);
    Require(cleanup.removed == 1 && cleanup.failed == 0, "期限切れpass0ディレクトリを回収できません");
    Require(!fs::exists(oldJob), "期限切れpass0ディレクトリが残りました");
    Require(fs::exists(newJob), "新しいpass0ディレクトリを誤削除しました");
    Require(fs::exists(unrelated), "所有外ディレクトリを誤削除しました");
    if (symlinkCreated) {
        Require(fs::is_symlink(symlinkJob), "pass0形式のsymlinkを削除または置換しました");
        Require(fs::exists(symlinkTarget / "marker"), "pass0形式symlinkの追跡先を変更しました");
    }

    Require(rootOwner->Cleanup(), "テスト用一時ディレクトリを削除できません");
    return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << std::endl;
        return 1;
    }
}
