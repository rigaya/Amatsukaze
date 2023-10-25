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
#include <Windows.h>
#include "Shlwapi.h"

#include <vector>
#include <string>

#include "CoreUtils.hpp"

extern HMODULE g_DllHandle;

std::wstring GetModulePath();

std::wstring GetModuleDirectory();

std::wstring SearchExe(const std::wstring& name);

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
std::vector<std::wstring> GetDirectoryFiles(const std::wstring& dirpath, const std::wstring& pattern);

// 現在のスレッドに設定されているコア数を取得
int GetProcessorCount();

