#pragma once

/**
* MPEG2-TS parser
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/
#pragma once

#include "StreamUtils.h"

/** @brief TSパケットのアダプテーションフィールド */
struct AdapdationField : public MemoryChunk {
    AdapdationField(uint8_t* data, int length);

    uint8_t adapdation_field_length() const;
    // ARIB TR-14 8.2.3に送出規定がある
    // TSパケットの連続性、PCR連続性が切れる場合に送られる
    uint8_t discontinuity_indicator() const;
    uint8_t randam_access_indicator() const;
    uint8_t elementary_stream_priority_indicator() const;
    uint8_t PCR_flag() const;
    uint8_t OPCR_flag() const;
    uint8_t splicing_point_flag() const;
    uint8_t transport_private_data_flag() const;
    uint8_t adaptation_field_extension_flag() const;

    int64_t program_clock_reference;
    int64_t original_program_clock_reference;

    bool parse();

    bool check();

private:
    int64_t read_pcr(uint8_t* ptr);
};

/** @brief TSパケット */
struct TsPacket : public MemoryChunk {

    TsPacket(uint8_t* data);

    uint8_t sync_byte() const;
    uint8_t transport_error_indicator() const;
    uint8_t payload_unit_start_indicator() const;
    uint8_t transport_priority() const;
    uint16_t PID() const;
    uint8_t transport_scrambling_control() const;
    uint8_t adaptation_field_control() const;
    uint8_t continuity_counter() const;

    bool has_adaptation_field();
    bool has_payload();

    bool parse();

    bool check();

    MemoryChunk adapdation_field();

    MemoryChunk payload();

private:
    int payload_offset;
};

/** @brief PESパケットの先頭9バイト */
struct PESConstantHeader : public MemoryChunk {

    PESConstantHeader(MemoryChunk mc);

    int32_t packet_start_code_prefix() const;
    uint8_t stream_id() const;
    uint16_t PES_packet_length() const;

    uint8_t PES_scrambling_control() const;
    uint8_t PES_priority() const;
    uint8_t data_alignment_indicator() const;
    uint8_t copyright() const;
    uint8_t original_or_copy() const;
    uint8_t PTS_DTS_flags() const;
    uint8_t ESCR_flags() const;
    uint8_t ES_rate_flags() const;
    uint8_t DSM_trick_mode_flags() const;
    uint8_t additional_copy_info_flags() const;
    uint8_t PES_CRC_flags() const;
    uint8_t PES_extension_flags() const;
    uint8_t PES_header_data_length() const;

    /** @brief 十分な長さがない場合 false を返す */
    bool parse();

    bool check();
};

struct PESPacket : public PESConstantHeader {

    PESPacket(MemoryChunk mc);

    bool has_PTS();
    bool has_DTS();

    bool parse();

    bool check();

    MemoryChunk paylod();

    // PTS, DTSがあるときだけ書き換える
    void changeTimestamp(int64_t PTS, int64_t DTS);

    void changeStreamId(uint8_t stream_id_);

    void writePacketLength();

    int64_t PTS, DTS;

private:
    int64_t readTimeStamp(uint8_t* ptr);

    void writeTimeStamp(uint8_t* ptr, int64_t TS);

    int payload_offset;
};

/** @brief TSパケットを切り出す
* inputTS()を必要回数呼び出して最後にflush()を必ず呼び出すこと。
* flush()を呼び出さないと内部のバッファに残ったデータが処理されない。
*/
class TsPacketParser : public AMTObject {
    enum {
        // 同期コードを探すときにチェックするパケット数
        CHECK_PACKET_NUM = 8,
    };
public:
    TsPacketParser(AMTContext& ctx);

    /** @brief TSデータを入力 */
    void inputTS(MemoryChunk data);

    /** @brief 内部バッファをフラッシュ */
    void flush();

    /** @brief 残っているデータを全てクリア */
    void reset();

protected:
    /** @brief 切りだされたTSパケットを処理 */
    virtual void onTsPacket(TsPacket packet) = 0;

private:
    AutoBuffer buffer;
    bool syncOK;

