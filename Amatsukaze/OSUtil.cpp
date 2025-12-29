/**
* Amtasukaze Avisynth Source Plugin
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include "OSUtil.h"
#include "StringUtils.h"
#include "rgy_osdep.h"
#include "rgy_filesystem.h"
#if defined(_WIN32) || defined(_WIN64)
#include "Shlwapi.h"
#endif
#include "cpu_info.h"

tstring GetModulePath() {
    return GetFullPathFrom(getModulePath(g_DllHandle).c_str(), nullptr);
}

tstring GetModuleDirectory() {
    return PathRemoveFileSpecFixed(getModulePath(g_DllHandle)).second;
}

std::wstring SearchExe(const std::wstring& name) {
    if (rgy_file_exists(name)) {
        return name;
    }
    auto ret = find_executable_in_path(name);
    if (ret.size() > 0) {
        return ret;
    }
    return name;
}

std::string SearchExe(const std::string& name) {
    if (rgy_file_exists(name)) {
        return name;
    }
    auto ret = find_executable_in_path(name);
    if (ret.size() > 0) {
        return ret;
    }
    return name;
}

//std::wstring GetDirectoryPath(const std::wstring& name)nin {
//	wchar_t buf[AMT_MAX_PATH] = { 0 };
//	std::copy(name.begin(), name.end(), buf);
//	PathRemoveFileSpecW(buf);
//	return buf;
//}

bool DirectoryExists(const std::wstring& dirName_in) {
    return rgy_directory_exists(dirName_in);
}

// dirpathは 終端\\なし
// patternは "*.*" とか
// ディレクトリ名を含まないファイル名リストが返る
std::vector<std::wstring> GetDirectoryFiles(const std::wstring& dirpath, const std::wstring& pattern) {
#if defined(_WIN32) || defined(_WIN64)
    std::wstring search = dirpath + _T("/") + pattern;
    std::vector<std::wstring> result;
    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileW(search.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) {
            // ファイルが1つもなかった
            return std::vector<std::wstring>();
        }
        THROWF(IOException, "ファイル列挙に失敗: %s", search);
    }
    do {
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // ディレクトリ
        } else {
            // ファイル
            result.push_back(findData.cFileName);
        }
    } while (FindNextFileW(hFind, &findData));
    FindClose(hFind);
    return result;
#else
    return get_file_list_with_filter(dirpath, pattern);
#endif
}

std::vector<std::string> GetDirectoryFiles(const std::string& dirpath, const std::string& pattern) {
    return get_file_list_with_filter(dirpath, pattern);
}

// 現在のスレッドに設定されているコア数を取得
int GetProcessorCount() {
    int cores = get_cpu_info().logical_cores;
#if defined(_WIN32) || defined(_WIN64)
    if (cores == 0) {
        // GetLogicalProcessorInformationEx が利用可能なら、RelationGroup から論理コア数を取得する
        // (SDK / _WIN32_WINNT の定義状況に依存せず動くように、動的ロード + 最低限の構造体で処理する)
        typedef BOOL(WINAPI *GetLogicalProcessorInformationEx_t)(int /*LOGICAL_PROCESSOR_RELATIONSHIP*/, void*, DWORD*);
        const auto hKernel32 = GetModuleHandleW(L"kernel32.dll");
        const auto fp = (hKernel32) ? (GetLogicalProcessorInformationEx_t)GetProcAddress(hKernel32, "GetLogicalProcessorInformationEx") : nullptr;
        if (fp) {
            // RelationGroup = 4 (LOGICAL_PROCESSOR_RELATIONSHIP)
            const int kRelationGroup = 4;
            DWORD bytes = 0;
            // 1回目で必要サイズ取得
            fp(kRelationGroup, nullptr, &bytes);
            if (bytes > 0 && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
                std::vector<uint8_t> buffer(bytes, 0);
                if (fp(kRelationGroup, buffer.data(), &bytes)) {
                    // 互換構造体定義 (RelationGroup のパースに必要な最低限)
                    struct PROCESSOR_GROUP_INFO_COMPAT {
                        BYTE MaximumProcessorCount;
                        BYTE ActiveProcessorCount;
                        BYTE Reserved[38];
                        ULONG_PTR ActiveProcessorMask;
                    };
                    struct GROUP_RELATIONSHIP_COMPAT {
                        WORD MaximumGroupCount;
                        WORD ActiveGroupCount;
                        BYTE Reserved[20];
                        PROCESSOR_GROUP_INFO_COMPAT GroupInfo[1]; // 可変長
                    };
                    struct SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX_COMPAT {
                        DWORD Relationship;
                        DWORD Size;
                        BYTE Data[1]; // 可変長 (union)
                    };

                    int total = 0;
                    DWORD offset = 0;
                    while (offset + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX_COMPAT) <= bytes) {
                        const auto info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX_COMPAT*>(buffer.data() + offset);
                        if (info->Size == 0 || offset + info->Size > bytes) {
                            break;
                        }
                        if ((int)info->Relationship == kRelationGroup) {
                            const auto group = reinterpret_cast<const GROUP_RELATIONSHIP_COMPAT*>(info->Data);
                            for (WORD i = 0; i < group->ActiveGroupCount; i++) {
                                total += (int)group->GroupInfo[i].ActiveProcessorCount;
                            }
                        }
                        offset += info->Size;
                    }
                    if (total > 0) {
                        cores = total;
                    }
                }
            }
        }

        // フォールバック (動的ロード失敗/取得失敗時)
        if (cores == 0) {
            SYSTEM_INFO sysinfo;
            GetSystemInfo(&sysinfo);
            cores = (int)sysinfo.dwNumberOfProcessors;
        }
    }
#endif
    return std::max(cores, 1);
}

// Windows/Linux両対応のmkgmtimeラッパー
long long amt_mkgmtime(struct tm* t) {
#if defined(_WIN32) || defined(_WIN64)
    return _mkgmtime(t);
#else
    return timegm(t);
#endif
}
