/**
* Amtasukaze Avisynth Source Plugin
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include <algorithm>
#include "StringUtils.h"
#include "rgy_osdep.h"
#include "rgy_util.h"

size_t strlenT(const wchar_t* string) {
    return wcslen(string);
}
size_t strlenT(const char* string) {
    return strlen(string);
}

int stricmpT(const wchar_t* string1, const wchar_t* string2) {
    return _wcsicmp(string1, string2);
}
int stricmpT(const char* string1, const char* string2) {
    return _stricmp(string1, string2);
}

FILE* fsopenT(const wchar_t* FileName, const wchar_t* Mode, int ShFlag) {
    return _wfsopen(FileName, Mode, ShFlag);
}

FILE* fsopenT(const char* FileName, const char* Mode, int ShFlag) {
    return _fsopen(FileName, Mode, ShFlag);
}
const char* string_internal::MakeArg(MakeArgContext& ctx, const char* value) { return value; }
const char* string_internal::MakeArg(MakeArgContext& ctx, const wchar_t* value) { return ctx.arg(value); }
const char* string_internal::MakeArg(MakeArgContext& ctx, const std::string& value) { return value.c_str(); }
const char* string_internal::MakeArg(MakeArgContext& ctx, const std::wstring& value) { return ctx.arg(value); }

const wchar_t* string_internal::MakeArgW(MakeArgWContext& ctx, const char* value) { return ctx.arg(value); }
const wchar_t* string_internal::MakeArgW(MakeArgWContext& ctx, const wchar_t* value) { return value; }
const wchar_t* string_internal::MakeArgW(MakeArgWContext& ctx, const std::string& value) { return ctx.arg(value); }
const wchar_t* string_internal::MakeArgW(MakeArgWContext& ctx, const std::wstring& value) { return value.c_str(); }
string_internal::StringBuilderBase::StringBuilderBase() {}

MemoryChunk string_internal::StringBuilderBase::getMC() {
    return buffer.get();
}

void string_internal::StringBuilderBase::clear() {
    buffer.clear();
}

std::string StringBuilder::str() const {
    auto mc = buffer.get();
    return std::string(
        reinterpret_cast<const char*>(mc.data),
        reinterpret_cast<const char*>(mc.data + mc.length));
}

std::wstring StringBuilderW::str() const {
    auto mc = buffer.get();
    return std::wstring(
        reinterpret_cast<const wchar_t*>(mc.data),
        reinterpret_cast<const wchar_t*>(mc.data + mc.length));
}
StringLiner::StringLiner() : searchIdx(0) {}

void StringLiner::AddBytes(MemoryChunk utf8) {
    buffer.add(utf8);
    while (SearchLineBreak());
}

void StringLiner::Flush() {
    if (buffer.size() > 0) {
        OnTextLine(buffer.ptr(), (int)buffer.size(), 0);
        buffer.clear();
    }
}

bool StringLiner::SearchLineBreak() {
    const uint8_t* ptr = buffer.ptr();
    for (int i = searchIdx; i < (int)buffer.size(); ++i) {
        if (ptr[i] == '\n') {
            int len = i;
            int brlen = 1;
            if (len > 0 && ptr[len - 1] == '\r') {
                --len; ++brlen;
            }
            OnTextLine(ptr, len, brlen);
            buffer.trimHead(i + 1);
            searchIdx = 0;
            return true;
        }
    }
    searchIdx = (int)buffer.size();
    return false;
}

std::vector<char> utf8ToString(const uint8_t* ptr, int sz) {
    auto w = char_to_wstring(std::string(reinterpret_cast<const char*>(ptr), sz), CP_UTF8);
    auto ret = wstring_to_string(w, CP_ACP);
    std::vector<char> vec(ret.length() + 1, 0);
    vec.resize(ret.length());
    memcpy(vec.data(), ret.data(), ret.length() * sizeof(char));
    return vec;
}

bool starts_with(const std::wstring& str, const std::wstring& test) {
    return str.compare(0, test.size(), test) == 0;
}
bool starts_with(const std::string& str, const std::string& test) {
    return str.compare(0, test.size(), test) == 0;
}

bool ends_with(const tstring & value, const tstring & ending) {
    if (ending.size() > value.size()) return false;
    return std::equal(ending.rbegin(), ending.rend(), value.rbegin());
}

/* static */ tstring pathNormalize(tstring path) {
    if (path.size() != 0) {
        // バックスラッシュはスラッシュに変換
        std::replace(path.begin(), path.end(), _T('\\'), _T('/'));
        // 最後のスラッシュは取る
        if (path.back() == _T('/')) {
            path.pop_back();
        }
    }
    return path;
}

/* static */ tstring pathGetDirectory(const tstring& path) {
    size_t lastsplit = path.rfind(_T('/'));
    size_t namebegin = (lastsplit == tstring::npos)
        ? 0
        : lastsplit;
    return path.substr(0, namebegin);
}

/* static */ tstring pathRemoveExtension(const tstring& path) {
    const tchar* exts[] = { _T(".mp4"), _T(".mkv"), _T(".m2ts"), _T(".ts"), nullptr };
    const tchar* c_path = path.c_str();
    for (int i = 0; exts[i]; ++i) {
        size_t extlen = strlenT(exts[i]);
        if (path.size() > extlen) {
            if (stricmpT(c_path + (path.size() - extlen), exts[i]) == 0) {
                return path.substr(0, path.size() - extlen);
            }
        }
    }
    return path;
}

/* static */ tstring pathToOS(const tstring& path) {
    tstring ret = path;
#if defined(_WIN32) || defined(_WIN64)
    std::replace(ret.begin(), ret.end(), _T('/'), _T('\\'));
#else
    std::replace(ret.begin(), ret.end(), _T('\\'), _T('/'));
#endif
    return ret;
}
