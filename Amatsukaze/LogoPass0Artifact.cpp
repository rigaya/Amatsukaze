#include "LogoPass0Artifact.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

#if defined(_WIN32) || defined(_WIN64)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace logopass0 {
namespace {

namespace fs = std::filesystem;

std::atomic<unsigned long long> tempSerial { 0 };

tstring ErrorText(const char* text) {
#if defined(_WIN32) || defined(_WIN64)
    std::wstring result;
    while (*text != '\0') {
        result.push_back((wchar_t)(unsigned char)*text++);
    }
    return result;
#else
    return text;
#endif
}

tstring NumberText(const unsigned long long value) {
#if defined(_WIN32) || defined(_WIN64)
    return std::to_wstring(value);
#else
    return std::to_string(value);
#endif
}

bool SetError(tstring& error, const char* text) {
    error = ErrorText(text);
    return false;
}

tstring DefaultTempPath(const tstring& finalPath, const unsigned int attempt) {
#if defined(_WIN32) || defined(_WIN64)
    const auto pid = (unsigned long long)GetCurrentProcessId();
#else
    const auto pid = (unsigned long long)getpid();
#endif
    return finalPath + _T(".tmp.") + NumberText(pid) + _T(".")
        + NumberText(tempSerial.fetch_add(1)) + _T(".") + NumberText(attempt);
}

FILE* OpenExclusiveTemp(const tstring& path) {
#if defined(_WIN32) || defined(_WIN64)
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return nullptr;
    const int fd = _open_osfhandle((intptr_t)handle, _O_WRONLY);
    if (fd < 0) {
        CloseHandle(handle);
        // CREATE_NEWでこの呼び出し自身が作成した一時ファイルだけを除去する。
        DeleteFileW(path.c_str());
        return nullptr;
    }
    FILE* file = _fdopen(fd, "wb");
    if (file == nullptr) {
        // _open_osfhandle成功後のHANDLEはfdが所有するため、CloseHandleではなく_closeする。
        _close(fd);
        // CREATE_NEWでこの呼び出し自身が作成した一時ファイルだけを除去する。
        DeleteFileW(path.c_str());
    }
    return file;
#else
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
    if (fd < 0) return nullptr;
    FILE* file = fdopen(fd, "wb");
    if (file == nullptr) {
        close(fd);
        // O_EXCLでこの呼び出し自身が作成した一時ファイルだけを除去する。
        unlink(path.c_str());
    }
    return file;
#endif
}

bool RemoveTempFile(const tstring& path) {
    std::error_code ec;
    fs::remove(fs::path(path), ec);
    return !ec;
}

bool IsPlainBaseName(const fs::path& path) {
    const auto name = path.filename();
    if (name.empty() || name == fs::path(".") || name == fs::path("..") || !path.parent_path().empty()) {
        return false;
    }
    const auto value = name.native();
    if (value.empty() || value.back() == _T('.') || value.back() == _T(' ')) {
        return false;
    }
    for (const auto ch : value) {
        if (ch < _T(' ') || ch == _T('/') || ch == _T('\\') || ch == _T(':')
            || ch == _T('<') || ch == _T('>') || ch == _T('"') || ch == _T('|')
            || ch == _T('?') || ch == _T('*')) {
            return false;
        }
    }
#if defined(_WIN32) || defined(_WIN64)
    auto stem = value.substr(0, value.find(_T('.')));
    for (auto& ch : stem) {
        if (ch >= _T('a') && ch <= _T('z')) ch = (tchar)(ch - _T('a') + _T('A'));
    }
    const tstring reserved[] = {
        _T("CON"), _T("PRN"), _T("AUX"), _T("NUL"),
        _T("COM1"), _T("COM2"), _T("COM3"), _T("COM4"), _T("COM5"), _T("COM6"), _T("COM7"), _T("COM8"), _T("COM9"),
        _T("LPT1"), _T("LPT2"), _T("LPT3"), _T("LPT4"), _T("LPT5"), _T("LPT6"), _T("LPT7"), _T("LPT8"), _T("LPT9"),
    };
    for (const auto& item : reserved) {
        if (stem == item) return false;
    }
#endif
    return true;
}

bool WriteBytes(FILE* file, const char* data, size_t size, tstring& error) {
    if (size > 0 && fwrite(data, 1, size, file) != size) {
        return SetError(error, "pass0成果物の一時ファイル書き込みに失敗しました");
    }
    return true;
}

} // namespace

