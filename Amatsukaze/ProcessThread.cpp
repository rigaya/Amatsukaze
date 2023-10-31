/**
* Sub process and thread utility
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include "ProcessThread.h"
#include "rgy_thread_affinity.h"

SubProcess::SubProcess(const tstring& args, const bool disablePowerThrottoling) :
    pi_(PROCESS_INFORMATION()),
    stdErrPipe_(),
    stdOutPipe_(),
    stdInPipe_(),
    exitCode_(0),
    thSetPowerThrottling_(),
    heSetPowerThrottlingAbort_(NULL) {
    STARTUPINFOW si = STARTUPINFOW();

    si.cb = sizeof(si);
    si.hStdError = stdErrPipe_.writeHandle;
    si.hStdOutput = stdOutPipe_.writeHandle;
    si.hStdInput = stdInPipe_.readHandle;
    si.dwFlags |= STARTF_USESTDHANDLES;

    // 必要ないハンドルは継承を無効化
    if (SetHandleInformation(stdErrPipe_.readHandle, HANDLE_FLAG_INHERIT, 0) == 0 ||
        SetHandleInformation(stdOutPipe_.readHandle, HANDLE_FLAG_INHERIT, 0) == 0 ||
        SetHandleInformation(stdInPipe_.writeHandle, HANDLE_FLAG_INHERIT, 0) == 0) {
        THROW(RuntimeException, "failed to set handle information");
    }

    // Priority Classは明示していないので、デフォルト動作となる
    // デフォルト動作は、親プロセス（このプロセス）がNORMAL_PRIORITY_CLASS以上で
    // 動いている場合は、NORMAL_PRIORITY_CLASS
    // IDLE_PRIORITY_CLASSおよびBELOW_NORMAL_PRIORITY_CLASSで
    // 動いている場合は、そのPriority Classが継承される
    // Priority Classは子プロセスに継承される対象ではないが、
    // NORMAL_PRIORITY_CLASS以下では実質継承されることに注意

    if (CreateProcessW(NULL, const_cast<tchar*>(args.c_str()), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi_) == 0) {
        THROW(RuntimeException, "プロセス起動に失敗。exeのパスを確認してください。");
    }

    // 子プロセス用のハンドルは必要ないので閉じる
    stdErrPipe_.closeWrite();
    stdOutPipe_.closeWrite();
    stdInPipe_.closeRead();

    if (disablePowerThrottoling) {
        runSetPowerThrottling();
    }
}
SubProcess::~SubProcess() {
    join();
}
void SubProcess::write(MemoryChunk mc) {
    if (mc.length > 0xFFFFFFFF) {
        THROW(RuntimeException, "buffer too large");
    }
    DWORD bytesWritten = 0;
    if (WriteFile(stdInPipe_.writeHandle, mc.data, (DWORD)mc.length, &bytesWritten, NULL) == 0) {
        THROW(RuntimeException, "failed to write to stdin pipe");
    }
    if (bytesWritten != mc.length) {
        THROW(RuntimeException, "failed to write to stdin pipe (bytes written mismatch)");
    }
}
size_t SubProcess::readErr(MemoryChunk mc) {
    return readGeneric(mc, stdErrPipe_.readHandle);
}
size_t SubProcess::readOut(MemoryChunk mc) {
    return readGeneric(mc, stdOutPipe_.readHandle);
}
void SubProcess::finishWrite() {
    stdInPipe_.closeWrite();
}

void SubProcess::runSetPowerThrottling() {
    heSetPowerThrottlingAbort_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (heSetPowerThrottlingAbort_ == NULL) {
        return;
    }
    thSetPowerThrottling_ = std::thread([&]() {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        while (WaitForSingleObject(heSetPowerThrottlingAbort_, 5000) == WAIT_TIMEOUT) {
            if (pi_.hProcess) {
                SetThreadPowerThrottolingModeForModule(pi_.dwProcessId, nullptr, RGYThreadPowerThrottlingMode::Disabled);
            }
        }
    });
}
int SubProcess::join() {
    if (pi_.hProcess != NULL) {
        if (thSetPowerThrottling_.joinable()) {
            SetEvent(heSetPowerThrottlingAbort_);
            thSetPowerThrottling_.join();
            CloseHandle(heSetPowerThrottlingAbort_);
            heSetPowerThrottlingAbort_ = NULL;
        }
        // 子プロセスの終了を待つ
        WaitForSingleObject(pi_.hProcess, INFINITE);
        // 終了コード取得
        GetExitCodeProcess(pi_.hProcess, &exitCode_);

        CloseHandle(pi_.hProcess);
        CloseHandle(pi_.hThread);
        pi_.hProcess = NULL;
    }
    return exitCode_;
}

SubProcess::Pipe::Pipe() {
    // 継承を有効にして作成
    SECURITY_ATTRIBUTES sa = SECURITY_ATTRIBUTES();
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;
    if (CreatePipe(&readHandle, &writeHandle, &sa, 0) == 0) {
        THROW(RuntimeException, "failed to create pipe");
    }
}
SubProcess::Pipe::~Pipe() {
    closeRead();
    closeWrite();
}
void SubProcess::Pipe::closeRead() {
    if (readHandle != NULL) {
        CloseHandle(readHandle);
        readHandle = NULL;
    }
}
void SubProcess::Pipe::closeWrite() {
    if (writeHandle != NULL) {
        CloseHandle(writeHandle);
        writeHandle = NULL;
    }
}

size_t SubProcess::readGeneric(MemoryChunk mc, HANDLE readHandle) {
    if (mc.length > 0xFFFFFFFF) {
        THROW(RuntimeException, "buffer too large");
    }
    DWORD bytesRead = 0;
    while (true) {
        if (ReadFile(readHandle, mc.data, (DWORD)mc.length, &bytesRead, NULL) == 0) {
            if (GetLastError() == ERROR_BROKEN_PIPE) {
                return 0;
            }
            THROW(RuntimeException, "failed to read from pipe");
        }
        // パイプはWriteFileにゼロを渡すとReadFileがゼロで帰ってくるのでチェック
        if (bytesRead != 0) {
            break;
        }
    }
    return bytesRead;
}

EventBaseSubProcess::EventBaseSubProcess(const tstring& args, const bool disablePowerThrottoling)
    : SubProcess(args, disablePowerThrottoling)
    , drainOut(this, false)
    , drainErr(this, true) {
    drainOut.start();
    drainErr.start();
}
EventBaseSubProcess::~EventBaseSubProcess() {
    if (drainOut.isRunning()) {
        THROW(InvalidOperationException, "call join before destroy object ...");
    }
}
int EventBaseSubProcess::join() {
    /*
    * 終了処理の流れ
    * finishWrite()
    * -> 子プロセスが終了検知
    * -> 子プロセスが終了
    * -> stdout,stderrの書き込みハンドルが自動的に閉じる
    * -> SubProcess.readGeneric()がEOFExceptionを返す
    * -> DrainThreadが例外をキャッチして終了
    * -> DrainThreadのjoin()が完了
    * -> EventBaseSubProcessのjoin()が完了
    * -> プロセスは終了しているのでSubProcessのデストラクタはすぐに完了
    */
    try {
        finishWrite();
    } catch (RuntimeException&) {
        // 子プロセスがエラー終了していると書き込みに失敗するが無視する
    }
    drainOut.join();
    drainErr.join();
    return SubProcess::join();
}
bool EventBaseSubProcess::isRunning() { return drainOut.isRunning(); }

