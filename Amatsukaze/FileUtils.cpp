/**
* Amatsukaze core utility
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include "FileUtils.h"
#include "rgy_osdep.h"
#include "rgy_codepage.h"
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>
#if defined(_WIN32) || defined(_WIN64)
#include <direct.h>
#endif // #if defined(_WIN32) || defined(_WIN64)
#include "rgy_filesystem.h"

class ReadAheadFile::Impl : NonCopyable {
public:
    Impl(const tstring& path, size_t bufferSize, size_t bufferCount)
        : file_(path, _T("rb"))
        , fileSize_(file_.size())
        , buffers_(bufferCount)
        , currentBuffer_(NO_BUFFER)
        , finished_(false)
        , stop_(false) {
        if (bufferSize == 0 || bufferCount < 2) {
            THROW(ArgumentException, "先読みバッファの指定が不正です");
        }
        for (size_t i = 0; i < buffers_.size(); i++) {
            buffers_[i].data.resize(bufferSize);
            freeBuffers_.push_back(i);
        }
        thread_ = std::thread([this]() { run(); });
    }

    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
            canRead_.notify_all();
            canConsume_.notify_all();
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    MemoryChunk read() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (currentBuffer_ != NO_BUFFER) {
            freeBuffers_.push_back(currentBuffer_);
            currentBuffer_ = NO_BUFFER;
            canRead_.notify_one();
        }
        canConsume_.wait(lock, [this]() {
            return !readyBuffers_.empty() || finished_ || error_;
        });
        if (!readyBuffers_.empty()) {
            currentBuffer_ = readyBuffers_.front();
            readyBuffers_.pop_front();
            auto& buffer = buffers_[currentBuffer_];
            return MemoryChunk(buffer.data.data(), buffer.length);
        }
        if (error_) {
            std::rethrow_exception(error_);
        }
        return MemoryChunk();
    }

    int64_t size() const {
        return fileSize_;
    }

private:
    struct Buffer {
        std::vector<uint8_t> data;
        size_t length = 0;
    };

    static constexpr size_t NO_BUFFER = static_cast<size_t>(-1);

    File file_;
    int64_t fileSize_;
    std::vector<Buffer> buffers_;
    std::deque<size_t> freeBuffers_;
    std::deque<size_t> readyBuffers_;
    size_t currentBuffer_;
    bool finished_;
    bool stop_;
    std::exception_ptr error_;
    std::mutex mutex_;
    std::condition_variable canRead_;
    std::condition_variable canConsume_;
    std::thread thread_;

    void run() {
        try {
            while (true) {
                size_t index;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    canRead_.wait(lock, [this]() { return stop_ || !freeBuffers_.empty(); });
                    if (stop_) return;
                    index = freeBuffers_.front();
                    freeBuffers_.pop_front();
                }

                auto& buffer = buffers_[index];
                const size_t length = file_.read(MemoryChunk(buffer.data.data(), buffer.data.size()));

                std::lock_guard<std::mutex> lock(mutex_);
                if (stop_) return;
                if (length > 0) {
                    buffer.length = length;
                    readyBuffers_.push_back(index);
                } else {
                    freeBuffers_.push_back(index);
                }
                if (length < buffer.data.size()) {
                    finished_ = true;
                }
                canConsume_.notify_one();
                if (finished_) return;
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            error_ = std::current_exception();
            finished_ = true;
            canConsume_.notify_all();
        }
    }
};

ReadAheadFile::ReadAheadFile(const tstring& path, size_t bufferSize, size_t bufferCount)
    : impl_(new Impl(path, bufferSize, bufferCount)) {}

ReadAheadFile::~ReadAheadFile() = default;

MemoryChunk ReadAheadFile::read() {
    return impl_->read();
}

int64_t ReadAheadFile::size() const {
    return impl_->size();
}


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
    auto buf = std::unique_ptr<uint8_t[]>(new uint8_t[sz + 1]);
    auto rsz = file.read(MemoryChunk(buf.get(), sz));
    buf[rsz] = '\0';
    const auto detectedCp = get_code_page(buf.get(), (uint32_t)rsz);
    const auto inputCp = (detectedCp == CODE_PAGE_UTF8) ? CODE_PAGE_UTF8 : CODE_PAGE_SJIS;
#if defined(_WIN32) || defined(_WIN64)
    const auto text = wstring_to_string(char_to_wstring((char *)buf.get(), inputCp), GetACP());
#else
    const auto text = char_to_string(CODE_PAGE_UTF8, (char *)buf.get(), inputCp);
#endif
    fwrite(text.data(), 1, text.size(), stderr);
    if (buf[rsz - 1] != '\n') {
        // 改行で終わっていないときは改行する
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}