bool ResolveArtifactPaths(const tstring& resumeDir, const tstring& requestedBase,
    ArtifactPaths& paths, tstring& error) {
    paths = ArtifactPaths {};
    error.clear();
    if (resumeDir.empty() || requestedBase.empty()) {
        return SetError(error, "resume-dirとpass0成果物名が必要です");
    }
    std::error_code ec;
    const fs::path resumeInput(resumeDir);
    if (!fs::is_directory(resumeInput, ec) || ec) {
        return SetError(error, "resume-dirが存在するディレクトリではありません");
    }
    const auto canonicalResume = fs::canonical(resumeInput, ec);
    if (ec) {
        return SetError(error, "resume-dirを正規化できません");
    }

    const fs::path request(requestedBase);
    fs::path base;
    if (request.is_relative()) {
        if (!IsPlainBaseName(request)) {
            return SetError(error, "相対のpass0成果物名はresume-dir直下の通常ファイル名にしてください");
        }
        base = canonicalResume / request;
    } else {
        if (!IsPlainBaseName(fs::path(request.filename()))) {
            return SetError(error, "pass0成果物名が不正です");
        }
        const auto parent = fs::canonical(request.parent_path(), ec);
        const bool sameParent = !ec && fs::equivalent(parent, canonicalResume, ec);
        if (ec || !sameParent) {
            return SetError(error, "pass0成果物はresume-dir直下にしてください");
        }
        base = parent / request.filename();
    }

    paths.base = base.native();
    paths.amtSource = paths.base + _T(".amts");
    paths.trimAvs = paths.base + _T(".trim.avs");
    paths.ready = paths.base + _T(".ready");
    return true;
}

bool HasExistingArtifact(const ArtifactPaths& paths) {
    std::error_code ec;
    return fs::exists(fs::path(paths.amtSource), ec)
        || fs::exists(fs::path(paths.trimAvs), ec)
        || fs::exists(fs::path(paths.ready), ec);
}

int SelectLargestVideoIndex(const std::vector<int>& frameCounts) {
    int bestIndex = -1;
    int bestFrames = -1;
    for (int index = 0; index < (int)frameCounts.size(); index++) {
        if (frameCounts[index] < 0) continue;
        if (bestIndex < 0 || frameCounts[index] > bestFrames) {
            bestIndex = index;
            bestFrames = frameCounts[index];
        }
    }
    return bestIndex;
}

