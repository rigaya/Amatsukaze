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

void WriteRawFile(const fs::path& path, const std::string& content) {
    std::ofstream file(path, std::ios::binary);
    file.write(content.data(), (std::streamsize)content.size());
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
#if !defined(_WIN32) && !defined(_WIN64)
    WriteFile(fs::path(base.native() + _T(".trim.avs")));
    fs::remove(fs::path(base.native() + _T(".amts")), ec);
    fs::create_symlink(fs::path(base.native() + _T(".trim.avs")), fs::path(base.native() + _T(".amts")), ec);
    Require(!ec && !IsArtifactReady(base), "symlink成果物を誤認識しました");
    fs::remove(fs::path(base.native() + _T(".amts")), ec);
#endif

    tstring error;
    auto owned1 = OwnedJobDirectory::Create(root, error);
    auto owned2 = OwnedJobDirectory::Create(root, error);
    Require(owned1.has_value() && owned2.has_value(), "所有pass0ディレクトリを作成できません");
    Require(owned1->path() != owned2->path(), "所有pass0ディレクトリが衝突しました");
#if !defined(_WIN32) && !defined(_WIN64)
    const auto permissions = fs::status(owned1->path(), ec).permissions();
    Require(!ec && (permissions & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none,
        "pass0所有ディレクトリが0700ではありません");
#endif
    const fs::path owned1Path = owned1->path();
    const std::vector<tstring> expectedNames = {
        _T("audio.dat"), _T("audio.wav"), _T("i0.mpg"), _T("streaminfo.dat"), _T("resume.dat"),
        _T("amts0.dat"), _T("amts0.avs"), _T("amts0_8bit.avs"), _T("chapter_exe0.txt"),
        _T("chapter_exe_o0.txt"), _T("trim0.avs"), _T("jls0.txt"), _T("div0.txt"),
        _T("logof0.txt"), _T("tsreadex_dump.txt"), _T("pass0.amts"), _T("pass0.trim.avs"),
        _T("pass0.ready"), _T("pass0.amts.tmp.1.0.0")
    };
    for (const auto& name : expectedNames) WriteFile(owned1Path / name);
    Require(owned1->Cleanup(), "所有pass0ディレクトリを削除できません");
    Require(!fs::exists(owned1Path), "所有pass0ディレクトリを削除できません");
    const fs::path owned2Path = owned2->path();
    WriteFile(owned2Path / "unknown.txt");
    Require(!owned2->Cleanup(), "未知名を含む所有ディレクトリを削除しました");
    Require(fs::exists(owned2Path / "unknown.txt"), "未知名を誤削除しました");
    const std::vector<tstring> invalidArtifactTemps = {
        _T("pass0.amts.tmp.1.0"), _T("pass0.trim.avs.tmp.1.0.0.0"),
        _T("pass0.ready.tmp.1.a.0"), _T("pass0.amts.tmp.1..0"), _T("pass0.unknown")
    };
    for (const auto& name : invalidArtifactTemps) {
        auto invalidJob = OwnedJobDirectory::Create(root, error);
        Require(invalidJob.has_value(), "不正成果物名確認用の所有pass0ディレクトリを作成できません");
        WriteFile(invalidJob->path() / name);
        Require(!invalidJob->Cleanup(), "不正なpass0一時成果物名を含むディレクトリを削除しました");
    }
#if !defined(_WIN32) && !defined(_WIN64)
    auto owned3 = OwnedJobDirectory::Create(root, error);
    Require(owned3.has_value(), "symlink確認用の所有pass0ディレクトリを作成できません");
    fs::create_symlink(owned2Path / "unknown.txt", owned3->path() / "amts0.dat", ec);
    Require(!ec && !owned3->Cleanup(), "symlinkを含む所有ディレクトリを削除しました");
#endif
    Require(!fs::exists(root / "job" / "not-owned"), "所有外パスを誤って作成しました");

    const fs::path oldJob = root / "pass0-genlogo-11111111111111111111111111111111";
    const fs::path newJob = root / "pass0-genlogo-22222222222222222222222222222222";
    const fs::path unrelated = root / "pass0-other";
    const fs::path symlinkTarget = root / "symlink-target";
    const fs::path symlinkJob = root / "pass0-genlogo-33333333333333333333333333333333";
    const fs::path foreignJob = root / "pass0-genlogo-44444444444444444444444444444444";
    fs::create_directory(oldJob, ec);
    fs::create_directory(newJob, ec);
    fs::create_directory(unrelated, ec);
    fs::create_directory(foreignJob, ec);
    WriteFile(foreignJob / "external.txt");
    fs::create_directory(symlinkTarget, ec);
    WriteFile(symlinkTarget / "marker");
    ec.clear();
    fs::create_directory_symlink(symlinkTarget, symlinkJob, ec);
    const bool symlinkCreated = !ec;
    WriteFile(oldJob / ".logo-pass0-owner", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    WriteFile(newJob / ".logo-pass0-owner", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    const fs::path invalidMarkerNewline = root / "pass0-genlogo-55555555555555555555555555555555";
    const fs::path invalidMarkerNul = root / "pass0-genlogo-66666666666666666666666666666666";
    const fs::path invalidMarkerExtra = root / "pass0-genlogo-77777777777777777777777777777777";
    fs::create_directory(invalidMarkerNewline, ec);
    fs::create_directory(invalidMarkerNul, ec);
    fs::create_directory(invalidMarkerExtra, ec);
    WriteRawFile(invalidMarkerNewline / ".logo-pass0-owner", "cccccccccccccccccccccccccccccccc\n");
    WriteRawFile(invalidMarkerNul / ".logo-pass0-owner", std::string("cccccccccccccccccccccccccccccccc\0junk", 37));
    WriteRawFile(invalidMarkerExtra / ".logo-pass0-owner", "ccccccccccccccccccccccccccccccccx");
    fs::last_write_time(invalidMarkerNewline, fs::file_time_type::clock::now() - std::chrono::hours(48), ec);
    fs::last_write_time(invalidMarkerNul, fs::file_time_type::clock::now() - std::chrono::hours(48), ec);
    fs::last_write_time(invalidMarkerExtra, fs::file_time_type::clock::now() - std::chrono::hours(48), ec);
    fs::last_write_time(oldJob, fs::file_time_type::clock::now() - std::chrono::hours(48), ec);
    Require(!ec, "期限切れテスト時刻を設定できません");
    tstring cleanupError;
    const auto cleanup = CleanupExpiredJobs(root, std::chrono::hours(24), cleanupError);
    Require(cleanup.removed == 1 && cleanup.failed == 0, "期限切れpass0ディレクトリを回収できません");
    Require(!fs::exists(oldJob), "期限切れpass0ディレクトリが残りました");
    Require(fs::exists(newJob), "新しいpass0ディレクトリを誤削除しました");
    Require(fs::exists(unrelated), "所有外ディレクトリを誤削除しました");
    Require(fs::exists(foreignJob / "external.txt"), "所有markerのない外部内容を誤削除しました");
    Require(fs::exists(invalidMarkerNewline) && fs::exists(invalidMarkerNul) && fs::exists(invalidMarkerExtra),
        "不正なowner markerを持つディレクトリを誤削除しました");
    if (symlinkCreated) {
        Require(fs::is_symlink(symlinkJob), "pass0形式のsymlinkを削除または置換しました");
        Require(fs::exists(symlinkTarget / "marker"), "pass0形式symlinkの追跡先を変更しました");
    }

    Require(!rootOwner->Cleanup(), "外部内容を含む所有ディレクトリを再帰削除しました");
    fs::remove_all(root, ec);
    Require(!ec, "テスト用一時ディレクトリを削除できません");
    return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << std::endl;
        return 1;
    }
}
