/**
* Amtasukaze Avisynth Source Plugin
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include <deque>
#include "InterProcessComm.h"
#include "PerformanceUtil.h"
#include "rgy_util.h"

/* static */ std::string toJsonString(const tstring& str) {
    if (str.size() == 0) {
        return std::string();
    }
    auto utf8str = wstring_to_string(tchar_to_wstring(str), CP_UTF8);
    std::vector<char> utf8(utf8str.length() + 1, 0);
    utf8.resize(utf8str.length());
    memcpy(utf8.data(), utf8str.data(), utf8str.length());
    std::vector<char> ret;
    for (char c : utf8) {
        switch (c) {
        case '\"':
            ret.push_back('\\');
            ret.push_back('\"');
            break;
        case '\\':
            ret.push_back('\\');
            ret.push_back('\\');
            break;
        case '/':
            ret.push_back('\\');
            ret.push_back('/');
            break;
        case '\b':
            ret.push_back('\\');
            ret.push_back('b');
            break;
        case '\f':
            ret.push_back('\\');
            ret.push_back('f');
            break;
        case '\n':
            ret.push_back('\\');
            ret.push_back('n');
            break;
        case '\r':
            ret.push_back('\\');
            ret.push_back('r');
            break;
        case '\t':
            ret.push_back('\\');
            ret.push_back('t');
            break;
        default:
            ret.push_back(c);
        }
    }
    return std::string(ret.begin(), ret.end());
}

bool ResourceAllocation::IsFailed() const {
    return gpuIndex == -1;
}

ResourceManger::ResourceManger(AMTContext& ctx, pipe_handle_t inPipe, pipe_handle_t outPipe)
        : AMTObject(ctx)
#if defined(_WIN32) || defined(_WIN64)
        , inPipe(inPipe)
        , outPipe(outPipe)
#else
        , inPipe((int)(intptr_t)inPipe)
        , outPipe((int)(intptr_t)outPipe)
#endif
{ };


ResourceManger::~ResourceManger() {
#if defined(_WIN32) || defined(_WIN64)
    if (inPipe != INVALID_HANDLE_VALUE) CloseHandle(inPipe);
    if (outPipe != INVALID_HANDLE_VALUE) CloseHandle(outPipe);
#else
    if (inPipe >= 0) close(inPipe);
    if (outPipe >= 0) close(outPipe);
#endif
}

bool ResourceManger::isValid() const {
#if defined(_WIN32) || defined(_WIN64)
    return inPipe != INVALID_HANDLE_VALUE && outPipe != INVALID_HANDLE_VALUE;
#else
    return inPipe >= 0 && outPipe >= 0;
#endif
}

void ResourceManger::write(MemoryChunk mc) const {
#if defined(_WIN32) || defined(_WIN64)
    DWORD bytesWritten = 0;
    if (WriteFile(outPipe, mc.data, (DWORD)mc.length, &bytesWritten, NULL) == 0) {
        THROW(RuntimeException, "failed to write to stdin pipe");
    }
    if (bytesWritten != mc.length) {
        THROW(RuntimeException, "failed to write to stdin pipe (bytes written mismatch)");
    }
#else
    ssize_t written = 0;
    while (written < (ssize_t)mc.length) {
        ssize_t result = ::write(outPipe, mc.data + written, mc.length - written);
        if (result < 0) {
            THROW(RuntimeException, "failed to write to stdin pipe");
        }
        written += result;
    }
#endif
}

void ResourceManger::read(MemoryChunk mc) const {
#if defined(_WIN32) || defined(_WIN64)
    int offset = 0;
    while (offset < mc.length) {
        DWORD bytesRead = 0;
        if (ReadFile(inPipe, mc.data + offset, (int)mc.length - offset, &bytesRead, NULL) == 0) {
            THROW(RuntimeException, "failed to read from pipe");
        }
        offset += bytesRead;
    }
#else
    ssize_t offset = 0;
    while (offset < (ssize_t)mc.length) {
        ssize_t result = ::read(inPipe, mc.data + offset, mc.length - offset);
        if (result < 0) {
            THROW(RuntimeException, "failed to read from pipe");
        }
        if (result == 0) {
            THROW(RuntimeException, "pipe closed unexpectedly");
        }
        offset += result;
    }
#endif
}

void ResourceManger::writeCommand(int cmd) const {
    write(MemoryChunk((uint8_t*)&cmd, 4));
}

/*
   void write(int cmd, const std::string& json) {
       write(MemoryChunk((uint8_t*)&cmd, 4));
       int sz = (int)json.size();
       write(MemoryChunk((uint8_t*)&sz, 4));
       write(MemoryChunk((uint8_t*)json.data(), sz));
   }
*/

/* static */ ResourceAllocation ResourceManger::DefaultAllocation() {
    ResourceAllocation res = { 0, -1, 0 };
    return res;
}

ResourceAllocation ResourceManger::readCommand(int expected) const {
    int32_t cmd;
    ResourceAllocation res;
    read(MemoryChunk((uint8_t*)&cmd, sizeof(cmd)));
    if (cmd != expected) {
        THROW(RuntimeException, "invalid return command");
    }
    read(MemoryChunk((uint8_t*)&res, sizeof(res)));
    return res;
}

ResourceAllocation ResourceManger::request(PipeCommand phase) const {
#if defined(_WIN32) || defined(_WIN64)
    if (inPipe == INVALID_HANDLE_VALUE) {
#else
    if (inPipe < 0) {
#endif
        return DefaultAllocation();
    }
    writeCommand(phase | HOST_CMD_NoWait);
    return readCommand(phase);
}

// リソ拏ス確保できるまで待つ
ResourceAllocation ResourceManger::wait(PipeCommand phase) const {
#if defined(_WIN32) || defined(_WIN64)
    if (inPipe == INVALID_HANDLE_VALUE) {
#else
    if (inPipe < 0) {
#endif
        return DefaultAllocation();
    }
    ResourceAllocation ret = request(phase);
    if (ret.IsFailed()) {
        writeCommand(phase);
        ctx.progress("リソース待ち ...");
        Stopwatch sw; sw.start();
        ret = readCommand(phase);
        ctx.infoF("リソース待ち %.2f秒", sw.getAndReset());
    }
    return ret;
}
