/**
* Sub process and thread utility
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include "ProcessThread.h"
#include "rgy_thread_affinity.h"

// コマンドライン文字列を引数リストに分割するヘルパー関数
std::vector<tstring> SplitCommandLine(const tstring& cmdLine) {
    std::vector<tstring> args;
    
#if defined(_WIN32) || defined(_WIN64)
    // Windows版の実装
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdLine.c_str(), &argc);
    if (argv) {
        for (int i = 0; i < argc; i++) {
            args.push_back(argv[i]);
        }
        LocalFree(argv);
    }
#else
    // Linux/Unix版の実装
    // 単純なシェル風の解析を行う
    const auto isSpace = [](tchar c) { return c == _T(' ') || c == _T('\t'); };
    
    bool inQuote = false;
    tchar quoteChar = 0;
    tstring currentArg;
    
    for (size_t i = 0; i < cmdLine.length(); i++) {
        tchar c = cmdLine[i];
        
        // クオート処理
        if (c == _T('\'') || c == _T('"')) {
            if (!inQuote) {
                inQuote = true;
                quoteChar = c;
            } else if (c == quoteChar) {
                inQuote = false;
                quoteChar = 0;
            } else {
                currentArg += c;
            }
            continue;
        }
        
        // バックスラッシュによるエスケープ
        if (c == _T('\\') && i + 1 < cmdLine.length()) {
            tchar nextChar = cmdLine[i + 1];
            if (nextChar == _T('\'') || nextChar == _T('"') || nextChar == _T('\\')) {
                currentArg += nextChar;
                i++; // 次の文字をスキップ
                continue;
            }
        }
        
        // 空白文字の処理
        if (isSpace(c) && !inQuote) {
            if (!currentArg.empty()) {
                args.push_back(currentArg);
                currentArg.clear();
            }
        } else {
            currentArg += c;
        }
    }
    
    // 最後の引数を追加
    if (!currentArg.empty()) {
        args.push_back(currentArg);
    }
#endif

    // 空のリストにならないように、少なくとも空の引数を1つ入れる
    if (args.empty()) {
        args.push_back(_T(""));
    }
    
    return args;
}

SubProcess::SubProcess(const tstring& args, const bool disablePowerThrottoling) :
    process_(createRGYPipeProcess()),
    exitCode_(0),
    thSetPowerThrottling() {
    
    // RGYPipeProcessの初期化（標準入出力のモード設定）
    process_->init(PIPE_MODE_ENABLE, PIPE_MODE_ENABLE, PIPE_MODE_ENABLE);
    
    // コマンドライン文字列を引数リストに分割
    std::vector<tstring> argsList = SplitCommandLine(args);
    
    // プロセス起動
    if (process_->run(argsList, nullptr, 0, false, false) != 0) {
        THROW(RuntimeException, "プロセス起動に失敗。exeのパスを確認してください。");
    }

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
    
    size_t bytesWritten = process_->stdInFpWrite(mc.data, mc.length);
    if (bytesWritten != mc.length) {
        THROW(RuntimeException, "failed to write to stdin pipe (bytes written mismatch)");
    }
}

size_t SubProcess::readErr(MemoryChunk mc) {
    std::vector<uint8_t> buffer(mc.length);
    int bytesRead = process_->stdErrRead(buffer);
    if (bytesRead < 0) {
        return 0;
    }
    
    if (bytesRead > 0) {
        memcpy(mc.data, buffer.data(), bytesRead);
    }
    
    return bytesRead;
}

size_t SubProcess::readOut(MemoryChunk mc) {
    std::vector<uint8_t> buffer(mc.length);
    int bytesRead = process_->stdOutRead(buffer);
    if (bytesRead < 0) {
        return 0;
    }
    
    if (bytesRead > 0) {
        memcpy(mc.data, buffer.data(), bytesRead);
    }
    
    return bytesRead;
}

void SubProcess::finishWrite() {
    process_->stdInFpClose();
}

void SubProcess::runSetPowerThrottling() {
#if defined(_WIN32) || defined(_WIN64)
    int pid = process_->pid();
    if (pid > 0) {
        thSetPowerThrottling = std::make_unique<RGYThreadSetPowerThrottoling>(pid);
        thSetPowerThrottling->run(RGYThreadPowerThrottlingMode::Disabled);
    }
#endif // defined(_WIN32) || defined(_WIN64)
}

int SubProcess::join() {
    if (process_) {
        if (thSetPowerThrottling) {
            thSetPowerThrottling->abortThread();
        }
        
        exitCode_ = process_->waitAndGetExitCode();
        process_->close();
    }
    return exitCode_;
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

#if defined(_WIN32) || defined(_WIN64)
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
#endif // defined(_WIN32) || defined(_WIN64)
