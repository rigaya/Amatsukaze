/**
* Sub process and thread utility
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include "ProcessThread.h"
#include "rgy_thread_affinity.h"
#include "cpu_info.h"

SubProcess::SubProcess(const tstring& args, const bool disablePowerThrottoling) :
    process_(createRGYPipeProcess()),
    bufferStdOut(),
    bufferStdErr(),
    exitCode_(0),
    thSetPowerThrottling() {
    // RGYPipeProcessの初期化（標準入出力のモード設定）
    process_->init(PIPE_MODE_ENABLE, PIPE_MODE_ENABLE, PIPE_MODE_ENABLE);
    process_->setStdOutBufferSize(4 * 1024);
    process_->setStdErrBufferSize(4 * 1024);
    
    // プロセス起動
    if (process_->run(args, nullptr, 0, false, false) != 0) {
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
    int bytesWritten = process_->stdInWrite(mc.data, mc.length);
    if (bytesWritten != (int)mc.length) {
        THROWF(RuntimeException, "failed to write to stdin pipe (bytes written mismatch, expected: %d, actual: %d)", (int)mc.length, bytesWritten);
    }
}

size_t SubProcess::readErr(MemoryChunk mc) {
    if (bufferStdErr.size() > 0) {
        memcpy(mc.data, bufferStdErr.data(), bufferStdErr.size());
        bufferStdErr.clear();
        return bufferStdErr.size();
    }
    int bytesRead = process_->stdErrRead(bufferStdErr);
    if (bytesRead < 0) {
        return 0;
    }
    
    if (bytesRead > 0) {
        bytesRead = std::min(bytesRead, (int)mc.length);
        memcpy(mc.data, bufferStdErr.data(), bytesRead);
        bufferStdErr.erase(bufferStdErr.begin(), bufferStdErr.begin() + bytesRead);
    }
    
    return bytesRead;
}

size_t SubProcess::readOut(MemoryChunk mc) {
    if (bufferStdOut.size() > 0) {
        memcpy(mc.data, bufferStdOut.data(), bufferStdOut.size());
        bufferStdOut.clear();
        return bufferStdOut.size();
    }
    int bytesRead = process_->stdOutRead(bufferStdOut);
    if (bytesRead < 0) {
        return 0;
    }
    
    if (bytesRead > 0) {
        bytesRead = std::min(bytesRead, (int)mc.length);
        memcpy(mc.data, bufferStdOut.data(), bytesRead);
        bufferStdOut.erase(bufferStdOut.begin(), bufferStdOut.begin() + bytesRead); 
    }
    
    return bytesRead;
}

void SubProcess::finishWrite() {
    process_->stdInClose();
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

StdRedirectedSubProcess::StdRedirectedSubProcess(const tstring& args, const int bufferLines, const bool isUtf8, const bool disablePowerThrottoling) :
    EventBaseSubProcess(args, disablePowerThrottoling),
    isUtf8(isUtf8),
    bufferLines(bufferLines),
    outLiner(this, false),
    errLiner(this, true),
    mtx(),
    lastLines() {}

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

// ANSIエスケープシーケンスとカラーコードを除去するヘルパー関数
std::vector<char> removeAnsiEscapeSequences(const std::vector<char>& input) {
    std::vector<char> output;
    output.reserve(input.size());
    
    bool inEscape = false;
    bool inCSI = false; // Control Sequence Introducer (\033[ or \x1b[)
    bool inOSC = false; // Operating System Command (\033] or \x1b])
    
    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        
        if (!inEscape && !inCSI && !inOSC) {
            if (c == '\033' || c == '\x1b') { // ESC文字
                inEscape = true;
            } else if (c >= '\x00' && c <= '\x1F' && c != '\t' && c != '\n' && c != '\r') {
                // 制御文字を除去（タブ、改行、復帰文字は除く）
                // 何もしない（スキップ）
            } else {
                output.push_back(c);
            }
        } else if (inEscape) {
            if (c == '[') {
                inCSI = true;
                inEscape = false;
            } else if (c == ']') {
                inOSC = true;
                inEscape = false;
            } else if (c >= '@' && c <= '~') {
                // 2文字エスケープシーケンス終了
                inEscape = false;
            } else if (c >= ' ' && c <= '/') {
                // 中間文字、パラメータ文字は続行
            } else {
                // その他の文字でエスケープ終了
                inEscape = false;
            }
        } else if (inCSI) {
            if (c >= '@' && c <= '~') {
                // CSI シーケンス終了文字
                inCSI = false;
            }
            // パラメータ文字（数字、セミコロン、スペースなど）や中間文字は無視して続行
        } else if (inOSC) {
            if (c == '\007' || (c == '\033' && i + 1 < input.size() && input[i + 1] == '\\')) {
                // OSC終了：BEL文字 または ESC\ 
                inOSC = false;
                if (c == '\033') {
                    ++i; // '\'をスキップ
                }
            }
        }
    }
    
    return output;
}

void StdRedirectedSubProcess::onTextLine(bool isErr, const uint8_t* ptr, int len, int brlen) {
    std::vector<char> line;
    if (isUtf8) {
        line = utf8ToString(ptr, len);
    } else {
        line.resize(len + 1, 0);
        line.resize(len);
        memcpy(line.data(), ptr, len);
    }
    
    // エラー出力の場合はANSIエスケープシーケンスを除去
    if (isErr) {
        line = removeAnsiEscapeSequences(line);
    }
    
    // nullターミネートを確保
    if (line.empty() || line.back() != '\0') {
        line.push_back('\0');
    }
    
    std::string br((char *)ptr + len, brlen);
    fprintf(SUBPROC_OUT, "%s%s", line.data(), br.c_str());
    fflush(SUBPROC_OUT);

    if (bufferLines > 0) {
        std::lock_guard<std::mutex> lock(mtx);
        if ((int)lastLines.size() > bufferLines) {
            lastLines.pop_front();
        }
        // バッファには終端文字を含めない
        std::vector<char> bufferLine = line;
        if (!bufferLine.empty() && bufferLine.back() == '\0') {
            bufferLine.pop_back();
        }
        lastLines.push_back(bufferLine);
    }
}

void StdRedirectedSubProcess::onOut(bool isErr, MemoryChunk mc) {
    if (bufferLines > 0 || isUtf8) { // 必要がある場合のみ
        (isErr ? errLiner : outLiner).AddBytes(mc);
    } else {
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
#else
CPUInfo::CPUInfo() {
    const auto cpuInfo = get_cpu_info();
    intptr_t allcoremask = 0;
    // Core
    for (int i = 0; i < MAX_CORE_COUNT; i++) {
        if (cpuInfo.proc_list[i].mask == 0) {
            break;
        }
        GROUP_AFFINITY af = GROUP_AFFINITY();
        af.Mask = (intptr_t)cpuInfo.proc_list[i].mask;
        data[PROC_TAG_CORE].push_back(af);
        allcoremask |= cpuInfo.proc_list[i].mask;
    }
    // とりあえず適当に埋める
    {
        GROUP_AFFINITY af = GROUP_AFFINITY();
        af.Mask = allcoremask;
        data[PROC_TAG_NUMA].push_back(af);
    }
    // L2
    for (int i = 0; i < MAX_CORE_COUNT; i++) {
        if (cpuInfo.caches[(int)RGYCacheLevel::L2][i].mask == 0) {
            break;
        }
        GROUP_AFFINITY af = GROUP_AFFINITY();
        af.Mask = (intptr_t)cpuInfo.caches[(int)RGYCacheLevel::L2][i].mask;
        data[PROC_TAG_L2].push_back(af);
    }
    // L3
    for (int i = 0; i < MAX_CORE_COUNT; i++) {
        if (cpuInfo.caches[(int)RGYCacheLevel::L3][i].mask == 0) {
            break;
        }
        GROUP_AFFINITY af = GROUP_AFFINITY();
        af.Mask = (intptr_t)cpuInfo.caches[(int)RGYCacheLevel::L3][i].mask;
        data[PROC_TAG_L3].push_back(af);
    } 
}
#endif // defined(_WIN32) || defined(_WIN64)

const GROUP_AFFINITY* CPUInfo::GetData(PROCESSOR_INFO_TAG tag, int* count) {
    *count = (int)data[tag].size();
    return data[tag].data();
}

extern "C" AMATSUKAZE_API void* CPUInfo_Create(AMTContext * ctx) {
    try {
        return new CPUInfo();
    } catch (const Exception& exception) {
        ctx->setError(exception);
    }
    return nullptr;
}

extern "C" AMATSUKAZE_API void CPUInfo_Delete(CPUInfo * ptr) { delete ptr; }

extern "C" AMATSUKAZE_API const GROUP_AFFINITY * CPUInfo_GetData(CPUInfo * ptr, int tag, int* count) {
    return ptr->GetData((PROCESSOR_INFO_TAG)tag, count);
}

bool SetCPUAffinity(int group, uint64_t mask) {
#if defined(_WIN32) || defined(_WIN64)
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
#else
    SetProcessAffinityMask(GetCurrentProcess(), (size_t)mask);
    return true;
#endif // defined(_WIN32) || defined(_WIN64)
}
