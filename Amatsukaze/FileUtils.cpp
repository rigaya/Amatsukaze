/**
* Amatsukaze core utility
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include "FileUtils.h"
#include <filesystem>

DWORD GetFullPathNameT(LPCWSTR lpFileName, DWORD nBufferLength, LPWSTR lpBuffer, LPWSTR* lpFilePart) {
    return GetFullPathNameW(lpFileName, nBufferLength, lpBuffer, lpFilePart);
}

DWORD GetFullPathNameT(LPCSTR lpFileName, DWORD nBufferLength, LPSTR lpBuffer, LPSTR* lpFilePart) {
    return GetFullPathNameA(lpFileName, nBufferLength, lpBuffer, lpFilePart);
}

int rmdirT(const wchar_t* dirname) {
    try {
        std::filesystem::remove_all(dirname);
    } catch (...) {
        return 1;
    }
    return 0;
}
int rmdirT(const char* dirname) {
    try {
        std::filesystem::remove_all(dirname);
    } catch (...) {
        return 1;
    }
    return 0;
}

int mkdirT(const wchar_t* dirname) {
    return _wmkdir(dirname);
}
int mkdirT(const char* dirname) {
    return _mkdir(dirname);
}

int removeT(const wchar_t* dirname) {
    try {
        std::filesystem::remove_all(dirname);
    } catch (...) {
        return 1;
    }
    return 0;
}
int removeT(const char* dirname) {
    try {
        std::filesystem::remove_all(dirname);
    } catch (...) {
        return 1;
    }
    return 0;
}

void PrintFileAll(const tstring& path) {
    File file(path, _T("rb"));
    int sz = (int)file.size();
    if (sz == 0) return;
    auto buf = std::unique_ptr<uint8_t[]>(new uint8_t[sz]);
    auto rsz = file.read(MemoryChunk(buf.get(), sz));
    fwrite(buf.get(), 1, strnlen_s((char*)buf.get(), rsz), stderr);
    if (buf[rsz - 1] != '\n') {
        // â¸çsÇ≈èIÇÌÇ¡ÇƒÇ¢Ç»Ç¢Ç∆Ç´ÇÕâ¸çsÇ∑ÇÈ
        fprintf(stderr, "\n");
    }
}

