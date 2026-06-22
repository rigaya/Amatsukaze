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
#include <cstring>
#include <cstdarg>
#include <cstdlib>
#include <vector>
//#include <io.h>
#if defined(AMT_COREUTILS_USE_EXISTING_TCHAR)
// Some source files intentionally define their own TCHAR before including this file.
#else
#include "rgy_tchar.h"
#endif
#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#endif

#define AMT_MAX_PATH 512

namespace core_utils {
#if defined(_WIN32) || defined(_WIN64)
inline tstring string_to_tstring_cp932(const std::string& str) {
#if defined(_UNICODE)
    if (str.empty()) {
        return tstring();
    }
    const int length = MultiByteToWideChar(932, 0, str.c_str(), -1, nullptr, 0);
    if (length <= 0) {
        return tstring();
    }
    std::vector<wchar_t> buffer(length, 0);
    if (MultiByteToWideChar(932, 0, str.c_str(), -1, buffer.data(), (int)buffer.size()) <= 0) {
        return tstring();
    }
    return tstring(buffer.data());
#else
    return str;
#endif
}
#endif

inline std::string tstring_to_string_acp(const TCHAR* str) {
    if (str == nullptr) {
        return std::string();
    }
#if defined(_WIN32) || defined(_WIN64)
#if defined(_UNICODE)
    const int length = WideCharToMultiByte(GetACP(), 0, str, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return std::string();
    }
    std::vector<char> buffer(length, 0);
    if (WideCharToMultiByte(GetACP(), 0, str, -1, buffer.data(), (int)buffer.size(), nullptr, nullptr) <= 0) {
        return std::string();
    }
    return std::string(buffer.data());
#else
    return std::string(str);
#endif
#else
#if defined(AMT_COREUTILS_USE_EXISTING_TCHAR)
    const auto length = wcstombs(nullptr, str, 0);
    if (length == (size_t)-1) {
        return std::string();
    }
    std::vector<char> buffer(length + 1, 0);
    wcstombs(buffer.data(), str, buffer.size());
    return std::string(buffer.data());
#else
    return std::string(str);
#endif
#endif
}

inline void fputs_t_stderr(const TCHAR* str) {
    const auto text = tstring_to_string_acp(str);
    fwrite(text.data(), 1, text.size(), stderr);
}

inline tstring ascii_to_tstring(const char* str) {
    tstring ret;
    if (str == nullptr) {
        return ret;
    }
    while (*str) {
        ret.push_back((TCHAR)(unsigned char)*str++);
    }
    return ret;
}
}

struct Exception {
    virtual ~Exception() {}
    virtual const TCHAR* message() const {
        return _T("No Message ...");
    };
    virtual void raise() const { throw *this; }
};

#if defined(_WIN32) || defined(_WIN64)
#define DEFINE_EXCEPTION(name) \
	struct name : public Exception { \
		name(const tstring& mes) : mes(mes) { } \
		name(const TCHAR *mes) : mes(mes) { } \
		name(const std::string& mes) : mes(core_utils::string_to_tstring_cp932(mes)) { } \
		virtual const TCHAR* message() const { return mes.c_str(); } \
		virtual void raise() const { throw *this;	} \
	private: \
		tstring mes; \
	};
#else
#define DEFINE_EXCEPTION(name) \
	struct name : public Exception { \
		name(const tstring& mes) : mes(mes) { } \
		name(const TCHAR *mes) : mes(mes) { } \
		virtual const TCHAR* message() const { return mes.c_str(); } \
		virtual void raise() const { throw *this;	} \
	private: \
		tstring mes; \
	};
#endif

DEFINE_EXCEPTION(EOFException)
DEFINE_EXCEPTION(FormatException)
DEFINE_EXCEPTION(InvalidOperationException)
DEFINE_EXCEPTION(ArgumentException)
DEFINE_EXCEPTION(IOException)
DEFINE_EXCEPTION(RuntimeException)
DEFINE_EXCEPTION(NoLogoException)
DEFINE_EXCEPTION(NoDrcsMapException)
DEFINE_EXCEPTION(AviSynthException)
DEFINE_EXCEPTION(TestException)

#undef DEFINE_EXCEPTION

namespace core_utils {
// プラットフォームに応じた区切り文字を定義
#if defined(_WIN32) || defined(_WIN64)
constexpr char PATH_SEPARATOR = '\\';
#else
constexpr char PATH_SEPARATOR = '/';
#endif

constexpr const char* str_end(const char *str) { return *str ? str_end(str + 1) : str; }
constexpr bool str_slant(const char *str) { return *str == PATH_SEPARATOR ? true : (*str ? str_slant(str + 1) : false); }
constexpr const char* r_slant(const char* str) { return *str == PATH_SEPARATOR ? (str + 1) : r_slant(str - 1); }
constexpr const char* file_name(const char* str) { return str_slant(str) ? r_slant(str_end(str)) : str; }
inline tstring file_name_t(const char* str) { return ascii_to_tstring(file_name(str)); }
}

#define __FILENAME__ core_utils::file_name(__FILE__)

#if defined(_WIN32) || defined(_WIN64)
#define THROW(exception, message) \
	throw_exception_(exception(StringFormat(_T("Exception thrown at %s:%d\r\nMessage: " message), core_utils::file_name_t(__FILENAME__).c_str(), __LINE__)))

