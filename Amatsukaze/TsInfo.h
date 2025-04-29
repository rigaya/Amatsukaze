/**
* Amtasukaze TS Info
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/
#pragma once

#include "Mpeg2TsParser.h"
#include "AribString.hpp"
#include "TsSplitter.h"
#include "CoreUtils.hpp"

#include <stdint.h>

#include <memory>

struct ProgramInfo {
    int programId;
    bool hasVideo;
    int videoPid; // 同じ映像の別サービスに注意
    VideoFormat videoFormat;
};

struct ServiceInfo {
    int serviceId;
    std::wstring provider;
    std::wstring name;
};

struct ContentNibbles {
    uint8_t content_nibble_level_1;
    uint8_t content_nibble_level_2;
    uint8_t user_nibble_1;
    uint8_t user_nibble_2;
};

struct ContentInfo {
    int serviceId;
    std::wstring eventName;
    std::wstring text;
    std::vector<ContentNibbles> nibbles;
};

class TsInfoParser : public AMTObject {
public:
    TsInfoParser(AMTContext& ctx);

    void inputTsPacket(int64_t clock, TsPacket packet);

    bool isProgramOK() const;

    bool isScrampbled() const;

    bool isOK() const;

    bool hasServiceInfo() {
        return serviceOK && timeOK;
    }

    int getNumPrograms() const {
        return (int)programList.size();
    }

    const ProgramInfo& getProgram(int i) const {
        return programList[i];
    }

    const ContentInfo& getContent(int i) const {
        return programList[i].contentInfo;
    }

    const std::vector<ServiceInfo>& getServiceList() const {
        return serviceList;
    }

    JSTTime getTime() const {
        return time;
    }

    std::vector<int> getSetPids() const {
        return handlerTable.getSetPids();
    }

private:
    class PSIDelegator : public PsiUpdatedDetector {
        TsInfoParser& this_;
        int pid;
    public:
        PSIDelegator(AMTContext&ctx, TsInfoParser& this_, int pid);
        virtual void onTableUpdated(int64_t clock, PsiSection section);
    };
    struct ProgramItem : ProgramInfo {
        bool programOK;
        bool hasScramble;
        int pmtPid;
        std::unique_ptr<PSIDelegator> pmtHandler;
        std::unique_ptr<VideoFrameParser> videoHandler;

        bool eventOK;
        ContentInfo contentInfo;

        ProgramItem();
    };
    class SpVideoFrameParser : public VideoFrameParser {
        TsInfoParser& this_;
        ProgramItem* item;
    public:
        SpVideoFrameParser(AMTContext&ctx, TsInfoParser& this_, ProgramItem* item);
        virtual void onTsPacket(int64_t clock, TsPacket packet);
    protected:
        virtual void onVideoPesPacket(int64_t clock, const std::vector<VideoFrameInfo>& frames, PESPacket packet);
        virtual void onVideoFormatChanged(VideoFormat fmt);
    };

    std::vector<std::unique_ptr<PSIDelegator>> psiHandlers;
    PidHandlerTable handlerTable;

    bool patOK;
    bool serviceOK;
    bool timeOK;

    int numPrograms;
    std::vector<ProgramItem> programList;
    std::vector<ServiceInfo> serviceList;
    JSTTime time;

    // イベント情報から
    JSTTime startTime;

    PSIDelegator* newPsiHandler(int pid);

    void AddConstantHandler(int pid);

    ProgramItem* getProgramItem(int programId);

    void onPsiUpdated(int pid, PsiSection section);

    void onPAT(PsiSection section);

    void onPMT(int pid, PsiSection section);

    void onSDT(PsiSection section);

    void onEIT(PsiSection section);

    void onTDT(PsiSection section);

    void onTOT(PsiSection section);
};

class TsInfo : public AMTObject {
public:
    TsInfo(AMTContext& ctx);

    void ReadFile(const tchar* filepath);

    bool ReadFileFromC(const tchar* filepath);

    bool HasServiceInfo();

    // ref intで受け取る
    void GetDay(int* y, int* m, int* d);

    void GetTime(int* h, int* m, int* s);

    int GetNumProgram();

    void GetProgramInfo(int i, int* progId, int* hasVideo, int* videoPid, int* numContent);

    void GetVideoFormat(int i, int* stream, int* width, int* height, int* sarW, int* sarH);

    void GetContentNibbles(int i, int ci, int *level1, int *level2, int* user1, int* user2);

    int GetNumService();

    int GetServiceId(int i);

    // IntPtrで受け取ってMarshal.PtrToStringUniで変換
    const wchar_t* GetProviderName(int i);

    const wchar_t* GetServiceName(int i);

    const wchar_t* GetEventName(int i);

    const wchar_t* GetEventText(int i);

    std::vector<int> getSetPids() const;

private:
    class SpTsPacketParser : public TsPacketParser {
        TsInfo& this_;
    public:
        SpTsPacketParser(TsInfo& this_)
            : TsPacketParser(this_.ctx)
            , this_(this_) {}

        virtual void onTsPacket(TsPacket packet) {
            this_.parser.inputTsPacket(-1, packet);
        }
    };

    TsInfoParser parser;

    int ReadTS(File& srcfile);
};

typedef bool(*TS_SLIM_CALLBACK)();

class TsSlimFilter : AMTObject {
public:
    TsSlimFilter(AMTContext& ctx, int videoPid);

    bool exec(const tchar* srcpath, const tchar* dstpath, TS_SLIM_CALLBACK cb);

private:
    class SpTsPacketParser : public TsPacketParser {
        TsSlimFilter& this_;
    public:
        SpTsPacketParser(TsSlimFilter& this_)
            : TsPacketParser(this_.ctx)
            , this_(this_) {}

        virtual void onTsPacket(TsPacket packet) {
            if (this_.videoOk == false && packet.PID() == this_.videoPid) {
                this_.videoOk = true;
            }
            if (this_.videoOk) {
                this_.pfile->write(packet);
            }
        }
    };

    File* pfile;
    int videoPid;
    bool videoOk;
};
