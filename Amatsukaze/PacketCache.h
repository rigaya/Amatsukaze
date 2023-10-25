#pragma once

/**
* Packet Cache for file
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/
#pragma once

#include <deque>
#include <vector>

#include "StreamUtils.h"

class PacketCache : public AMTObject {
public:
    PacketCache(
        AMTContext& ctx,
        const tstring& filepath,
        const std::vector<int64_t> offsets, // データ数+1要素
        int nLinebit, // キャッシュラインデータ数のビット数
        int nEntry)	 // 最大キャッシュ保持ライン数
;
    ~PacketCache();
    // MemoryChunkは少なくともnEntry回の呼び出しまで有効
    MemoryChunk operator[](int index);
private:
    int nLinebit_;
    int nEntry_;
    int nLineSize_;
    int nBaseIndexMask_;
    File file_;
    std::vector<int64_t> offsets_;

    std::vector<uint8_t*> cacheTable_;
    std::deque<int> cacheEntries_;

    int getLineNumber(int index) const;
    int getLineBaseIndex(int index) const;
    uint8_t* getEntry(int lineNumber);
};



