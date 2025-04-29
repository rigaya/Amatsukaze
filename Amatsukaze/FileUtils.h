/**
* Amatsukaze core utility
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/
#pragma once

#include <memory>
#include "common.h"

#include <string>
#include "rgy_osdep.h"
#include "StringUtils.h"
#include "CoreUtils.hpp"
#include "rgy_filesystem.h"

DWORD GetFullPathNameT(LPCWSTR lpFileName, DWORD nBufferLength, LPWSTR lpBuffer, LPWSTR* lpFilePart);
DWORD GetFullPathNameT(LPCSTR lpFileName, DWORD nBufferLength, LPSTR lpBuffer, LPSTR* lpFilePart);

int rmdirT(const wchar_t* dirname);
int rmdirT(const char* dirname);

int mkdirT(const wchar_t* dirname);
int mkdirT(const char* dirname);

int removeT(const wchar_t* dirname);
int removeT(const char* dirname);

template <typename Char>
std::basic_string<Char> GetFullPath(const std::basic_string<Char>& path) {
    Char buf[AMT_MAX_PATH];
    int sz = GetFullPathNameT(path.c_str(), AMT_MAX_PATH, buf, nullptr);
    if (sz >= AMT_MAX_PATH) {
        THROWF(IOException, "パスが長すぎます: %s", path);
    }
    if (sz == 0) {
        THROWF(IOException, "GetFullPathName()に失敗: %s", path);
    }
    return std::basic_string<Char>(buf);
}

class File : NonCopyable {
public:
    File(const tstring& path, const TCHAR* mode) : path_(path) {
        fp_ = fsopenT(path.c_str(), mode, _SH_DENYNO);
        if (fp_ == NULL) {
            THROWF(IOException, "ファイルを開けません: %s", GetFullPath(path));
        }
    }
    ~File() {
        fclose(fp_);
    }
    void write(MemoryChunk mc) const {
        if (mc.length == 0) return;
        if (fwrite(mc.data, mc.length, 1, fp_) != 1) {
            THROWF(IOException, "failed to write to file: %s", GetFullPath(path_));
        }
    }
    template <typename T>
    void writeValue(T v) const {
        write(MemoryChunk((uint8_t*)&v, sizeof(T)));
    }
    template <typename T>
    void writeArray(const std::vector<T>& arr) const {
        writeValue((int64_t)arr.size());
        auto dataptr = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(arr.data()));
        write(MemoryChunk(dataptr, sizeof(T) * arr.size()));
    }
    void writeString(const std::string& str) const {
        writeValue((int64_t)str.size());
        auto dataptr = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(str.data()));
        write(MemoryChunk(dataptr, sizeof(str[0]) * str.size()));
    }
    size_t read(MemoryChunk mc) const {
        if (mc.length == 0) return 0;
        size_t ret = fread(mc.data, 1, mc.length, fp_);
        if (ret == 0 && feof(fp_)) {
            // ファイル終端
            return 0;
        }
        if (ret <= 0) {
            THROWF(IOException, "failed to read from file: %s", GetFullPath(path_));
        }
        return ret;
    }
    template <typename T>
    T readValue() const {
        T v;
        if (read(MemoryChunk((uint8_t*)&v, sizeof(T))) != sizeof(T)) {
            THROWF(IOException, "failed to read value from file: %s", GetFullPath(path_));
        }
        return v;
    }
    template <typename T>
    std::vector<T> readArray() const {
        size_t len = (size_t)readValue<int64_t>();
        std::vector<T> arr(len);
        if (read(MemoryChunk((uint8_t*)arr.data(), sizeof(T) * len)) != sizeof(T) * len) {
            THROWF(IOException, "failed to read array from file: %s", GetFullPath(path_));
        }
        return arr;
    }
    std::string readString() const {
        auto v = readArray<char>();
        return std::string(v.begin(), v.end());
    }
    void flush() const {
        fflush(fp_);
    }
    void seek(int64_t offset, int origin) const {
        if (_fseeki64(fp_, offset, origin) != 0) {
            THROWF(IOException, "failed to seek file: %s", GetFullPath(path_));
        }
    }
    int64_t pos() const {
        return _ftelli64(fp_);
    }
    int64_t size() const {
        int64_t cur = _ftelli64(fp_);
        if (cur < 0) {
            THROWF(IOException, "_ftelli64 failed: %s", GetFullPath(path_));
        }
        if (_fseeki64(fp_, 0L, SEEK_END) != 0) {
            THROWF(IOException, "failed to seek to end: %s", GetFullPath(path_));
        }
        int64_t last = _ftelli64(fp_);
        if (last < 0) {
            THROWF(IOException, "_ftelli64 failed: %s", GetFullPath(path_));
        }
        _fseeki64(fp_, cur, SEEK_SET);
        if (_fseeki64(fp_, cur, SEEK_SET) != 0) {
            THROWF(IOException, "failed to seek back to current: %s", GetFullPath(path_));
        }
        return last;
    }
    bool getline(std::string& line) {
        enum { BUF_SIZE = 200 };
        char buf[BUF_SIZE];
        line.clear();
        while (1) {
            buf[BUF_SIZE - 2] = 0;
            if (fgets(buf, BUF_SIZE, fp_) == nullptr) {
                return line.size() > 0;
            }
            if (buf[BUF_SIZE - 2] != 0 && buf[BUF_SIZE - 2] != '\n') {
                // まだある
                line.append(buf);
                continue;
            } else {
                // 改行文字を取り除く
                size_t len = strlen(buf);
                if (buf[len - 1] == '\n') buf[--len] = 0;
                if (buf[len - 1] == '\r') buf[--len] = 0;
                line.append(buf);
                break;
            }
        }
        return true;
    }
    void writeline(std::string& line) {
        fputs(line.c_str(), fp_);
        fputs("\n", fp_);
    }
    static bool exists(const tstring& path) {
        FILE* fp_ = fsopenT(path.c_str(), _T("rb"), _SH_DENYNO);
        if (fp_) {
            fclose(fp_);
            return true;
        }
        return false;
    }
    static void copy(const tstring& srcpath, const tstring& dstpath) {
        rgy_file_copy(srcpath, dstpath, true);
    }
private:
    const tstring path_; // エラーメッセージ表示用
    FILE* fp_;
};

template <typename T>
void WriteArray(const File& file, const std::vector<T>& arr) {
    file.writeValue((int)arr.size());
    for (int i = 0; i < (int)arr.size(); ++i) {
        arr[i].Write(file);
    }
}

template <typename T>
std::vector<T> ReadArray(const File& file) {
    int num = file.readValue<int>();
    std::vector<T> ret(num);
    for (int i = 0; i < num; ++i) {
        ret[i] = T::Read(file);
    }
    return ret;
}

template <typename F>
void WriteGrayBitmap(const std::string& path, int w, int h, F pixels) {

    int stride = (3 * w + 3) & ~3;
    auto buf = std::unique_ptr<uint8_t[]>(new uint8_t[h * stride]);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* ptr = &buf[3 * x + (h - y - 1) * stride];
            ptr[0] = ptr[1] = ptr[2] = pixels(x, y);
        }
    }

    BITMAPINFOHEADER bmiHeader = { 0 };
    bmiHeader.biSize = sizeof(bmiHeader);
    bmiHeader.biWidth = w;
    bmiHeader.biHeight = h;
    bmiHeader.biPlanes = 1;
    bmiHeader.biBitCount = 24;
    bmiHeader.biCompression = BI_RGB;
    bmiHeader.biSizeImage = 0;
    bmiHeader.biXPelsPerMeter = 1;
    bmiHeader.biYPelsPerMeter = 1;
    bmiHeader.biClrUsed = 0;
    bmiHeader.biClrImportant = 0;

    BITMAPFILEHEADER bmfHeader = { 0 };
    bmfHeader.bfType = 0x4D42;
    bmfHeader.bfOffBits = sizeof(bmfHeader) + sizeof(bmiHeader);
    bmfHeader.bfSize = bmfHeader.bfOffBits + bmiHeader.biSizeImage;

    File file(path, "wb");
    file.write(MemoryChunk((uint8_t*)&bmfHeader, sizeof(bmfHeader)));
    file.write(MemoryChunk((uint8_t*)&bmiHeader, sizeof(bmiHeader)));
    file.write(MemoryChunk(buf.get(), h * stride));
}

void PrintFileAll(const tstring& path);
