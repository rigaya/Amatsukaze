/**
* Amtasukaze TS Info
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include "TsInfo.h"

TsInfoParser::TsInfoParser(AMTContext& ctx)
    : AMTObject(ctx)
    , patOK(false)
    , serviceOK(false)
    , timeOK(false)
    , numPrograms(0) {
    AddConstantHandler(0x0000); // PAT
    AddConstantHandler(0x0011); // SDT/BAT
    AddConstantHandler(0x0012); // H-EIT
    AddConstantHandler(0x0026); // M-EIT
    AddConstantHandler(0x0027); // L-EIT
    AddConstantHandler(0x0014); // TDT/TOT
}

void TsInfoParser::inputTsPacket(int64_t clock, TsPacket packet) {
    TsPacketHandler* handler = handlerTable.get(packet.PID());
    if (handler != NULL) {
        handler->onTsPacket(clock, packet);
    }
}

bool TsInfoParser::isProgramOK() const {
    if (!patOK) return false;
    for (int i = 0; i < numPrograms; ++i) {
        // �S�v���O�����̃p�P�b�g������Ƃ͌���Ȃ��̂�
        // 1�ł��f���̂���v���O�������擾�ł����OK�Ƃ���
        if (programList[i].programOK && programList[i].hasVideo) return true;
    }
    return false;
}

bool TsInfoParser::isScrampbled() const {
    bool hasScrambleNG = false;
    bool hasOKVideo = false;
    for (int i = 0; i < numPrograms; ++i) {
        if (!programList[i].programOK && programList[i].hasVideo && programList[i].hasScramble) {
            // �X�N�����u���ŉf����񂪎擾�ł��Ȃ�����
            hasScrambleNG = true;
        }
        if (programList[i].programOK && programList[i].hasVideo) {
            // �f����񂪎擾�ł����v���O����
            hasOKVideo = true;
        }
    }
    // �f�����1���擾�ł��Ȃ��āA�X�N�����u���f�����������ꍇ
    return !hasOKVideo && hasScrambleNG;
}

bool TsInfoParser::isOK() const {
    for (int i = 0; i < numPrograms; ++i) {
        if (programList[i].programOK == false) return false;
    }
    for (int i = 0; i < numPrograms; ++i) {
        // �f������T�[�r�X�ŃC�x���g��񂪂Ȃ��̂̓_��
        if (programList[i].hasVideo && programList[i].eventOK == false) return false;
    }
    return patOK && serviceOK && timeOK;
}

TsInfoParser::PSIDelegator* TsInfoParser::newPsiHandler(int pid) {
        auto p = new PSIDelegator(ctx, *this, pid);
        psiHandlers.emplace_back(p);
        return p;
    }

void TsInfoParser::AddConstantHandler(int pid) {
    handlerTable.addConstant(pid, newPsiHandler(pid));
}

TsInfoParser::ProgramItem* TsInfoParser::getProgramItem(int programId) {
    for (int i = 0; i < (int)numPrograms; ++i) {
        if (programList[i].programId == programId) {
            return &programList[i];
        }
    }
    return nullptr;
}

void TsInfoParser::onPsiUpdated(int pid, PsiSection section) {
    // ���ׂĂ�PID���ꏏ�����ɂɂ��Ă�̂ŁAPID���`�F�b�N����
    switch (section.table_id()) {
    case 0x00: // PAT
        if (pid == 0x0000) {
            onPAT(section);
        }
        break;
    case 0x02: // PMT
        // PMT��PID��onPMT�Ń`�F�b�N����
        onPMT(pid, section);
        break;
    case 0x42: // SDT�i���X�g���[���j
        if (pid == 0x0011) {
            onSDT(section);
        }
        break;
    case 0x4E: // EIT�i���X�g���[���̌��݂Ǝ��j
        if (pid == 0x0012 || pid == 0x0026 || pid == 0x0027) {
            onEIT(section);
        }
        break;
    case 0x70: // TDT
        if (pid == 0x0014) {
            onTDT(section);
        }
        break;
    case 0x73: // TOT
        if (pid == 0x0014) {
            onTOT(section);
        }
        break;
    }
}

void TsInfoParser::onPAT(PsiSection section) {
    PAT pat = section;
    if (pat.parse() && pat.check()) {
#if 1
        if (section.current_next_indicator() == 0) {
            printf("IS NEXT PAT\n");
        }
#endif
        std::vector<PATElement> programs;
        for (int i = 0; i < pat.numElems(); ++i) {
            auto elem = pat.get(i);
            int pn = elem.program_number();
            if (pn != 0) {
                programs.push_back(elem);
            }
        }
        numPrograms = (int)programs.size();
        // ���Ȃ��Ƃ��������₷�i�t�Ɍ��炷��handlerTable�Ƀ_���O�����O�|�C���^���c��̂Œ��Ӂj
        if ((int)programList.size() < numPrograms) {
            programList.resize(programs.size());
        }
        for (int i = 0; i < (int)programs.size(); ++i) {
            int pid = programs[i].PID();
            if (programList[i].pmtHandler == nullptr) {
                programList[i].pmtHandler =
                    std::unique_ptr<PSIDelegator>(new PSIDelegator(ctx, *this, pid));
            }
            programList[i].pmtPid = pid;
            programList[i].programId = programs[i].program_number();
            handlerTable.add(pid, programList[i].pmtHandler.get());
        }
        patOK = true;
    }
}

void TsInfoParser::onPMT(int pid, PsiSection section) {
    PMT pmt = section;
    if (section.current_next_indicator() && pmt.parse() && pmt.check()) {
        // �Y���v���O������T��
        ProgramItem* item = getProgramItem(pmt.program_number());
        if (item != nullptr && item->pmtPid == pid) { // PMT-PID�������Ă邩���`�F�b�N
            // �f�����݂���
            item->hasVideo = false;
            for (int i = 0; i < pmt.numElems(); ++i) {
                PMTElement elem = pmt.get(i);
                uint8_t stream_type = elem.stream_type();
                VIDEO_STREAM_FORMAT type = VS_UNKNOWN;
                switch (stream_type) {
                case 0x02:
                    type = VS_MPEG2;
                    break;
                case 0x1B:
                    type = VS_H264;
                    break;
                }
                if (type != VS_UNKNOWN) {
                    int videoPid = elem.elementary_PID();

                    // ����videoPid�̃v���O������T��
                    ProgramItem* top = nullptr;
                    for (int i = 0; i < (int)numPrograms; ++i) {
                        if (programList[i].videoPid == videoPid &&
                            programList[i].videoHandler != nullptr) {
                            top = &programList[i];
                            break;
                        }
                    }

                    item->hasVideo = true;
                    item->videoPid = videoPid;

                    if (top == nullptr) {
                        // �������ŏ��Ȃ̂Ŏ擾����
                        if (item->videoHandler == nullptr) {
                            item->videoHandler =
                                std::unique_ptr<SpVideoFrameParser>(new SpVideoFrameParser(ctx, *this, item));
                        }
                        item->videoHandler->setStreamFormat(type);
                        handlerTable.add(item->videoPid, item->videoHandler.get());
                    } else {
                        // �����͐擪�ł͂Ȃ��̂ŁA�擪�̃t�H�[�}�b�g�����炤
                        if (top->programOK) {
                            item->videoFormat = top->videoFormat;
                            item->programOK = true;
                        }
                    }
                    break;
                }
            }
            if (item->hasVideo == false) {
                // ���̃v���O�����ɂ͑Ή�����f�����Ȃ�
                item->programOK = true;
            }
        }
    }
}

void TsInfoParser::onSDT(PsiSection section) {
    SDT sdt(section);
    if (section.current_next_indicator() && sdt.parse() && sdt.check()) {
        serviceList.clear();
        for (int i = 0; i < sdt.numElems(); ++i) {
            ServiceInfo info;
            auto elem = sdt.get(i);
            info.serviceId = elem.service_id();
            auto descs = ParseDescriptors(elem.descriptor());
            for (int i = 0; i < (int)descs.size(); ++i) {
                if (descs[i].tag() == 0x48) { // �T�[�r�X�L�q�q
                    ServiceDescriptor servicedesc(descs[i]);
                    if (servicedesc.parse()) {
                        info.provider = GetAribString(servicedesc.service_provider_name());
                        info.name = GetAribString(servicedesc.service_name());
                        break;
                    }
                }
            }
            serviceList.push_back(info);
        }
        serviceOK = true;
    }
}

void TsInfoParser::onEIT(PsiSection section) {
    EIT eit(section);
    if (section.current_next_indicator() && eit.parse() && eit.check() &&
        section.section_number() == 0) // ���݂̔ԑg�̂�
    {
        ProgramItem* item = getProgramItem(eit.service_id());
        if (item != nullptr) {
            if (eit.numElems() > 0) {
                auto elem = eit.get(0);
                startTime = elem.start_time();
                auto descs = ParseDescriptors(elem.descriptor());
                ContentInfo info;
                for (int i = 0; i < (int)descs.size(); ++i) {
                    if (descs[i].tag() == 0x4D) { // �Z�`���C�x���g�L�q�q
                        ShortEventDescriptor seventdesc(descs[i]);
                        if (seventdesc.parse()) {
                            info.eventName = GetAribString(seventdesc.event_name());
                            info.text = GetAribString(seventdesc.text());
                        }
                    } else if (descs[i].tag() == 0x54) { // �R���e���g�L�q�q
                        ContentDescriptor contentdesc(descs[i]);
                        if (contentdesc.parse()) {
                            int num = contentdesc.numElems();
                            for (int i = 0; i < num; ++i) {
                                auto data_i = contentdesc.get(i);
                                ContentNibbles data;
                                data.content_nibble_level_1 = data_i.content_nibble_level_1();
                                data.content_nibble_level_2 = data_i.content_nibble_level_2();
                                data.user_nibble_1 = data_i.user_nibble_1();
                                data.user_nibble_2 = data_i.user_nibble_2();
                                info.nibbles.push_back(data);
                            }
                        }
                    }
                }
                // ����PID�̃v���O�����͑S�ď㏑��
                for (int i = 0; i < (int)programList.size(); ++i) {
                    if (programList[i].videoPid == item->videoPid) {
                        programList[i].contentInfo = info;
                        programList[i].contentInfo.serviceId = programList[i].programId;
                        programList[i].eventOK = true;
                    }
                }
            }
        }
    }
}

void TsInfoParser::onTDT(PsiSection section) {
    TDT tdt(section);
    if (tdt.parse() && tdt.check()) {
        time = tdt.JST_time();
        timeOK = true;
    }
}

void TsInfoParser::onTOT(PsiSection section) {
    TOT tot(section);
    if (tot.parse() && tot.check()) {
        time = tot.JST_time();
        timeOK = true;
    }
}

TsInfoParser::PSIDelegator::PSIDelegator(AMTContext&ctx, TsInfoParser& this_, int pid)
    : PsiUpdatedDetector(ctx), this_(this_), pid(pid) {}

void TsInfoParser::PSIDelegator::onTableUpdated(int64_t clock, PsiSection section) {
    this_.onPsiUpdated(pid, section);
}

TsInfoParser::ProgramItem::ProgramItem()
    : programOK(false)
    , hasScramble(false)
    , pmtPid(-1)
    , eventOK(false) {
    programId = -1;
    hasVideo = false;
    videoPid = -1;
    videoFormat = VideoFormat();
}

TsInfoParser::SpVideoFrameParser::SpVideoFrameParser(AMTContext&ctx, TsInfoParser& this_, ProgramItem* item)
    : VideoFrameParser(ctx), this_(this_), item(item) {}

void TsInfoParser::SpVideoFrameParser::onTsPacket(int64_t clock, TsPacket packet) {
    if (packet.transport_scrambling_control()) {
        // �X�N�����u���p�P�b�g
        if (item->hasScramble == false) {
            for (int i = 0; i < (int)this_.programList.size(); ++i) {
                if (this_.programList[i].videoPid == item->videoPid) {
                    this_.programList[i].hasScramble = true;
                }
            }
        }
        return;
    }
    PesParser::onTsPacket(clock, packet);
}

void TsInfoParser::SpVideoFrameParser::onVideoPesPacket(int64_t clock, const std::vector<VideoFrameInfo>& frames, PESPacket packet) {}
void TsInfoParser::SpVideoFrameParser::onVideoFormatChanged(VideoFormat fmt) {
    // ����PID�̃v���O�����͑S�ď㏑��
    for (int i = 0; i < (int)this_.programList.size(); ++i) {
        if (this_.programList[i].videoPid == item->videoPid) {
            this_.programList[i].videoFormat = fmt;
            this_.programList[i].programOK = true;
        }
    }
}

TsInfo::TsInfo(AMTContext& ctx)
    : AMTObject(ctx)
    , parser(ctx) {}

void TsInfo::ReadFile(const tchar* filepath) {
    File srcfile(std::wstring(filepath), _T("rb"));
    // �t�@�C���̐^�񒆂�ǂ�
    srcfile.seek(srcfile.size() / 2, SEEK_SET);
    int ret = ReadTS(srcfile);
    if (ret == 0) {
        return;
    }
    bool isScrampbled = (ret == 2);
    // �_����������t�@�C���̐擪�t�߂�ǂ�
    srcfile.seek(srcfile.size() / 30, SEEK_SET);
    ret = ReadTS(srcfile);
    if (ret == 0) {
        return;
    }
    if (isScrampbled) {
        THROW(FormatException, "���ׂẴv���O�������X�N�����u������Ă��܂�");
    } else {
        THROW(FormatException, "TS�t�@�C���ɏ�񂪂���܂���");
    }
}

bool TsInfo::ReadFileFromC(const tchar* filepath) {
    try {
        ReadFile(filepath);
        return true;
    } catch (const Exception& exception) {
        ctx.setError(exception);
    }
    return false;
}

bool TsInfo::HasServiceInfo() {
    return parser.hasServiceInfo();
}

// ref int�Ŏ󂯎��
void TsInfo::GetDay(int* y, int* m, int* d) {
    parser.getTime().getDay(*y, *m, *d);
}

void TsInfo::GetTime(int* h, int* m, int* s) {
    parser.getTime().getTime(*h, *m, *s);
}

int TsInfo::GetNumProgram() {
    return parser.getNumPrograms();
}

void TsInfo::GetProgramInfo(int i, int* progId, int* hasVideo, int* videoPid, int* numContent) {
    auto& prog = parser.getProgram(i);
    *progId = prog.programId;
    *hasVideo = prog.hasVideo;
    *videoPid = prog.videoPid;
    *numContent = (int)parser.getContent(i).nibbles.size();
}

void TsInfo::GetVideoFormat(int i, int* stream, int* width, int* height, int* sarW, int* sarH) {
    auto& fmt = parser.getProgram(i).videoFormat;
    *stream = fmt.format;
    *width = fmt.width;
    *height = fmt.height;
    *sarW = fmt.sarWidth;
    *sarH = fmt.sarHeight;
}

void TsInfo::GetContentNibbles(int i, int ci, int *level1, int *level2, int* user1, int* user2) {
    auto& data = parser.getContent(i).nibbles[ci];
    *level1 = data.content_nibble_level_1;
    *level2 = data.content_nibble_level_2;
    *user1 = data.user_nibble_1;
    *user2 = data.user_nibble_2;
}

int TsInfo::GetNumService() {
    return (int)parser.getServiceList().size();
}

int TsInfo::GetServiceId(int i) {
    return parser.getServiceList()[i].serviceId;
}

// IntPtr�Ŏ󂯎����Marshal.PtrToStringUni�ŕϊ�
const wchar_t* TsInfo::GetProviderName(int i) {
    return parser.getServiceList()[i].provider.c_str();
}

const wchar_t* TsInfo::GetServiceName(int i) {
    return parser.getServiceList()[i].name.c_str();
}

const wchar_t* TsInfo::GetEventName(int i) {
    return parser.getContent(i).eventName.c_str();
}

const wchar_t* TsInfo::GetEventText(int i) {
    return parser.getContent(i).text.c_str();
}

std::vector<int> TsInfo::getSetPids() const {
    return parser.getSetPids();
}

int TsInfo::ReadTS(File& srcfile) {
    enum {
        BUFSIZE = 4 * 1024 * 1024,
        MAX_BYTES = 100 * 1024 * 1024
    };
    auto buffer_ptr = std::unique_ptr<uint8_t[]>(new uint8_t[BUFSIZE]);
    MemoryChunk buffer(buffer_ptr.get(), BUFSIZE);
    size_t totalRead = 0;
    size_t readBytes;
    SpTsPacketParser packetParser(*this);
    do {
        readBytes = srcfile.read(buffer);
        packetParser.inputTS(MemoryChunk(buffer.data, readBytes));
        if (parser.isOK()) return 0;
        totalRead += readBytes;
    } while (readBytes == buffer.length && totalRead < MAX_BYTES);
    if (parser.isProgramOK()) return 0;
    if (parser.isScrampbled()) return 2;
    return 1;
}

// C API for P/Invoke
extern "C" __declspec(dllexport) void* TsInfo_Create(AMTContext * ctx) { return new TsInfo(*ctx); }
extern "C" __declspec(dllexport) void TsInfo_Delete(TsInfo * ptr) { delete ptr; }
extern "C" __declspec(dllexport) int TsInfo_ReadFile(TsInfo * ptr, const tchar * filepath) { return ptr->ReadFileFromC(filepath); }
extern "C" __declspec(dllexport) int TsInfo_HasServiceInfo(TsInfo * ptr) { return ptr->HasServiceInfo(); }
extern "C" __declspec(dllexport) void TsInfo_GetDay(TsInfo * ptr, int* y, int* m, int* d) { ptr->GetDay(y, m, d); }
extern "C" __declspec(dllexport) void TsInfo_GetTime(TsInfo * ptr, int* h, int* m, int* s) { return ptr->GetTime(h, m, s); }
extern "C" __declspec(dllexport) int TsInfo_GetNumProgram(TsInfo * ptr) { return ptr->GetNumProgram(); }
extern "C" __declspec(dllexport) void TsInfo_GetProgramInfo(TsInfo * ptr, int i, int* progId, int* hasVideo, int* videoPid, int* numContent) {
    return ptr->GetProgramInfo(i, progId, hasVideo, videoPid, numContent);
}
extern "C" __declspec(dllexport) void TsInfo_GetVideoFormat(TsInfo * ptr, int i, int* stream, int* width, int* height, int* sarW, int* sarH) {
    return ptr->GetVideoFormat(i, stream, width, height, sarW, sarH);
}
extern "C" __declspec(dllexport) void TsInfo_GetContentNibbles(TsInfo * ptr, int i, int ci, int *level1, int *level2, int* user1, int* user2) {
    return ptr->GetContentNibbles(i, ci, level1, level2, user1, user2);
}
extern "C" __declspec(dllexport) int TsInfo_GetNumService(TsInfo * ptr) { return ptr->GetNumService(); }
extern "C" __declspec(dllexport) int TsInfo_GetServiceId(TsInfo * ptr, int i) { return ptr->GetServiceId(i); }
extern "C" __declspec(dllexport) const wchar_t* TsInfo_GetProviderName(TsInfo * ptr, int i) { return ptr->GetProviderName(i); }
extern "C" __declspec(dllexport) const wchar_t* TsInfo_GetServiceName(TsInfo * ptr, int i) { return ptr->GetServiceName(i); }
extern "C" __declspec(dllexport) const wchar_t* TsInfo_GetEventName(TsInfo * ptr, int i) { return ptr->GetEventName(i); }
extern "C" __declspec(dllexport) const wchar_t* TsInfo_GetEventText(TsInfo * ptr, int i) { return ptr->GetEventText(i); }

typedef bool(*TS_SLIM_CALLBACK)();


TsSlimFilter::TsSlimFilter(AMTContext& ctx, int videoPid)
    : AMTObject(ctx)
    , videoPid(videoPid) {}

bool TsSlimFilter::exec(const tchar* srcpath, const tchar* dstpath, TS_SLIM_CALLBACK cb) {
    try {
        File srcfile(std::wstring(srcpath), L"rb");
        File dstfile(std::wstring(dstpath), L"wb");
        pfile = &dstfile;
        videoOk = false;
        enum {
            BUFSIZE = 4 * 1024 * 1024
        };
        auto buffer_ptr = std::unique_ptr<uint8_t[]>(new uint8_t[BUFSIZE]);
        MemoryChunk buffer(buffer_ptr.get(), BUFSIZE);
        size_t readBytes;
        SpTsPacketParser packetParser(*this);
        do {
            readBytes = srcfile.read(buffer);
            packetParser.inputTS(MemoryChunk(buffer.data, readBytes));
            if (cb() == false) return false;
        } while (readBytes == buffer.length);
        packetParser.flush();
        return true;
    } catch (const Exception& exception) {
        ctx.setError(exception);
    }
    return false;
}

extern "C" __declspec(dllexport) void* TsSlimFilter_Create(AMTContext * ctx, int videoPid) { return new TsSlimFilter(*ctx, videoPid); }
extern "C" __declspec(dllexport) void TsSlimFilter_Delete(TsSlimFilter * ptr) { delete ptr; }
extern "C" __declspec(dllexport) bool TsSlimFilter_Exec(TsSlimFilter * ptr, const tchar * srcpath, const tchar * dstpath, TS_SLIM_CALLBACK cb) { return ptr->exec(srcpath, dstpath, cb); }