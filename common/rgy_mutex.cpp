// -----------------------------------------------------------------------------------------
// QSVEnc/NVEnc by rigaya
// -----------------------------------------------------------------------------------------
// The MIT License
//
// Copyright (c) 2011-2016 rigaya
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// --------------------------------------------------------------------------------------------

#include <stdexcept>
#include "rgy_mutex.h"

#if (defined(_WIN32) || defined(_WIN64))
#include "rgy_osdep.h"

RGYMutex::RGYMutex(const std::string& name) :
    m_name(name), m_handle(nullptr) {
    m_handle = CreateMutexA(nullptr, FALSE, m_name.c_str());
    if (!m_handle) {
        throw std::runtime_error("Failed to create or open mutex: " + m_name);
    }
}

RGYMutex::~RGYMutex() {
    if (m_handle) {
        CloseHandle(m_handle);
        m_handle = nullptr;
    }
}

void RGYMutex::lock() {
    DWORD result = WaitForSingleObject(m_handle, INFINITE);
    if (result != WAIT_OBJECT_0) {
        throw std::runtime_error("Failed to lock mutex: " + m_name);
    }
}

void RGYMutex::unlock() {
    ReleaseMutex(m_handle);
}

#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <semaphore.h>

RGYMutex::RGYMutex(const std::string& name) :
    m_name(name), m_handle(nullptr) {
    std::string semName = "/" + m_name;  // sem_open()は'/'が必要
    m_handle = sem_open(semName.c_str(), O_CREAT, 0666, 1);
    if ((sem_t*)m_handle == SEM_FAILED) {
        throw std::runtime_error("Failed to create or open semaphore: " + m_name);
    }
}

RGYMutex::~RGYMutex() {
    if ((sem_t*)m_handle != SEM_FAILED && (sem_t*)m_handle != nullptr) {
        sem_close((sem_t*)m_handle);
        // ※ 他プロセス使用中の可能性があるため sem_unlink は行わない
        m_handle = nullptr;
    }
}

void RGYMutex::lock() {
    if (sem_wait((sem_t*)m_handle) < 0) {
        throw std::runtime_error("Failed to lock semaphore: " + m_name);
    }
}

void RGYMutex::unlock() {
    if (sem_post((sem_t*)m_handle) < 0) {
        throw std::runtime_error("Failed to unlock semaphore: " + m_name);
    }
}

#endif //#if (defined(_WIN32) || defined(_WIN64))

RGYMutex::Guard::Guard(RGYMutex& mtx) : m_mtx(mtx) {
    m_mtx.lock();
}

RGYMutex::Guard::~Guard() {
    m_mtx.unlock();
}