EventBaseSubProcess::DrainThread::DrainThread(EventBaseSubProcess* this_, bool isErr)
    : this_(this_)
    , isErr_(isErr) {}
void EventBaseSubProcess::DrainThread::run() {
    this_->drain_thread(isErr_);
}

void EventBaseSubProcess::drain_thread(bool isErr) {
    std::vector<uint8_t> buffer(4 * 1024);
    MemoryChunk mc(buffer.data(), buffer.size());
    while (true) {
        size_t bytesRead = isErr ? readErr(mc) : readOut(mc);
        if (bytesRead == 0) { // 終了
            break;
        }
        onOut(isErr, MemoryChunk(mc.data, bytesRead));
    }
}

StdRedirectedSubProcess::StdRedirectedSubProcess(const tstring& args, const int bufferLines, const bool isUtf8, const bool disablePowerThrottoling)
    : EventBaseSubProcess(args, disablePowerThrottoling)
    , bufferLines(bufferLines)
    , isUtf8(isUtf8)
    , outLiner(this, false)
    , errLiner(this, true) {}

StdRedirectedSubProcess::~StdRedirectedSubProcess() {
    if (isUtf8) {
        outLiner.Flush();
        errLiner.Flush();
    }
}

const std::deque<std::vector<char>>& StdRedirectedSubProcess::getLastLines() {
    outLiner.Flush();
    errLiner.Flush();
    return lastLines;
}

StdRedirectedSubProcess::SpStringLiner::SpStringLiner(StdRedirectedSubProcess* pThis, bool isErr)
    : pThis(pThis), isErr(isErr) {}

