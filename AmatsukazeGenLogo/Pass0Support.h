#pragma once

#include "rgy_tchar.h"

#include <filesystem>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace genlogo::pass0 {

struct ToolPaths {
    std::filesystem::path cli;
    std::filesystem::path chapterExe;
    std::filesystem::path jls;
    std::filesystem::path jlsCmd;
};

struct InvocationOptions {
    tstring input;
    int serviceId = 0;
    ToolPaths tools;
    tstring jlsOption;
    tstring chapterExeOptions;
    std::filesystem::path resumeDir;
    std::filesystem::path artifactBase;
};

// 配布ディレクトリを優先してpass0に必要な補助ツールを決定する。
ToolPaths ResolveToolPaths(const std::filesystem::path& executableDir,
    const std::optional<tstring>& cliOverride,
    const std::optional<tstring>& chapterExeOverride,
    const std::optional<tstring>& jlsOverride,
    const std::optional<tstring>& jlsCmdOverride);

// AmatsukazeCLIへ渡すCM解析専用の引数列を構築する。
std::vector<tstring> BuildInvocationArgs(const InvocationOptions& options);

// 手動矩形ではCM解析を起動しない。
bool ShouldRunPass0(bool automaticRect, bool enabled);

// 新APIを呼ぶ前の失敗は従来のTS直接自動検出へ戻す。
bool ShouldFallbackToLegacy(bool cliSucceeded, bool artifactReady, bool hasPass0Api);

// pass0成果物はreadyと同じ基準パスの3ファイルが全て通常ファイルの場合だけ利用する。
bool IsArtifactReady(const std::filesystem::path& artifactBase);

struct ExpiredCleanupResult {
    int removed = 0;
    int failed = 0;
};

// 所有者が存在しない古いpass0ジョブだけを回収する。直下以外とシンボリックリンクは対象外。
ExpiredCleanupResult CleanupExpiredJobs(const std::filesystem::path& baseDirectory,
    std::chrono::hours maxAge, tstring& error);

// WindowsのCreateProcess用に、argv一要素を可逆に引用する。
tstring QuoteWindowsArgument(const tstring& argument);

// 排他的に作成したジョブ専用ディレクトリだけを削除するRAII所有者。
class OwnedJobDirectory {
public:
    OwnedJobDirectory() = default;
    ~OwnedJobDirectory();
    OwnedJobDirectory(const OwnedJobDirectory&) = delete;
    OwnedJobDirectory& operator=(const OwnedJobDirectory&) = delete;
    OwnedJobDirectory(OwnedJobDirectory&& other) noexcept;
    OwnedJobDirectory& operator=(OwnedJobDirectory&& other) noexcept;

    static std::optional<OwnedJobDirectory> Create(const std::filesystem::path& baseDirectory, tstring& error);
    const std::filesystem::path& path() const { return path_; }
    bool owns() const { return owns_; }
    bool Cleanup(tstring* error = nullptr);

private:
    explicit OwnedJobDirectory(std::filesystem::path path) : path_(std::move(path)), owns_(true) {}
    std::filesystem::path path_;
    bool owns_ = false;
};

} // namespace genlogo::pass0
