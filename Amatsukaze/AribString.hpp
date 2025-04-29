/*
* ARIB String
* このソースコードは大部分がTvTestのコードです。
* https://github.com/DBCTRADO/TVTest
* GPLで利用しています。
*/
#pragma once

#include <string>

struct MemoryChunk;

std::wstring GetAribString(MemoryChunk mc);
