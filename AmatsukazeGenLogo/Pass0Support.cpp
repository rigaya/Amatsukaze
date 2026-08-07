#include "Pass0Support.h"

#include "rgy_util.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <random>
#include <system_error>

#if defined(_WIN32) || defined(_WIN64)
#include <Windows.h>
#else
#include <fcntl.h>
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

constexpr const TCHAR* kJobPrefix = _T("pass0-genlogo-");
constexpr const TCHAR* kOwnerMarkerName = _T(".logo-pass0-owner");
constexpr size_t kTokenLength = 32;

bool IsHexToken(const tstring& token) {
    if (token.size() != kTokenLength) {
        return false;
    }
    for (const auto ch : token) {
        if (!((ch >= _T('0') && ch <= _T('9')) || (ch >= _T('a') && ch <= _T('f')))) {
            return false;
        }
    }
    return true;
}

tstring MakeRandomToken() {
    static std::random_device randomDevice;
    static std::mt19937_64 randomEngine(randomDevice());
    static std::mutex randomMutex;
    std::lock_guard<std::mutex> lock(randomMutex);
    const uint64_t first = randomEngine() ^ CurrentUniqueTick();
    const uint64_t second = randomEngine() ^ (CurrentProcessId() << 32);
    const char* hex = "0123456789abcdef";
    tstring token;
    token.reserve(kTokenLength);
    for (int shift = 60; shift >= 0; shift -= 4) token.push_back((TCHAR)hex[(first >> shift) & 0xf]);
    for (int shift = 60; shift >= 0; shift -= 4) token.push_back((TCHAR)hex[(second >> shift) & 0xf]);
    // N形式GUIDとしても解釈できるよう、version 4 / RFC 4122 variantを固定する。
    token[12] = _T('4');
    token[16] = (TCHAR)hex[8 + ((second >> 62) & 0x3)];
    return token;
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
    const auto status = fs::symlink_status(path, ec);
    return !ec && !fs::is_symlink(status) && fs::is_regular_file(status);
}