    // numPacket個分のパケットの同期バイトが合っているかチェック
    bool checkSyncByte(uint8_t* ptr, int numPacket);

    // 「先頭と次のパケットの同期バイトを見て合っていれば出力」を繰り返す
    void outPackets();

    // パケットをチェックして出力
    void checkAndOutPacket(MemoryChunk data);
};

class TsPacketHandler {
public:
    virtual void onTsPacket(int64_t clock, TsPacket packet) = 0;
};

class PesParser : public TsPacketHandler {
public:
    PesParser();

    /** @brief TSパケット(チェック済み)を入力 */
    virtual void onTsPacket(int64_t clock, TsPacket packet);

protected:
    virtual void onPesPacket(int64_t clock, PESPacket packet) = 0;

private:
    AutoBuffer buffer;
    int contCounter;

    // パケットをチェックして出力
    void checkAndOutPacket(int64_t clock, MemoryChunk data);
};

struct Descriptor : public MemoryChunk {

    Descriptor(uint8_t* data, int length);

    int tag();
    int desc_length();
    MemoryChunk payload();
};

std::vector<Descriptor> ParseDescriptors(MemoryChunk mc);;

struct ServiceDescriptor {
    ServiceDescriptor(Descriptor desc);

    int service_type();

    MemoryChunk service_provider_name();
    MemoryChunk service_name();

    bool parse();

private:
    Descriptor desc;
    MemoryChunk payload_;
    uint8_t* service_provider_name_;
    uint8_t* service_name_;
};

struct ShortEventDescriptor {
    ShortEventDescriptor(Descriptor desc);

    int ISO_639_language_code();

    MemoryChunk event_name();
    MemoryChunk text();

    bool parse();

private:
    Descriptor desc;
    MemoryChunk payload_;
    uint8_t* event_name_;
    uint8_t* text_;
};

struct StreamIdentifierDescriptor {
    StreamIdentifierDescriptor(Descriptor desc);

    int component_tag();

    bool parse();

private:
    Descriptor desc;
    uint8_t component_tag_;
};

struct ContentElement {
    ContentElement(uint8_t* ptr);

    uint8_t content_nibble_level_1();
    uint8_t content_nibble_level_2();
    uint8_t user_nibble_1();
    uint8_t user_nibble_2();

private:
    uint8_t * ptr;
};

struct ContentDescriptor {
    ContentDescriptor(Descriptor desc);

    bool parse();

    int numElems() const;

    ContentElement get(int i) const;

private:
    Descriptor desc;
    std::vector<ContentElement> elems;
};

struct PsiConstantHeader : public MemoryChunk {

    PsiConstantHeader(MemoryChunk mc);

    uint8_t table_id() const;
    uint8_t section_syntax_indicator() const;
    uint16_t section_length() const;

    bool parse();

    bool check() const;

};

struct PsiSection : public AMTObject, public PsiConstantHeader {

    PsiSection(AMTContext& ctx, MemoryChunk mc);

    uint16_t id();
    uint8_t version_number();
    uint8_t current_next_indicator();
    uint8_t section_number();
    uint8_t last_section_number();

    bool parse();

    bool check() const;

    MemoryChunk payload();
};

struct PATElement {

    PATElement(uint8_t* ptr);

    uint16_t program_number();
    uint16_t PID();

    bool is_network_PID();

private:
    uint8_t* ptr;
};

struct PAT {

    PAT(PsiSection section);

    uint16_t TSID();

    bool parse();

    bool check() const;

    int numElems() const;

    PATElement get(int i) const;

private:
    PsiSection section;
    MemoryChunk payload_;
};

struct PMTElement {

    PMTElement(uint8_t* ptr);

    uint8_t stream_type();
    uint16_t elementary_PID();
    uint16_t ES_info_length();
    MemoryChunk descriptor();
    int size();

private:
    uint8_t* ptr;
};

struct PMT {

    PMT(PsiSection section);

    uint16_t program_number();
    uint16_t PCR_PID();
    uint16_t program_info_length();

    bool parse();

    bool check() const;

    int numElems() const;

