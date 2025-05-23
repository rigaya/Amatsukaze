﻿/**
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
    return get_cpu_info().logical_cores;
}

// Windows/Linux両対応のmkgmtimeラッパー
long long amt_mkgmtime(struct tm* t) {
#if defined(_WIN32) || defined(_WIN64)
    return _mkgmtime(t);
#else
    return timegm(t);
#endif
}
