/**
* Amtasukaze Avisynth Source Plugin
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include <algorithm>
#include "StringUtils.h"

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

// null終端があるので
/* static */ std::vector<char> string_internal::to_string(std::wstring str, uint32_t codepage) {
    if (str.size() == 0) {
        return std::vector<char>(1);
    }
    int dstlen = WideCharToMultiByte(
        codepage, 0, str.c_str(), (int)str.size(), NULL, 0, NULL, NULL);
    std::vector<char> ret(dstlen + 1);
    WideCharToMultiByte(codepage, 0,
        str.c_str(), (int)str.size(), ret.data(), (int)ret.size(), NULL, NULL);
    ret.back() = 0; // null terminate
    return ret;
}
/* static */ std::vector<wchar_t> string_internal::to_wstring(std::string str, uint32_t codepage) {
    if (str.size() == 0) {
        return std::vector<wchar_t>(1);
    }
    int dstlen = MultiByteToWideChar(
        codepage, 0, str.c_str(), (int)str.size(), NULL, 0);
    std::vector<wchar_t> ret(dstlen + 1);
    MultiByteToWideChar(codepage, 0,
        str.c_str(), (int)str.size(), ret.data(), (int)ret.size());
    ret.back() = 0; // null terminate
    return ret;
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

/* static */ std::string str_replace(std::string str, const std::string& from, const std::string& to) {
    std::string::size_type pos = 0;
    while (pos = str.find(from, pos), pos != std::string::npos) {
        str.replace(pos, from.length(), to);
        pos += to.length();
    }
    return str;
}

/* static */ std::wstring str_replace(std::wstring str, const std::wstring& from, const std::wstring& to) {
    std::wstring::size_type pos = 0;
    while (pos = str.find(from, pos), pos != std::wstring::npos) {
        str.replace(pos, from.length(), to);
        pos += to.length();
    }
    return str;
}

/* static */ std::string to_string(const std::wstring& str, uint32_t codepage) {
    std::vector<char> ret = string_internal::to_string(str, codepage);
    return std::string(ret.begin(), ret.end());
}

/* static */ std::string to_string(const std::string& str, uint32_t codepage) {
    return str;
}

/* static */ std::wstring to_wstring(const std::wstring& str, uint32_t codepage) {
    return str;
}

/* static */ std::wstring to_wstring(const std::string& str, uint32_t codepage) {
    std::vector<wchar_t> ret = string_internal::to_wstring(str, codepage);
    return std::wstring(ret.begin(), ret.end());

#ifdef _MSC_VER
}/* static */ std::wstring to_tstring(const std::wstring& str, uint32_t codepage) {
    return str;
}

/* static */ std::wstring to_tstring(const std::string& str, uint32_t codepage) {
    return to_wstring(str, codepage);
}
#else
/* static */ std::string to_tstring(const std::wstring& str, uint32_t codepage) {
    return to_string(str);
}

/* static */ std::string to_tstring(const std::string& str, uint32_t codepage) {
    return str;
}
#endif

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
    for (int i = searchIdx; i < buffer.size(); ++i) {
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
    int dstlen = MultiByteToWideChar(
        CP_UTF8, 0, (const char*)ptr, sz, nullptr, 0);
    std::vector<wchar_t> w(dstlen);
    MultiByteToWideChar(
        CP_UTF8, 0, (const char*)ptr, sz, w.data(), (int)w.size());
    dstlen = WideCharToMultiByte(
        CP_ACP, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::vector<char> ret(dstlen);
    WideCharToMultiByte(CP_ACP, 0,
        w.data(), (int)w.size(), ret.data(), (int)ret.size(), nullptr, nullptr);
    return ret;
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
            if (_wcsicmp(c_path + (path.size() - extlen), exts[i]) == 0) {
                return path.substr(0, path.size() - extlen);
            }
        }
    }
    return path;
}

/* static */ tstring pathToOS(const tstring& path) {
    tstring ret = path;
    std::replace(ret.begin(), ret.end(), _T('/'), _T('\\'));
    return ret;
}