    PMTElement get(int i) const;

private:
    PsiSection section;
    MemoryChunk payload_;
    std::vector<PMTElement> elems;
};

struct SDTElement {

    SDTElement(uint8_t* ptr);

    uint16_t service_id();
    uint16_t descriptor_loop_length();
    MemoryChunk descriptor();
    int size();

private:
    uint8_t* ptr;
};

struct SDT {

    SDT(PsiSection section);

    uint16_t TSID();
    uint16_t original_network_id();

    bool parse();

    bool check() const;

    int numElems() const;

    SDTElement get(int i) const;

private:
    PsiSection section;
    MemoryChunk payload_;
    std::vector<SDTElement> elems;
};

struct JSTTime {
    uint64_t time;

    JSTTime();
    JSTTime(uint64_t time);

    void getDay(int& y, int& m, int& d);

    void getTime(int& h, int& m, int& s);

    static void MJDtoYMD(uint16_t mjd16, int& y, int& m, int& d);
};

struct TDT {

    TDT(PsiSection section);

    JSTTime JST_time();

    bool parse();

    bool check() const;

private:
    PsiSection section;
    MemoryChunk payload_;
};

struct TOT {

    TOT(PsiSection section);

    JSTTime JST_time();

    bool parse();

    bool check() const;

private:
    PsiSection section;
    MemoryChunk payload_;
};

struct EITElement {

    EITElement(uint8_t* ptr);

    uint16_t event_id();
    JSTTime start_time();
    int32_t duration();
    uint8_t running_status();
    uint8_t free_CA_mode();
    uint16_t descriptor_loop_length();
    MemoryChunk descriptor();
    int size();

private:
    uint8_t * ptr;
};

struct EIT {

    EIT(PsiSection section);

    uint16_t service_id();
    uint16_t transport_stream_id();
    uint16_t original_network_id();
    uint8_t segment_last_section_number();
    uint8_t last_table_id();

    bool parse();

    bool check() const;

    int numElems() const;

    EITElement get(int i) const;

private:
    PsiSection section;
    MemoryChunk payload_;
    std::vector<EITElement> elems;
};

class PsiParser : public AMTObject, public TsPacketHandler {
public:
    PsiParser(AMTContext& ctx);

    /** 状態リセット */
    void clear();

    virtual void onTsPacket(int64_t clock, TsPacket packet);

protected:
    virtual void onPsiSection(int64_t clock, PsiSection section) = 0;

private:
    AutoBuffer buffer;
    int64_t packetClock;

    // パケットをチェックして出力
    void checkAndOutSection();
};

class PsiUpdatedDetector : public PsiParser {
public:

    PsiUpdatedDetector(AMTContext&ctx);

protected:
    virtual void onPsiSection(int64_t clock, PsiSection section);

    virtual void onTableUpdated(int64_t clock, PsiSection section) = 0;

private:
    AutoBuffer curSection;
};

class PidHandlerTable {
public:
    PidHandlerTable();

    // 固定PIDハンドラを登録。必ず最初に呼び出すこと
    //（clearしても削除されない）
    void addConstant(int pid, TsPacketHandler* handler);

    /** @brief 番号pid位置にhandlerをセット
    * 同じテーブル中で同じハンドラは１つのpidにしか紐付けできない
    * ハンドラが別のpidにひも付けされている場合解除されてから新しくセットされる
    */
    bool add(int pid, TsPacketHandler* handler, bool overwrite = true);

    TsPacketHandler* get(int pid);

    void clear();

    std::vector<int> getSetPids() const;

private:
    TsPacketHandler *table[MAX_PID + 1];
    std::map<int, TsPacketHandler*> constHandlers;
    std::map<TsPacketHandler*, int> handlers;
};

struct PMTESInfo {
    int stype;
    int pid;

    PMTESInfo();
    PMTESInfo(int stype, int pid);
};

class TsPacketSelectorHandler {
public:
    // サービスを設定する場合はサービスのpids上でのインデックス
    // なにもしない場合は負の値の返す
    virtual int onPidSelect(int TSID, const std::vector<int>& pids) = 0;

