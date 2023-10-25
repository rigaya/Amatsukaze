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

class ResourceManger : AMTObject {
    HANDLE inPipe;
    HANDLE outPipe;

    void write(MemoryChunk mc) const;

    void read(MemoryChunk mc) const;

    void writeCommand(int cmd) const;

    /*
       void write(int cmd, const std::string& json) {
           write(MemoryChunk((uint8_t*)&cmd, 4));
           int sz = (int)json.size();
           write(MemoryChunk((uint8_t*)&sz, 4));
           write(MemoryChunk((uint8_t*)json.data(), sz));
       }
    */

    static ResourceAllocation DefaultAllocation();

    ResourceAllocation readCommand(int expected) const;

public:
    ResourceManger(AMTContext& ctx, HANDLE inPipe, HANDLE outPipe);

    ResourceAllocation request(PipeCommand phase) const;

    // リソース確保できるまで待つ
    ResourceAllocation wait(PipeCommand phase) const;
};