bool PublishFileAtomically(const tstring& finalPath, const TempFileWriter& writer,
    tstring& error, std::vector<tstring>* publishOrder, const TempPathGenerator& tempPathGenerator) {
    error.clear();
    if (!writer) return SetError(error, "pass0成果物の書き込み処理がありません");
    const auto makeTemp = tempPathGenerator ? tempPathGenerator : DefaultTempPath;
    tstring tempPath;
    FILE* file = nullptr;
    for (unsigned int attempt = 0; attempt < 128; attempt++) {
        tempPath = makeTemp(finalPath, attempt);
        file = OpenExclusiveTemp(tempPath);
        if (file != nullptr) break;
    }
    if (file == nullptr) return SetError(error, "pass0成果物の一時ファイルを排他的に作成できません");

    bool wrote = false;
    try {
        wrote = writer(file, error);
    } catch (...) {
        fclose(file);
        RemoveTempFile(tempPath);
        throw;
    }
    const int flushResult = fflush(file);
    const int closeResult = fclose(file);
    if (flushResult != 0 || closeResult != 0) {
        RemoveTempFile(tempPath);
        return SetError(error, "pass0成果物の一時ファイルを閉じられません");
    }
    if (!wrote) {
        RemoveTempFile(tempPath);
        if (error.empty()) SetError(error, "pass0成果物の一時ファイル書き込みに失敗しました");
        return false;
    }

#if defined(_WIN32) || defined(_WIN64)
    const bool published = MoveFileW(tempPath.c_str(), finalPath.c_str()) != 0;
#else
    const bool published = link(tempPath.c_str(), finalPath.c_str()) == 0;
#endif
    if (!published) {
        RemoveTempFile(tempPath);
        return SetError(error, "pass0成果物の公開先が既に存在するか公開に失敗しました");
    }
#if !defined(_WIN32) && !defined(_WIN64)
    if (!RemoveTempFile(tempPath)) {
        // finalはすでに公開済みだが、不完全扱いとしてready公開を止める。
        return SetError(error, "pass0成果物の公開後に一時ファイルを削除できません");
    }
#endif
    if (publishOrder != nullptr) publishOrder->push_back(finalPath);
    return true;
}

bool CopyFileAtomically(const tstring& sourcePath, const tstring& finalPath,
    tstring& error, std::vector<tstring>* publishOrder) {
    return PublishFileAtomically(finalPath, [&](FILE* output, tstring& writeError) {
        std::ifstream input(fs::path(sourcePath), std::ios::binary);
        if (!input) return SetError(writeError, "pass0成果物のコピー元を開けません");
        char buffer[64 * 1024];
        while (input.read(buffer, sizeof(buffer)) || input.gcount() > 0) {
            if (!WriteBytes(output, buffer, (size_t)input.gcount(), writeError)) return false;
        }
        if (!input.eof()) return SetError(writeError, "pass0成果物のコピー元読み込みに失敗しました");
        return true;
    }, error, publishOrder);
}

bool WriteUtf8TextAtomically(const std::string& text, const tstring& finalPath,
    tstring& error, std::vector<tstring>* publishOrder) {
    return PublishFileAtomically(finalPath, [&](FILE* output, tstring& writeError) {
        return WriteBytes(output, text.data(), text.size(), writeError);
    }, error, publishOrder);
}

bool PublishArtifactsWithWriters(const ArtifactPaths& paths, const TempFileWriter& amtWriter,
    const TempFileWriter& trimWriter, tstring& error, std::vector<tstring>* publishOrder) {
    error.clear();
    if (!PublishFileAtomically(paths.amtSource, amtWriter, error, publishOrder)) return false;
    if (!PublishFileAtomically(paths.trimAvs, trimWriter, error, publishOrder)) return false;
    return WriteUtf8TextAtomically("1\n", paths.ready, error, publishOrder);
}

bool PublishArtifacts(const ArtifactPaths& paths, const tstring& sourceAmtPath,
    const std::string& trimAvsText, tstring& error, std::vector<tstring>* publishOrder) {
    error.clear();
    const auto copyWriter = [&](FILE* output, tstring& writeError) {
        std::ifstream input(fs::path(sourceAmtPath), std::ios::binary);
        if (!input) return SetError(writeError, "pass0成果物のコピー元を開けません");
        char buffer[64 * 1024];
        while (input.read(buffer, sizeof(buffer)) || input.gcount() > 0) {
            if (!WriteBytes(output, buffer, (size_t)input.gcount(), writeError)) return false;
        }
        if (!input.eof()) return SetError(writeError, "pass0成果物のコピー元読み込みに失敗しました");
        return true;
    };
    const auto trimWriter = [&](FILE* output, tstring& writeError) {
        return WriteBytes(output, trimAvsText.data(), trimAvsText.size(), writeError);
    };
    return PublishArtifactsWithWriters(paths, copyWriter, trimWriter, error, publishOrder);
}

} // namespace logopass0
