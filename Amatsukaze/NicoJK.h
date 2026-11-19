#pragma once

/**
* Amtasukaze ニコニコ実況 ASS generator
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/
#pragma once

#include <regex>
#include <array>

#include "TranscodeSetting.h"
#include "StreamReform.h"
#include "ProcessThread.h"


class NicoJK : public AMTObject {
    enum ConvMode {
        CONV_ASS_TS,   // そのままtsを投げて変換
        CONV_ASS_LOG,  // NicoJKログを優先的に使って変換
    };
public:
    NicoJK(AMTContext& ctx,
        const ConfigWrapper& setting);

    bool makeASS(int serviceId, time_t startTime, int duration, const StreamReformInfo* reform = nullptr);

    bool isFail() const;

    const std::array<std::vector<std::string>, NICOJK_MAX>& getHeaderLines() const;

    const std::array<std::vector<NicoJKLine>, NICOJK_MAX>& getDialogues() const;

private:
    class MySubProcess : public EventBaseSubProcess {
    public:
        MySubProcess(const tstring& args);

        ~MySubProcess();

        bool isFail() const;

    private:
        class OutConv : public StringLiner {
        public:
            OutConv(bool isErr);
            int nlines;
        protected:
            bool isErr;
            virtual void OnTextLine(const uint8_t* ptr, int len, int brlen, bool endedWithCarriageReturnOnly);
        };

        OutConv outConv, errConv;
        virtual void onOut(bool isErr, MemoryChunk mc);
    };

    const ConfigWrapper& setting_;

    bool isFail_;
    std::array<std::vector<std::string>, NICOJK_MAX> headerlines_;
    std::array<std::vector<NicoJKLine>, NICOJK_MAX> dislogues_;

    const StreamReformInfo* reform_ = nullptr;
    // 方式A(基準録画の音声照合)の結果。nicojk_assのmakeに--timemapなどで渡す。
    tstring timeMapPath_;
    int refServiceId_ = -1;
    double refStart_ = 0;
    double refDuration_ = 0;
    int mergeChannel_ = -1;
    bool prepareReferenceSource(int serviceId, time_t startTime, int duration);
    NicoJKSourceSpec source_;
    bool resolveSource();
    int jknum_;
    std::string tvname_;

    void getJKNum(int serviceId);

    enum NicoJKMask {
        MASK_720S = 1,
        MASK_720T = 2,
        MASK_1080S = 4,
        MASK_1080T = 8,
        MASK_720X = MASK_720S | MASK_720T,
        MASK_1080X = MASK_1080S | MASK_1080T,
    };

    void makeT(NicoJKType srcType, NicoJKType dstType);

    tstring MakeNicoConvASSArgs(ConvMode mode, size_t startTime, NicoJKType type);

    bool nicoConvASS(ConvMode mode, size_t startTime);

    // nicojk_assのmakeを1回呼び出して、取得とASS変換(全解像度)を行う
    bool makeASSByScript(int serviceId, time_t startTime, int duration);
    tstring MakeNicoJKScriptArgs(const tstring& pythonCommand, int serviceId, time_t startTime, int duration);
    tstring getNicoJKAssScriptPath() const;

    static double toClock(int h, int m, int s, int ss);

    void readASS();

    bool makeASS_(int serviceId, time_t startTime, int duration);
};

class NicoJKFormatter : public AMTObject {
public:
    NicoJKFormatter(AMTContext& ctx);

    std::string generate(
        const std::vector<std::string>& headers,
        const std::vector<NicoJKLine>& dialogues);

private:
    StringBuilder sb;

    void time(double t);
};