void StdRedirectedSubProcess::SpStringLiner::OnTextLine(const uint8_t* ptr, int len, int brlen) {
    pThis->onTextLine(isErr, ptr, len, brlen);
}


void StdRedirectedSubProcess::onTextLine(bool isErr, const uint8_t* ptr, int len, int brlen) {

    std::vector<char> line;
    if (isUtf8) {
        line = utf8ToString(ptr, len);
        // 変換する場合はここで出力
        fwrite(line.data(), line.size(), 1, SUBPROC_OUT);
        fprintf(SUBPROC_OUT, "\n");
        fflush(SUBPROC_OUT);
    } else {
        line = std::vector<char>(ptr, ptr + len);
    }

    if (bufferLines > 0) {
        std::lock_guard<std::mutex> lock(mtx);
        if (lastLines.size() > bufferLines) {
            lastLines.pop_front();
        }
        lastLines.push_back(line);
    }
}

void StdRedirectedSubProcess::onOut(bool isErr, MemoryChunk mc) {
    if (bufferLines > 0 || isUtf8) { // 必要がある場合のみ
        (isErr ? errLiner : outLiner).AddBytes(mc);
    }
    if (!isUtf8) {
        // 変換しない場合はここですぐに出力
        fwrite(mc.data, mc.length, 1, SUBPROC_OUT);
        fflush(SUBPROC_OUT);
    }
}


CPUInfo::CPUInfo() {
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationAll, nullptr, &length);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        THROW(RuntimeException, "GetLogicalProcessorInformationExがERROR_INSUFFICIENT_BUFFERを返さなかった");
    }
    std::unique_ptr<uint8_t[]> buf = std::unique_ptr<uint8_t[]>(new uint8_t[length]);
    uint8_t* ptr = buf.get();
    uint8_t* end = ptr + length;
    if (GetLogicalProcessorInformationEx(
        RelationAll, (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)ptr, &length) == 0) {
        THROW(RuntimeException, "GetLogicalProcessorInformationExに失敗");
    }
    while (ptr < end) {
        auto info = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)ptr;
        switch (info->Relationship) {
        case RelationCache:
            // 必要なのはL2,L3のみ
            if (info->Cache.Level == 2 || info->Cache.Level == 3) {
                data[(info->Cache.Level == 2) ? PROC_TAG_L2 : PROC_TAG_L3].push_back(info->Cache.GroupMask);
            }
            break;
        case RelationGroup:
            for (int i = 0; i < info->Group.ActiveGroupCount; ++i) {
                GROUP_AFFINITY af = GROUP_AFFINITY();
                af.Group = i;
                af.Mask = info->Group.GroupInfo[i].ActiveProcessorMask;
                data[PROC_TAG_GROUP].push_back(af);
            }
            break;
        case RelationNumaNode:
            data[PROC_TAG_NUMA].push_back(info->NumaNode.GroupMask);
            break;
        case RelationProcessorCore:
            if (info->Processor.GroupCount != 1) {
                THROW(RuntimeException, "GetLogicalProcessorInformationExで予期しないデータ");
            }
            data[PROC_TAG_CORE].push_back(info->Processor.GroupMask[0]);
            break;
        default:
            break;
        }
        ptr += info->Size;
    }
}
const GROUP_AFFINITY* CPUInfo::GetData(PROCESSOR_INFO_TAG tag, int* count) {
    *count = (int)data[tag].size();
    return data[tag].data();
}

extern "C" __declspec(dllexport) void* CPUInfo_Create(AMTContext * ctx) {
    try {
        return new CPUInfo();
    } catch (const Exception& exception) {
        ctx->setError(exception);
    }
    return nullptr;
}
extern "C" __declspec(dllexport) void CPUInfo_Delete(CPUInfo * ptr) { delete ptr; }
extern "C" __declspec(dllexport) const GROUP_AFFINITY * CPUInfo_GetData(CPUInfo * ptr, int tag, int* count) {
    return ptr->GetData((PROCESSOR_INFO_TAG)tag, count);
}

bool SetCPUAffinity(int group, uint64_t mask) {
    if (mask == 0) {
        return true;
    }
    GROUP_AFFINITY gf = GROUP_AFFINITY();
    gf.Group = group;
    gf.Mask = (KAFFINITY)mask;
    bool result = (SetThreadGroupAffinity(GetCurrentThread(), &gf, nullptr) != FALSE);
    // プロセスが複数のグループにまたがってると↓はエラーになるらしい
    SetProcessAffinityMask(GetCurrentProcess(), (DWORD_PTR)mask);
    return result;
}
