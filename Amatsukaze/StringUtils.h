#pragma once

/**
* String Utility
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/
#pragma once

#include <string>
#include <cassert>
#include <vector>
#include <direct.h>
#include "CoreUtils.hpp"

#ifdef _MSC_VER
typedef std::wstring tstring;
typedef wchar_t tchar;
#define PRITSTR "ls"
#define _T(s) L ## s
#else
typedef std::string tstring;
typedef char tchar;
#define PRITSTR "s"
#define _T(s) s
#endif

template <typename ... Args>
int sscanfT(const wchar_t* buffer, const wchar_t* format, const Args& ... args) {
    return swscanf_s(buffer, format, args ...);
}
template <typename ... Args>
int sscanfT(const char* buffer, const char* format, const Args& ... args) {
    return sscanf(buffer, format, args ...);
}

size_t strlenT(const wchar_t* string);
size_t strlenT(const char* string);

int stricmpT(const wchar_t* string1, const wchar_t* string2);
int stricmpT(const char* string1, const char* string2);

FILE* fsopenT(const wchar_t* FileName, const wchar_t* Mode, int ShFlag);
FILE* fsopenT(const char* FileName, const char* Mode, int ShFlag);


namespace string_internal {

// null終端があるので
static std::vector<char> to_string(std::wstring str, uint32_t codepage = CP_ACP);
static std::vector<wchar_t> to_wstring(std::string str, uint32_t codepage = CP_ACP);

class MakeArgContext {
    std::vector<std::vector<char>> args;
public:
    template <typename T> const char* arg(const T& value) {
        args.push_back(to_string(value));
        return args.back().data();
    }
};

class MakeArgWContext {
    std::vector<std::vector<wchar_t>> args;
public:
    template <typename T> const wchar_t* arg(const T& value) {
        args.push_back(to_wstring(value));
        return args.back().data();
    }
};

template <typename T> T MakeArg(MakeArgContext& ctx, T value) { return value; }
template <typename T> T MakeArgW(MakeArgWContext& ctx, T value) { return value; }

const char* MakeArg(MakeArgContext& ctx, const char* value);
const char* MakeArg(MakeArgContext& ctx, const wchar_t* value);
const char* MakeArg(MakeArgContext& ctx, const std::string& value);
const char* MakeArg(MakeArgContext& ctx, const std::wstring& value);

const wchar_t* MakeArgW(MakeArgWContext& ctx, const char* value);
const wchar_t* MakeArgW(MakeArgWContext& ctx, const wchar_t* value);
const wchar_t* MakeArgW(MakeArgWContext& ctx, const std::string& value);
const wchar_t* MakeArgW(MakeArgWContext& ctx, const std::wstring& value);

class StringBuilderBase {
public:
    StringBuilderBase();

    MemoryChunk getMC();

    void clear();

protected:
    AutoBuffer buffer;
};
}

std::string str_replace(std::string str, const std::string& from, const std::string& to);

std::wstring str_replace(std::wstring str, const std::wstring& from, const std::wstring& to);

std::string to_string(const std::wstring& str, uint32_t codepage = CP_ACP);

std::string to_string(const std::string& str, uint32_t codepage = CP_ACP);

std::wstring to_wstring(const std::wstring& str, uint32_t codepage = CP_ACP);

std::wstring to_wstring(const std::string& str, uint32_t codepage = CP_ACP);

#ifdef _MSC_VER
std::wstring to_tstring(const std::wstring& str, uint32_t codepage = CP_ACP);

std::wstring to_tstring(const std::string& str, uint32_t codepage = CP_ACP);
#else
std::string to_tstring(const std::wstring& str, uint32_t codepage = CP_ACP);

std::string to_tstring(const std::string& str, uint32_t codepage = CP_ACP);
#endif

template <typename ... Args>
std::string StringFormat(const char* fmt, const Args& ... args) {
    std::string str;
    string_internal::MakeArgContext ctx;
    size_t size = _scprintf(fmt, string_internal::MakeArg(ctx, args) ...);
    if (size > 0) {
        str.reserve(size + 1); // null終端を足す
        str.resize(size);
        snprintf(&str[0], str.size() + 1, fmt, string_internal::MakeArg(ctx, args) ...);
    }
    return str;
}

template <typename ... Args>
std::wstring StringFormat(const wchar_t* fmt, const Args& ... args) {
    std::wstring str;
    string_internal::MakeArgWContext ctx;
    size_t size = _scwprintf(fmt, string_internal::MakeArgW(ctx, args) ...);
    if (size > 0) {
        str.reserve(size + 1); // null終端を足す
        str.resize(size);
        swprintf(&str[0], str.size() + 1, fmt, string_internal::MakeArgW(ctx, args) ...);
    }
    return str;
}

class StringBuilder : public string_internal::StringBuilderBase {
public:
    template <typename ... Args>
    StringBuilder& append(const char* const fmt, Args const & ... args) {
        string_internal::MakeArgContext ctx;
        size_t size = _scprintf(fmt, string_internal::MakeArg(ctx, args) ...);
        if (size > 0) {
            auto mc = buffer.space((int)((size + 1) * sizeof(char))); // null終端を足す
            snprintf(reinterpret_cast<char*>(mc.data), mc.length / sizeof(char),
                fmt, string_internal::MakeArg(ctx, args) ...);
        }
        buffer.extend((int)(size * sizeof(char)));
        return *this;
    }

    std::string str() const;
};

class StringBuilderW : public string_internal::StringBuilderBase {
public:
    template <typename ... Args>
    StringBuilderW& append(const wchar_t* const fmt, Args const & ... args) {
        string_internal::MakeArgWContext ctx;
        size_t size = _scwprintf(fmt, string_internal::MakeArgW(ctx, args) ...);
        if (size > 0) {
            auto mc = buffer.space((int)((size + 1) * sizeof(wchar_t))); // null終端を足す
            swprintf(reinterpret_cast<wchar_t*>(mc.data), mc.length / sizeof(wchar_t),
                fmt, string_internal::MakeArgW(ctx, args) ...);
        }
        buffer.extend((int)(size * sizeof(wchar_t)));
        return *this;
    }

    std::wstring str() const;
};

#ifdef _MSC_VER
typedef StringBuilderW StringBuilderT;
#else
typedef StringBuilder StringBuilderT;
#endif

class StringLiner {
public:
    StringLiner();

    void AddBytes(MemoryChunk utf8);

    void Flush();

protected:
    AutoBuffer buffer;
    int searchIdx;

    virtual void OnTextLine(const uint8_t* ptr, int len, int brlen) = 0;

    bool SearchLineBreak();
};

std::vector<char> utf8ToString(const uint8_t* ptr, int sz);

template <typename tchar>
std::vector<std::basic_string<tchar>> split(const std::basic_string<tchar>& text, const tchar* delimiters) {
    std::vector<std::basic_string<tchar>> ret;
    std::vector<tchar> text_(text.begin(), text.end());
    text_.push_back(0); // null terminate
    char* ctx;
    ret.emplace_back(strtok_s(text_.data(), delimiters, &ctx));
    while (1) {
        const char* tp = strtok_s(NULL, delimiters, &ctx);
        if (tp == nullptr) break;
        ret.emplace_back(tp);
    }
    return ret;
}

bool starts_with(const std::wstring& str, const std::wstring& test);
bool starts_with(const std::string& str, const std::string& test);

bool ends_with(const tstring & value, const tstring & ending);

tstring pathNormalize(tstring path);

template <typename STR>
static size_t pathGetExtensionSplitPos(const STR& path) {
    size_t lastsplit = path.rfind(_T('/'));
    size_t namebegin = (lastsplit == STR::npos)
        ? 0
        : lastsplit + 1;
    size_t dotpos = path.rfind(_T('.'));
    size_t len = (dotpos == STR::npos || dotpos < namebegin)
        ? path.size()
        : dotpos;
    return len;
}

tstring pathGetDirectory(const tstring& path);

tstring pathRemoveExtension(const tstring& path);

tstring pathToOS(const tstring& path);

