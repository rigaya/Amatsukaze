#pragma once

/**
* Amtasukaze OS Utility
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/
#pragma once

#include "common.h"
#include "rgy_osdep.h"
#include "rgy_tchar.h"

#include <vector>
#include <string>

#include "CoreUtils.hpp"

extern HMODULE g_DllHandle;

tstring GetModulePath();

tstring GetModuleDirectory();

std::wstring SearchExe(const std::wstring& name);
std::string SearchExe(const std::string& name);

//std::wstring GetDirectoryPath(const std::wstring& name) {
//	wchar_t buf[AMT_MAX_PATH] = { 0 };
//	std::copy(name.begin(), name.end(), buf);
//	PathRemoveFileSpecW(buf);
//	return buf;
//}

bool DirectoryExists(const std::wstring& dirName_in);

// dirpathは 終端\\なし
// patternは "*.*" とか
// ディレクトリ名を含まないファイル名リストが返る
std::vector<std::string> GetDirectoryFiles(const std::string& dirpath, const std::string& pattern);
std::vector<std::wstring> GetDirectoryFiles(const std::wstring& dirpath, const std::wstring& pattern);

// 現在のスレッドに設定されているコア数を取得
int GetProcessorCount();

// Windows/Linux両対応のmkgmtimeラッパー
// tmをUTCとしてtime_tに変換
long long amt_mkgmtime(struct tm* t);