#define THROWF(exception, fmt, ...) \
	throw_exception_(exception(StringFormat(_T("Exception thrown at %s:%d\r\nMessage: " fmt), core_utils::file_name_t(__FILENAME__).c_str(), __LINE__, __VA_ARGS__)))
#else
#define THROW(exception, message) \
    throw_exception_(exception(StringFormat(_T("Exception thrown at %s:%d\r\nMessage: " message), core_utils::file_name_t(__FILENAME__).c_str(), __LINE__)))

// 可変引数マクロを修正
#define THROWF(exception, fmt, ...) \
    throw_exception_(exception(StringFormat(_T("Exception thrown at %s:%d\r\nMessage: " fmt), core_utils::file_name_t(__FILENAME__).c_str(), __LINE__ __VA_OPT__(,) __VA_ARGS__)))
#endif

static void throw_exception_(const Exception& exc) {
#if defined(_WIN32) || defined(_WIN64)
    core_utils::fputs_t_stderr(_T("AMT [error] "));
    core_utils::fputs_t_stderr(exc.message());
    core_utils::fputs_t_stderr(_T("\n"));
    fflush(stderr);
#elif defined(AMT_COREUTILS_USE_EXISTING_TCHAR)
    fwprintf(stderr, L"AMT [error] %s\n", exc.message());
    fflush(stderr);
#else
    PRINTF("AMT [error] %s\n", exc.message());
#endif
    //MessageBox(NULL, exc.message(), "Amatsukaze Error", MB_OK);
    exc.raise();
}

// コピー禁止オブジェクト
class NonCopyable {
protected:
    NonCopyable() {}
    ~NonCopyable() {} /// protected な非仮想デストラクタ
private:
    NonCopyable(const NonCopyable &) = delete;
    NonCopyable& operator=(const NonCopyable &) = delete;
};

static void DebugPrint(const char* fmt, ...) {
    va_list argp;
    char buf[4096];
    va_start(argp, fmt);
    vsprintf(buf, fmt, argp);
    va_end(argp);
    fprintf(stderr, "%s", buf);
}

/** @brief ポインタとサイズのセット */
struct MemoryChunk {

    MemoryChunk() : data(NULL), length(0) {}
    MemoryChunk(uint8_t* data, size_t length) : data(data), length(length) {}

    // データの中身を比較
    bool operator==(MemoryChunk o) const {
        if (o.length != length) return false;
        return memcmp(data, o.data, length) == 0;
    }
    bool operator!=(MemoryChunk o) const {
        return !operator==(o);
    }

    uint8_t* data;
    size_t length;
};

/** @brief リングバッファではないがtrimHeadとtrimTailが同じくらい高速なバッファ */
class AutoBuffer {
public:
    AutoBuffer()
        : data_(NULL)
        , capacity_(0)
        , head_(0)
        , tail_(0) {}

    ~AutoBuffer() {
        release();
    }

    void add(MemoryChunk mc) {
        ensure(mc.length);
        memcpy(data_ + tail_, mc.data, mc.length);
        tail_ += mc.length;
    }

    void add(uint8_t byte) {
        if (tail_ >= capacity_) {
            ensure(1);
        }
        data_[tail_++] = byte;
    }

    /** @brief 有効なデータサイズ */
    size_t size() const {
        return tail_ - head_;
    }

    uint8_t* ptr() const {
        return &data_[head_];
    }

    /** @brief データへ */
    MemoryChunk get() const {
        return MemoryChunk(&data_[head_], size());
    }

    /** @brief 追加スペース取得 */
    MemoryChunk space(int at_least = 0) {
        if (at_least > 0) {
            ensure(at_least);
        }
        return MemoryChunk(&data_[tail_], capacity_ - tail_);
    }

    /** @brief 尻をsizeだけ後ろにずらす（その分サイズも増える） */
    void extend(int size) {
        ensure(size);
        tail_ += size;
    }

    /** @brief size分だけ頭を削る */
    void trimHead(size_t size) {
        head_ = std::min(head_ + size, tail_);
        if (head_ == tail_) { // 中身がなくなったら位置を初期化しておく
            head_ = tail_ = 0;
        }
    }

    /** @brief size分だけ尻を削る */
    void trimTail(size_t size) {
        if (this->size() < size) {
            tail_ = head_;
        } else {
            tail_ -= size;
        }
    }

    /** @brief メモリは開放しないがデータサイズをゼロにする */
    void clear() {
        head_ = tail_ = 0;
    }

    /** @brief メモリを開放して初期状態にする（再使用可能）*/
    void release() {
        clear();
        if (data_ != NULL) {
            delete[] data_;
            data_ = NULL;
            capacity_ = 0;
        }
    }

private:
    uint8_t* data_;
    size_t capacity_;
    size_t head_;
    size_t tail_;

    size_t nextSize(size_t current) {
        if (current < 256) {
            return 256;
        }
        return current * 3 / 2;
    }

    void ensure(size_t extra) {
        if (tail_ + extra > capacity_) {
            // 足りない
            size_t next = nextSize(tail_ - head_ + extra);
            if (next <= capacity_) {
                // 容量は十分なのでデータを移動してスペースを作る
                memmove(data_, data_ + head_, tail_ - head_);
            } else {
                uint8_t* new_ = new uint8_t[next];
                if (data_ != NULL) {
                    memcpy(new_, data_ + head_, tail_ - head_);
                    delete[] data_;
                }
                data_ = new_;
                capacity_ = next;
            }
            tail_ -= head_;
            head_ = 0;
        }
    }
};
