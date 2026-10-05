/**
* Amatsukaze common header
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/
#pragma once

#include <cstdint>
#include <stdio.h>

// Windows関連の定義
#if !defined(AMATSUKAZE_API)
  #if defined(_WIN32) || defined(_WIN64)
    #if defined(AMATSUKAZE_IMPORTS)
      #define AMATSUKAZE_API __declspec(dllimport)
    #else
      #define AMATSUKAZE_API __declspec(dllexport)
    #endif
    #define WINAPI __stdcall
  #else
    #define AMATSUKAZE_API __attribute__((visibility("default")))
    #define WINAPI
  #endif
#endif

#define PRINTF(...) fprintf(stderr, __VA_ARGS__); fflush(stderr)


inline void assertion_failed(const char* line, const char* file, int lineNum) {
    char buf[500];
#if defined(_WIN32) || defined(_WIN64)
    sprintf_s(buf, "Assertion failed!! %s (%s:%d)", line, file, lineNum);
#else
    sprintf(buf, "Assertion failed!! %s (%s:%d)", line, file, lineNum);
#endif
    PRINTF("%s\n", buf);
    //MessageBox(NULL, "Error", "Amatsukaze", MB_OK);
    throw buf;
}

#ifndef _DEBUG
#define ASSERT(exp)
#else
#define ASSERT(exp) do { if(!(exp)) assertion_failed(#exp, __FILE__, __LINE__); } while(0)
#endif

// 最上位の1のビット位置 (0始まり) を返す。maskは0以外であること
inline int bitScanReverse64(uint64_t mask) {
#if defined(_WIN32) || defined(_WIN64)
    unsigned long index;
#ifdef _WIN64
    _BitScanReverse64(&index, mask);
#else
    unsigned long highWord = (unsigned long)(mask >> 32);
    unsigned long lowWord = (unsigned long)mask;
    if (highWord) {
        _BitScanReverse(&index, highWord);
        index += 32;
    } else {
        _BitScanReverse(&index, lowWord);
    }
#endif
    return (int)index;
#else
    return 63 - __builtin_clzll(mask);
#endif
}