bool HasPrefix(const tstring& value, const tstring& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool IsJobDirectory(const fs::path& path) {
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (ec || fs::is_symlink(status) || !fs::is_directory(status)) {
        return false;
    }
    const auto name = path.filename().native();
    const tstring prefix(kJobPrefix);
    return HasPrefix(name, prefix) && IsHexToken(name.substr(prefix.size()));
}

bool ReadOwnerMarker(const fs::path& path, tstring& token) {
    const fs::path marker = path / kOwnerMarkerName;
    if (!IsRegularFile(marker)) {
        return false;
    }
    std::ifstream input(marker, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if ((!input.good() && !input.eof()) || content.size() != kTokenLength) {
        return false;
    }
    tstring parsed;
    parsed.reserve(content.size());
    for (const auto ch : content) {
        parsed.push_back((TCHAR)(unsigned char)ch);
    }
    if (!IsHexToken(parsed)) {
        return false;
    }
    token = std::move(parsed);
    return true;
}

bool WriteOwnerMarker(const fs::path& path, const tstring& token) {
    const fs::path marker = path / kOwnerMarkerName;
    const std::string content = tchar_to_string(token);
#if defined(_WIN32) || defined(_WIN64)
    const HANDLE handle = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(handle, content.data(), (DWORD)content.size(), &written, nullptr)
        && written == content.size();
    CloseHandle(handle);
    return ok;
#else
    const int fd = open(marker.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return false;
    size_t offset = 0;
    while (offset < content.size()) {
        const ssize_t written = write(fd, content.data() + offset, content.size() - offset);
        if (written <= 0) {
            close(fd);
            return false;
        }
        offset += (size_t)written;
    }
    return close(fd) == 0;
#endif
}

bool IsExpectedJobFileName(const tstring& name) {
    const auto numbered = [&](const tstring& prefix, const tstring& suffix) {
        if (!HasPrefix(name, prefix) || name.size() <= prefix.size() + suffix.size()
            || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
            return false;
        }
        for (size_t i = prefix.size(); i < name.size() - suffix.size(); i++) {
            if (name[i] < _T('0') || name[i] > _T('9')) return false;
        }
        return true;
    };
    const auto artifactTemp = [&](const tstring& artifactName) {
        const tstring prefix = artifactName + _T(".tmp.");
        if (!HasPrefix(name, prefix)) return false;
        size_t index = prefix.size();
        for (int part = 0; part < 3; part++) {
            const size_t begin = index;
            while (index < name.size() && name[index] >= _T('0') && name[index] <= _T('9')) index++;
            if (index == begin) return false;
            if (part < 2) {
                if (index >= name.size() || name[index++] != _T('.')) return false;
            }
        }
        return index == name.size();
    };
    if (name == kOwnerMarkerName || name == _T("audio.dat") || name == _T("audio.wav")
        || name == _T("streaminfo.dat") || name == _T("resume.dat") || name == _T("tsreadex_dump.txt")
        || name == _T("pass0.amts") || name == _T("pass0.trim.avs") || name == _T("pass0.ready")
        || artifactTemp(_T("pass0.amts")) || artifactTemp(_T("pass0.trim.avs"))
        || artifactTemp(_T("pass0.ready")) || numbered(_T("i"), _T(".mpg"))
        || numbered(_T("amts"), _T(".dat")) || numbered(_T("amts"), _T(".avs"))
        || numbered(_T("amts"), _T("_8bit.avs"))
        || numbered(_T("logof"), _T(".txt")) || numbered(_T("chapter_exe"), _T(".txt"))
        || numbered(_T("chapter_exe_o"), _T(".txt")) || numbered(_T("trim"), _T(".avs"))
        || numbered(_T("jls"), _T(".txt")) || numbered(_T("div"), _T(".txt"))) {
        return true;
    }
    return false;
}

bool HasOnlyExpectedJobContents(const fs::path& path) {
    std::error_code ec;
    for (fs::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)) {
        const auto status = it->symlink_status(ec);
        if (ec || fs::is_symlink(status) || !fs::is_regular_file(status)
            || !IsExpectedJobFileName(it->path().filename().native())) {
            return false;
        }
    }
    return !ec;
}

bool IsOwnedJob(const fs::path& path, const tstring& expectedToken) {
    tstring markerToken;
    return IsJobDirectory(path) && IsHexToken(expectedToken) && ReadOwnerMarker(path, markerToken)
        && markerToken == expectedToken && HasOnlyExpectedJobContents(path);
}

void CleanupCreationCandidate(const fs::path& path, const tstring& token) {
    tstring markerToken;
    std::error_code ec;
    if (IsJobDirectory(path) && ReadOwnerMarker(path, markerToken) && markerToken == token) {
        fs::remove(path / kOwnerMarkerName, ec);
    }
    ec.clear();
    // 作成途中の候補は空の場合だけ除去する。外部内容を再帰削除してはならない。
    fs::remove(path, ec);
}

bool RemoveOwnedJob(const fs::path& path, const tstring& token, std::error_code& ec) {
    if (!IsOwnedJob(path, token)) {
        return false;
    }
    // marker確認後の差し替えを縮めるため、削除直前にリンク・所有者・内容を再確認する。
    if (!IsOwnedJob(path, token)) {
        return false;
    }
    fs::remove_all(path, ec);
    return !ec;
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
        if (!HasPrefix(name, kJobPrefix) || fs::is_symlink(status) || !fs::is_directory(status)) {
            continue;
        }
        const auto modified = entry.last_write_time(ec);
        if (ec) {
            break;
        }
        if (modified >= cutoff) {
            continue;
        }
        tstring token;
        if (!ReadOwnerMarker(entry.path(), token) || !IsHexToken(token)) {
            continue;
        }
        ec.clear();
        if (!RemoveOwnedJob(entry.path(), token, ec)) {
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
    : path_(std::move(other.path_)), token_(std::move(other.token_)), owns_(other.owns_) {
    other.owns_ = false;
}

OwnedJobDirectory& OwnedJobDirectory::operator=(OwnedJobDirectory&& other) noexcept {
    if (this != &other) {
        Cleanup();
        path_ = std::move(other.path_);
        token_ = std::move(other.token_);
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
    for (uint64_t index = 0; index < 128; index++) {
        const tstring directoryToken = MakeRandomToken();
        const tstring ownerToken = MakeRandomToken();
        const fs::path candidate = baseDirectory / (tstring(kJobPrefix) + directoryToken);
        ec.clear();
        if (fs::create_directory(candidate, ec)) {
#if !defined(_WIN32) && !defined(_WIN64)
            // 共有WorkPathでも別ユーザーからowner markerを読めないようにする。
            fs::permissions(candidate, fs::perms::owner_all, fs::perm_options::replace, ec);
            if (ec) {
                CleanupCreationCandidate(candidate, ownerToken);
                continue;
            }
#endif
            if (WriteOwnerMarker(candidate, ownerToken) && IsOwnedJob(candidate, ownerToken)) {
                return OwnedJobDirectory(candidate, ownerToken);
            }
            CleanupCreationCandidate(candidate, ownerToken);
            continue;
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
    if (!RemoveOwnedJob(path_, token_, ec)) {
        if (error) {
            *error = _T("pass0一時ディレクトリの所有権または内容を確認できないため削除しません");
        }
        return false;
    }
    owns_ = false;
    return true;
}

} // namespace genlogo::pass0
