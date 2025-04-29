#pragma once

/**
* Amtasukaze Communication to Host Process
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/
#pragma once

#include "common.h"
#include <string>
#include <vector>
#include <memory>

#include "StreamUtils.h"

#if defined(_WIN32) || defined(_WIN64)
using pipe_handle_t = HANDLE;
#else
using pipe_handle_t = int;
static const int INVALID_HANDLE_VALUE = -1;
#endif

std::vector<char> toUTF8String(const std::wstring& str);

std::string toJsonString(const tstring& str);

enum PipeCommand {
    HOST_CMD_TSAnalyze = 0,
    HOST_CMD_CMAnalyze,
    HOST_CMD_Filter,
    HOST_CMD_Encode,
    HOST_CMD_Mux,

    HOST_CMD_NoWait = 0x100,
};

struct ResourceAllocation {
    int32_t gpuIndex;
    int32_t group;
    uint64_t mask;

    bool IsFailed() const;
};

class ResourceManger : public AMTObject {
private:
    pipe_handle_t inPipe;
    pipe_handle_t outPipe;

public:
    ResourceManger(AMTContext& ctx, pipe_handle_t inPipe, pipe_handle_t outPipe)
        : AMTObject(ctx)
#if defined(_WIN32) || defined(_WIN64)
        , inPipe(inPipe)
        , outPipe(outPipe)
#else
        , inPipe((int)(intptr_t)inPipe)
        , outPipe((int)(intptr_t)outPipe)
#endif
    {
        if (!isValid()) {
            THROW(RuntimeException, "invalid pipe handle");
        }
    }

    ~ResourceManger() {
#if defined(_WIN32) || defined(_WIN64)
        if (inPipe != INVALID_HANDLE_VALUE) CloseHandle(inPipe);
        if (outPipe != INVALID_HANDLE_VALUE) CloseHandle(outPipe);
#else
        if (inPipe >= 0) close(inPipe);
        if (outPipe >= 0) close(outPipe);
#endif
    }

    bool isValid() const {
#if defined(_WIN32) || defined(_WIN64)
        return inPipe != INVALID_HANDLE_VALUE && outPipe != INVALID_HANDLE_VALUE;
#else
        return inPipe >= 0 && outPipe >= 0;
#endif
    }

    void write(MemoryChunk mc) const;
    void read(MemoryChunk mc) const;
    void writeCommand(int cmd) const;
    static ResourceAllocation DefaultAllocation();
    ResourceAllocation readCommand(int expected) const;
    ResourceAllocation request(PipeCommand phase) const;

    // リソース確保できるまで待つ
    ResourceAllocation wait(PipeCommand phase) const;
};

