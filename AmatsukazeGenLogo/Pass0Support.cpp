#include "Pass0Support.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <system_error>

#if defined(_WIN32) || defined(_WIN64)
#include <Windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace genlogo::pass0 {
namespace {

tstring PlatformExeName(const TCHAR* base) {
#if defined(_WIN32) || defined(_WIN64)
    return tstring(base) + _T(".exe");
#else
    return tstring(base);
#endif
}

fs::path ResolveDistributionPath(const fs::path& executableDir, const tstring& name,
    const fs::path& compatibilityFallback = fs::path()) {
    const fs::path besideExecutable = executableDir / name;
    if (fs::exists(besideExecutable)) {
        return besideExecutable;
    }
    if (!compatibilityFallback.empty() && fs::exists(compatibilityFallback)) {
        return compatibilityFallback;
    }
    return besideExecutable;
}

uint64_t CurrentProcessId() {
#if defined(_WIN32) || defined(_WIN64)
    return static_cast<uint64_t>(GetCurrentProcessId());
#else
    return static_cast<uint64_t>(getpid());
#endif
}

uint64_t CurrentUniqueTick() {
    const uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    static std::atomic<uint64_t> lastTick{ 0 };
    uint64_t previous = lastTick.load(std::memory_order_relaxed);
    for (;;) {
        const uint64_t next = std::max(now, previous + 1);
        if (lastTick.compare_exchange_weak(previous, next, std::memory_order_relaxed)) {
            return next;
        }
    }
}

tstring UInt64ToTString(uint64_t value) {
    TCHAR buffer[32] = {};
#if defined(_WIN32) || defined(_WIN64)
    _stprintf_s(buffer, _T("%llu"), static_cast<unsigned long long>(value));
#else
    snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
#endif
    return tstring(buffer);
}

tstring IntToTString(int value) {
#if defined(_WIN32) || defined(_WIN64)
    return std::to_wstring(value);
#else
    return std::to_string(value);
#endif
}

bool IsRegularFile(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec) && !ec;
}

bool HasPrefix(const tstring& value, const tstring& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

} // namespace

ToolPaths ResolveToolPaths(const fs::path& executableDir,
    const std::optional<tstring>& cliOverride,
    const std::optional<tstring>& chapterExeOverride,
    const std::optional<tstring>& jlsOverride,
    const std::optional<tstring>& jlsCmdOverride) {
    ToolPaths paths;
    paths.cli = cliOverride ? fs::path(*cliOverride)
        : ResolveDistributionPath(executableDir, PlatformExeName(_T("AmatsukazeCLI")));
    paths.chapterExe = chapterExeOverride ? fs::path(*chapterExeOverride)
        : ResolveDistributionPath(executableDir, PlatformExeName(_T("chapter_exe")));
    paths.jls = jlsOverride ? fs::path(*jlsOverride)
        : ResolveDistributionPath(executableDir, PlatformExeName(_T("join_logo_scp")));
    paths.jlsCmd = jlsCmdOverride ? fs::path(*jlsCmdOverride)
        : ResolveDistributionPath(executableDir, _T("JL_標準.txt"), executableDir.parent_path() / _T("JL") / _T("JL_標準.txt"));
    return paths;
}

std::vector<tstring> BuildInvocationArgs(const InvocationOptions& options) {
    std::vector<tstring> args;
    args.reserve(29);
    args.push_back(options.tools.cli.native());
    args.push_back(_T("--mode"));
    args.push_back(_T("cm"));
    args.push_back(_T("-i"));
    args.push_back(options.input);
    args.push_back(_T("-s"));
    args.push_back(IntToTString(options.serviceId));
    args.push_back(_T("--chapter"));
    args.push_back(_T("--no-logo-in-cm"));
    args.push_back(_T("--no-delogo"));
    args.push_back(_T("--auto-logo-detect"));
    args.push_back(_T("0"));
    args.push_back(_T("--no-remove-tmp"));
    args.push_back(_T("--resume-dir"));
    args.push_back(options.resumeDir.native());
    args.push_back(_T("--logo-pass0-output"));
    args.push_back(options.artifactBase.native());
    args.push_back(_T("--chapter-exe"));
    args.push_back(options.tools.chapterExe.native());
    args.push_back(_T("--jls"));
    args.push_back(options.tools.jls.native());
    args.push_back(_T("--jls-cmd"));
    args.push_back(options.tools.jlsCmd.native());
    if (!options.jlsOption.empty()) {
        args.push_back(_T("--jls-option"));
        args.push_back(options.jlsOption);
    }
    if (!options.chapterExeOptions.empty()) {
        args.push_back(_T("--chapter-exe-options"));
        args.push_back(options.chapterExeOptions);
    }
    return args;
}