    virtual void onPmtUpdated(int PcrPid) = 0;

    // TsPacketSelectorでPID Tableが変更された時変更後の情報が送られる
    virtual void onPidTableChanged(const PMTESInfo video, const std::vector<PMTESInfo>& audio, const PMTESInfo caption) = 0;

    virtual void onVideoPacket(int64_t clock, TsPacket packet) = 0;

    virtual void onAudioPacket(int64_t clock, TsPacket packet, int audioIdx) = 0;

    virtual void onCaptionPacket(int64_t clock, TsPacket packet) = 0;

    virtual void onTime(int64_t clock, JSTTime time) = 0;
};

class TsPacketSelector : public AMTObject {
public:
    TsPacketSelector(AMTContext& ctx);

    ~TsPacketSelector();

    void setHandler(TsPacketSelectorHandler* handler);

    // 表示用
    void setStartClock(int64_t startClock);

    // PSIパーサ内部バッファをクリア
    void resetParser();

    void inputTsPacket(int64_t clock, TsPacket packet);

private:
    class PATDelegator : public PsiUpdatedDetector {
        TsPacketSelector& this_;
    public:
        PATDelegator(AMTContext&ctx, TsPacketSelector& this_);
        virtual void onTableUpdated(int64_t clock, PsiSection section);
    };
    class PMTDelegator : public PsiUpdatedDetector {
        TsPacketSelector& this_;
    public:
        PMTDelegator(AMTContext&ctx, TsPacketSelector& this_);
        virtual void onTableUpdated(int64_t clock, PsiSection section);
    };
    class TDTDelegator : public PsiUpdatedDetector {
        TsPacketSelector& this_;
    public:
        TDTDelegator(AMTContext&ctx, TsPacketSelector& this_);
        virtual void onTableUpdated(int64_t clock, PsiSection section);
    };
    class VideoDelegator : public TsPacketHandler {
        TsPacketSelector& this_;
    public:
        VideoDelegator(TsPacketSelector& this_);
        virtual void onTsPacket(int64_t clock, TsPacket packet);
    };
    class AudioDelegator : public TsPacketHandler {
        TsPacketSelector& this_;
        int audioIdx;
    public:
        AudioDelegator(TsPacketSelector& this_, int audioIdx);
        virtual void onTsPacket(int64_t clock, TsPacket packet);
    };
    class CaptionDelegator : public TsPacketHandler {
        TsPacketSelector& this_;
    public:
        CaptionDelegator(TsPacketSelector& this_);
        virtual void onTsPacket(int64_t clock, TsPacket packet);
    };

    AutoBuffer buffer;

    bool waitingNewVideo;
    int TSID_;
    int SID_;
    PMTESInfo videoEs;
    std::vector<PMTESInfo> audioEs;
    PMTESInfo captionEs;

    PATDelegator PsiParserPAT;
    PMTDelegator PsiParserPMT;
    TDTDelegator PsiParserTDT;
    int pmtPid;
    VideoDelegator videoDelegator;
    std::vector<AudioDelegator*> audioDelegators;
    CaptionDelegator captionDelegator;

    PidHandlerTable *curHandlerTable;
    PidHandlerTable *nextHandlerTable;

    TsPacketSelectorHandler *selectorHandler;

    // 27MHzクロック
    int64_t startClock;
    int64_t currentClock;

    void initHandlerTable(PidHandlerTable* table);

    void onPatUpdated(PsiSection section);

    void onPmtUpdated(PsiSection section);

    void onTdtUpdated(int64_t clock, PsiSection section);

    void onVideoPacket(int64_t clock, TsPacket packet);

    void onAudioPacket(int64_t clock, TsPacket packet, int audioIdx);

    void onCaptionPacket(int64_t clock, TsPacket packet);

    void swapHandlerTable();

    void ensureAudioDelegators(int numAudios);

    bool isVideo(uint8_t stream_type);

    bool isAudio(uint8_t stream_type);

    bool isCaption(uint8_t stream_type);

    static const char* streamTypeString(int stream_type);

    static const char* componentTagString(int component_tag);

    void printPMT(const PMT& pmt);
};

