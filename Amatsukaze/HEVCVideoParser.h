// -----------------------------------------------------------------------------------------
// Amatsukaze改造版 by rigaya (original by nekopanda)
// -----------------------------------------------------------------------------------------
// The MIT License
//
// Copyright (c) 2024 rigaya
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

#pragma once

#include "StreamUtils.h"
#include "ReaderWriterFFmpeg.h"
#include "rgy_bitstream.h"

template<typename T>
struct RGYAVDeleter {
    RGYAVDeleter() : deleter(nullptr) {};
    RGYAVDeleter(std::function<void(T**)> deleter) : deleter(deleter) {};
    void operator()(T *p) { deleter(&p); }
    std::function<void(T**)> deleter;
};

class HEVCVideoParser : public AMTObject, public IVideoParser {
public:
    HEVCVideoParser(AMTContext& ctx);

    void initParser();

    virtual void reset();

    virtual bool inputFrame(MemoryChunk frame, std::vector<VideoFrameInfo>& info, int64_t PTS, int64_t DTS);

private:
    std::unique_ptr<AVCodecParserContext, decltype(&av_parser_close)> m_parserCtx;            //動画ストリームのParser
    std::unique_ptr<AVCodecContext, RGYAVDeleter<AVCodecContext>> m_codecCtxParser;       //動画ストリームのParser用

    decltype(parse_nal_unit_hevc_c)* m_parse_nal_unit_hevc;
    int m_posBytes;
};