bool ShouldRunPass0(const bool automaticRect, const bool enabled) {
    return automaticRect && enabled;
}

bool ShouldFallbackToLegacy(const bool cliSucceeded, const bool artifactReady, const bool hasPass0Api) {
    return !cliSucceeded || !artifactReady || !hasPass0Api;
}

bool IsArtifactReady(const fs::path& artifactBase) {
    const tstring base = artifactBase.native();
    const fs::path readyPath(base + _T(".ready"));
    if (!IsRegularFile(readyPath)
        || !IsRegularFile(fs::path(base + _T(".amts")))
        || !IsRegularFile(fs::path(base + _T(".trim.avs")))) {
        return false;
    }
    std::ifstream ready(readyPath, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(ready)), std::istreambuf_iterator<char>());
    return content == "1" || content == "1\n" || content == "1\r\n";
}

ExpiredCleanupResult CleanupExpiredJobs(const fs::path& baseDirectory, const std::chrono::hours maxAge, tstring& error) {
    ExpiredCleanupResult result;
    std::error_code ec;
    if (!fs::exists(baseDirectory, ec) || ec) {
        return result;
    }
    const auto cutoff = fs::file_time_type::clock::now() - maxAge;
    for (fs::directory_iterator it(baseDirectory, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::directory_entry& entry = *it;
        const tstring name = entry.path().filename().native();
        const auto status = entry.symlink_status(ec);
        if (ec) {
            break;
        }
        if (!HasPrefix(name, _T("pass0-genlogo-")) || fs::is_symlink(status) || !fs::is_directory(status)) {
            continue;
        }
        const auto modified = entry.last_write_time(ec);
        if (ec) {
            break;
        }
        if (modified >= cutoff) {
            continue;
        }
        ec.clear();
        fs::remove_all(entry.path(), ec);
        if (ec) {
            result.failed++;
            ec.clear();
        } else {
            result.removed++;
        }
    }
    if (ec) {
        error = _T("pass0期限切れ一時ディレクトリの確認に失敗しました");
    } else if (result.failed > 0) {
        error = _T("pass0期限切れ一時ディレクトリの一部を削除できません");
    }
    return result;
}

tstring QuoteWindowsArgument(const tstring& argument) {
    tstring quoted = _T("\"");
    size_t slashCount = 0;
    for (const auto ch : argument) {
        if (ch == _T('\\')) {
            slashCount++;
        } else if (ch == _T('\"')) {
            quoted.append(slashCount * 2 + 1, _T('\\'));
            quoted.push_back(ch);
            slashCount = 0;
        } else {
            quoted.append(slashCount, _T('\\'));
            quoted.push_back(ch);
            slashCount = 0;
        }
    }
    quoted.append(slashCount * 2, _T('\\'));
    quoted.push_back(_T('\"'));
    return quoted;
}

OwnedJobDirectory::~OwnedJobDirectory() {
    Cleanup();
}

OwnedJobDirectory::OwnedJobDirectory(OwnedJobDirectory&& other) noexcept
    : path_(std::move(other.path_)), owns_(other.owns_) {
    other.owns_ = false;
}

OwnedJobDirectory& OwnedJobDirectory::operator=(OwnedJobDirectory&& other) noexcept {
    if (this != &other) {
        Cleanup();
        path_ = std::move(other.path_);
        owns_ = other.owns_;
        other.owns_ = false;
    }
    return *this;
}

std::optional<OwnedJobDirectory> OwnedJobDirectory::Create(const fs::path& baseDirectory, tstring& error) {
    std::error_code ec;
    fs::create_directories(baseDirectory, ec);
    if (ec) {
        error = _T("pass0一時ディレクトリの親を作成できません");
        return std::nullopt;
    }
    const tstring prefix = _T("pass0-genlogo-") + UInt64ToTString(CurrentProcessId()) + _T("-") + UInt64ToTString(CurrentUniqueTick()) + _T("-");
    for (uint64_t index = 0; index < 128; index++) {
        const fs::path candidate = baseDirectory / (prefix + UInt64ToTString(index));
        ec.clear();
        if (fs::create_directory(candidate, ec)) {
            return OwnedJobDirectory(candidate);
        }
        if (ec && ec != std::errc::file_exists) {
            error = _T("pass0一時ディレクトリを作成できません");
            return std::nullopt;
        }
    }
    error = _T("pass0一時ディレクトリ名が衝突しました");
    return std::nullopt;
}

bool OwnedJobDirectory::Cleanup(tstring* error) {
    if (!owns_) {
        return true;
    }
    std::error_code ec;
    fs::remove_all(path_, ec);
    if (ec) {
        if (error) {
            *error = _T("pass0一時ディレクトリを削除できません");
        }
        return false;
    }
    owns_ = false;
    return true;
}

} // namespace genlogo::pass0
