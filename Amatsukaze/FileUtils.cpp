/**
* Amatsukaze core utility
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include "FileUtils.h"
#include "rgy_osdep.h"
#if defined(_WIN32) || defined(_WIN64)
#include <direct.h>
#endif // #if defined(_WIN32) || defined(_WIN64)
#include "rgy_filesystem.h"


#if (defined(_WIN32) || defined(_WIN64))
DWORD GetFullPathNameT(LPCWSTR lpFileName, DWORD nBufferLength, LPWSTR lpBuffer, LPWSTR* lpFilePart) {
    return GetFullPathNameW(lpFileName, nBufferLength, lpBuffer, lpFilePart);
}

DWORD GetFullPathNameT(LPCSTR lpFileName, DWORD nBufferLength, LPSTR lpBuffer, LPSTR* lpFilePart) {
    return GetFullPathNameA(lpFileName, nBufferLength, lpBuffer, lpFilePart);
}
#else
DWORD GetFullPathNameT(LPCWSTR lpFileName, DWORD nBufferLength, LPWSTR lpBuffer, LPWSTR* lpFilePart) {
    std::wstring fullPath = GetFullPathFrom(lpFileName);
    if (fullPath.length() >= nBufferLength) {
        return static_cast<DWORD>(fullPath.length() + 1); // 必要なバッファサイズを返す
    }
    
    wcscpy(lpBuffer, fullPath.c_str());
    
    if (lpFilePart != nullptr) {
        // ファイル名部分のポインタを設定
        *lpFilePart = lpBuffer + fullPath.rfind(L'/') + 1;
    }
    
    return static_cast<DWORD>(fullPath.length());
}

DWORD GetFullPathNameT(LPCSTR lpFileName, DWORD nBufferLength, LPSTR lpBuffer, LPSTR* lpFilePart) {
    std::string fullPath = GetFullPathFrom(lpFileName);
    if (fullPath.length() >= nBufferLength) {
        return static_cast<DWORD>(fullPath.length() + 1); // 必要なバッファサイズを返す
    }
    
    strcpy(lpBuffer, fullPath.c_str());
    
    if (lpFilePart != nullptr) {
        // ファイル名部分のポインタを設定
        *lpFilePart = lpBuffer + fullPath.rfind('/') + 1;
    }
    
    return static_cast<DWORD>(fullPath.length());
}
#endif

int rmdirT(const wchar_t* dirname) {
    return rgy_directory_remove(dirname);
}
int rmdirT(const char* dirname) {
    return rgy_directory_remove(dirname);
}

int mkdirT(const wchar_t* dirname) {
    return CreateDirectoryRecursive(dirname, true) ? 0 : -1;
}
int mkdirT(const char* dirname) {
    return CreateDirectoryRecursive(dirname, true) ? 0 : -1;
}

int removeT(const wchar_t* dirname) {
    return rgy_directory_remove(dirname);
}
int removeT(const char* dirname) {
    return rgy_directory_remove(dirname);
}

void PrintFileAll(const tstring& path) {
    File file(path, _T("rb"));
    int sz = (int)file.size();
    if (sz == 0) return;
    auto buf = std::unique_ptr<uint8_t[]>(new uint8_t[sz]);
    auto rsz = file.read(MemoryChunk(buf.get(), sz));
    fwrite(buf.get(), 1, strnlen_s((char*)buf.get(), rsz), stderr);
    if (buf[rsz - 1] != '\n') {
        // 改行で終わっていないときは改行する
        fprintf(stderr, "\n");
    }
}

