/**
* Amtasukaze Avisynth Source Plugin
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include "PacketCache.h"

PacketCache::PacketCache(
    AMTContext& ctx,
    const tstring& filepath,
    const std::vector<int64_t> offsets, // データ数+1要素
    int nLinebit, // キャッシュラインデータ数のビット数
    int nEntry)	 // 最大キャッシュ保持ライン数
    : AMTObject(ctx)
    , file_(filepath, _T("rb"))
    , offsets_(offsets)
    , nLinebit_(nLinebit)
    , nEntry_(nEntry) {
    int numData = (int)offsets.size() - 1;

    nLineSize_ = 1 << nLinebit;
    nBaseIndexMask_ = ~(nLineSize_ - 1);
    cacheTable_.resize((numData + nLineSize_ - 1) >> nLinebit_, nullptr);
}
PacketCache::~PacketCache() {
    for (int entry : cacheEntries_) {
        delete[] cacheTable_[entry];
    }
    cacheTable_.clear();
    cacheEntries_.clear();
}
// MemoryChunkは少なくともnEntry回の呼び出しまで有効
MemoryChunk PacketCache::operator[](int index) {
    int64_t localOffset = offsets_[index] - offsets_[getLineBaseIndex(index)];
    int dataSize = int(offsets_[index + 1] - offsets_[index]);
    uint8_t* entryPtr = getEntry(getLineNumber(index));
    return MemoryChunk(entryPtr + localOffset, dataSize);
}

int PacketCache::getLineNumber(int index) const {
    return index >> nLinebit_;
}
int PacketCache::getLineBaseIndex(int index) const {
    return index & nBaseIndexMask_;
}
uint8_t* PacketCache::getEntry(int lineNumber) {
    uint8_t*& entry = cacheTable_[lineNumber];
    if (entry == nullptr) {
        // キャッシュしていないので読み込む
        if ((int)cacheEntries_.size() >= nEntry_) {
            // エントリ数を超える場合は最初に読み込んだキャッシュラインを削除
            auto& firstEntry = cacheTable_[cacheEntries_.front()];
            delete[] firstEntry; firstEntry = nullptr;
            cacheEntries_.pop_front();
        }
        int baseIndex = lineNumber << nLinebit_;
        int numData = (int)offsets_.size() - 1;
        int64_t offset = offsets_[baseIndex];
        int64_t lineDataSize = offsets_[std::min(baseIndex + nLineSize_, numData)] - offset;
        entry = new uint8_t[(size_t)lineDataSize];
        cacheEntries_.push_back(lineNumber);
        file_.seek(offset, SEEK_SET);
        file_.read(MemoryChunk(entry, (size_t)lineDataSize));
    }
    return entry;
}
