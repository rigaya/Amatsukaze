/**
* Amtasukaze Avisynth Source Plugin
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include "Mpeg2TsParser.h"

AdapdationField::AdapdationField(uint8_t* data, int length) : MemoryChunk(data, length) {}

uint8_t AdapdationField::adapdation_field_length() const { return data[0]; }
// ARIB TR-14 8.2.3に送出規定がある
// TSパケットの連続性、PCR連続性が切れる場合に送られる
uint8_t AdapdationField::discontinuity_indicator() const { return bsm(data[1], 7, 1); }
uint8_t AdapdationField::randam_access_indicator() const { return bsm(data[1], 6, 1); }
uint8_t AdapdationField::elementary_stream_priority_indicator() const { return bsm(data[1], 5, 1); }
uint8_t AdapdationField::PCR_flag() const { return bsm(data[1], 4, 1); }
uint8_t AdapdationField::OPCR_flag() const { return bsm(data[1], 3, 1); }
uint8_t AdapdationField::splicing_point_flag() const { return bsm(data[1], 2, 1); }
uint8_t AdapdationField::transport_private_data_flag() const { return bsm(data[1], 1, 1); }
uint8_t AdapdationField::adaptation_field_extension_flag() const { return bsm(data[1], 0, 1); }

bool AdapdationField::parse() {
    int consumed = 2;
    if (PCR_flag()) {
        if (consumed + 6 > (int)length) return false;
        program_clock_reference = read_pcr(&data[consumed]);
        consumed += 6;
    }
    if (OPCR_flag()) {
        if (consumed + 6 > (int)length) return false;
        original_program_clock_reference = read_pcr(&data[consumed]);
        consumed += 6;
    }
    return true;
}

bool AdapdationField::check() {
    // do nothing
    return true;
}
int64_t AdapdationField::read_pcr(uint8_t* ptr) {
    int64_t raw = read48(ptr);
    return bsm(raw, 15, 33) * 300 + bsm(raw, 0, 9);
}

TsPacket::TsPacket(uint8_t* data)
    : MemoryChunk(data, TS_PACKET_LENGTH), payload_offset(0) {}

uint8_t TsPacket::sync_byte() const { return data[0]; }
uint8_t TsPacket::transport_error_indicator() const { return bsm(data[1], 7, 1); }
uint8_t TsPacket::payload_unit_start_indicator() const { return bsm(data[1], 6, 1); }
uint8_t TsPacket::transport_priority() const { return bsm(data[1], 6, 1); }
uint16_t TsPacket::PID() const { return bsm(read16(&data[1]), 0, 13); }
uint8_t TsPacket::transport_scrambling_control() const { return bsm(data[3], 6, 2); }
uint8_t TsPacket::adaptation_field_control() const { return bsm(data[3], 4, 2); }
uint8_t TsPacket::continuity_counter() const { return bsm(data[3], 0, 4); }

bool TsPacket::has_adaptation_field() { return (adaptation_field_control() & 0x02) != 0; }
bool TsPacket::has_payload() { return (adaptation_field_control() & 0x01) != 0; }

bool TsPacket::parse() {
    if (adaptation_field_control() & 0x01) {
        if (adaptation_field_control() & 0x2) {
            // exists adapdation field
            int adaptation_field_length = data[4];
            // TSパケットヘッダ+アダプテーションフィールド長はadaptation_field_lengthに含まれていない
            payload_offset = 4 + 1 + adaptation_field_length;
        } else {
            payload_offset = 4;
        }
    }
    return true;
}

bool TsPacket::check() {
    if (sync_byte() != TS_SYNC_BYTE) return false; // 同期コードが合っていない
    if (PID() >= 0x0002U && PID() <= 0x000FU) return false; // 未定義PID
    if (transport_scrambling_control() == 0x01) return false; // 未定義スクランブル制御
    if (adaptation_field_control() == 0x00) return false; // 未定義アダプテーションフィールド制御
    if (has_payload() && payload_offset >= TS_PACKET_LENGTH) {
        // アダプテーションフィールドが長すぎる
        return false;
    }
    return true;
}

MemoryChunk TsPacket::adapdation_field() {
    if (has_payload()) {
        return MemoryChunk(&data[4], payload_offset - 4);
    }
    return MemoryChunk(&data[4], TS_PACKET_LENGTH - 4);
}

MemoryChunk TsPacket::payload() {
    // ペイロードの長さ調整はアダプテーションフィールドでなされるよ～
    return MemoryChunk(
        &data[payload_offset], TS_PACKET_LENGTH - payload_offset);
}

PESConstantHeader::PESConstantHeader(MemoryChunk mc) : MemoryChunk(mc) {}

int32_t PESConstantHeader::packet_start_code_prefix() const { return read24(data); }
uint8_t PESConstantHeader::stream_id() const { return data[3]; }
uint16_t PESConstantHeader::PES_packet_length() const { return read16(&data[4]); }

uint8_t PESConstantHeader::PES_scrambling_control() const { return bsm(data[6], 4, 2); }
uint8_t PESConstantHeader::PES_priority() const { return bsm(data[6], 3, 1); }
uint8_t PESConstantHeader::data_alignment_indicator() const { return bsm(data[6], 2, 1); }
uint8_t PESConstantHeader::copyright() const { return bsm(data[6], 1, 1); }
uint8_t PESConstantHeader::original_or_copy() const { return bsm(data[6], 0, 1); }
uint8_t PESConstantHeader::PTS_DTS_flags() const { return bsm(data[7], 6, 2); }
uint8_t PESConstantHeader::ESCR_flags() const { return bsm(data[7], 5, 1); }
uint8_t PESConstantHeader::ES_rate_flags() const { return bsm(data[7], 4, 1); }
uint8_t PESConstantHeader::DSM_trick_mode_flags() const { return bsm(data[7], 3, 1); }
uint8_t PESConstantHeader::additional_copy_info_flags() const { return bsm(data[7], 2, 1); }
uint8_t PESConstantHeader::PES_CRC_flags() const { return bsm(data[7], 1, 1); }
uint8_t PESConstantHeader::PES_extension_flags() const { return bsm(data[7], 0, 1); }
uint8_t PESConstantHeader::PES_header_data_length() const { return data[8]; }

/** @brief 十分な長さがない場合 false を返す */
bool PESConstantHeader::parse() {
    if (length < 9) return false;
    return true;
}

bool PESConstantHeader::check() {
    if (packet_start_code_prefix() != 0x000001) return false; // スタートコード
    if ((data[6] & 0xC0) != 0x80U) return false; // 固定ビット
    if (PTS_DTS_flags() == 0x01)return false; // forbidden
    return true;
}

PESPacket::PESPacket(MemoryChunk mc) : PESConstantHeader(mc) {}

bool PESPacket::has_PTS() { return (PTS_DTS_flags() & 0x02) != 0; }
bool PESPacket::has_DTS() { return (PTS_DTS_flags() & 0x01) != 0; }

bool PESPacket::parse() {
    if (!PESConstantHeader::parse()) {
        return false;
    }
    if (stream_id() == 0xBF) {
        // private_stream_2は対応しない
        return false;
    }
    // ヘッダ長チェック
    int headerLength = PES_header_data_length();
    int calculatedLength = 0;
    if (has_PTS()) calculatedLength += 5;
    if (has_DTS()) calculatedLength += 5;
    if (ESCR_flags()) calculatedLength += 6;
    if (ES_rate_flags()) calculatedLength += 3;
    if (DSM_trick_mode_flags()) calculatedLength += 1;
    if (additional_copy_info_flags()) calculatedLength += 1;
    if (PES_CRC_flags()) calculatedLength += 2;
    if (PES_extension_flags()) calculatedLength += 1;

    if (headerLength < calculatedLength) {
        // ヘッダ長が足りない
        return false;
    }

    int consumed = 9;
    if (has_PTS()) {
        PTS = readTimeStamp(&data[consumed]);
        consumed += 5;
    }
    if (has_DTS()) {
        DTS = readTimeStamp(&data[consumed]);
        consumed += 5;
    }

    payload_offset = PES_header_data_length() + 9;

    return true;
}

bool PESPacket::check() {
    if (!PESConstantHeader::check()) {
        return false;
    }
    // 長さチェック
    int packetLength = PES_packet_length();
    if (payload_offset >= (int)length) return false;
    if (packetLength != 0 && packetLength + 6 != (int)length) return false;
    return true;
}

MemoryChunk PESPacket::paylod() {
    return MemoryChunk(data + payload_offset, (int)length - payload_offset);
}

// PTS, DTSがあるときだけ書き換える
void PESPacket::changeTimestamp(int64_t PTS, int64_t DTS) {
    int pos = 9;
    if (has_PTS()) {
        writeTimeStamp(&data[pos], PTS);
        pos += 5;
    }
    if (has_DTS()) {
        writeTimeStamp(&data[pos], DTS);
        pos += 5;
    }

    this->PTS = PTS;
    this->DTS = DTS;
}

void PESPacket::changeStreamId(uint8_t stream_id_) {
    data[3] = stream_id_;
    ASSERT(stream_id() == stream_id_);
}

void PESPacket::writePacketLength() {
    write16(&data[4], uint16_t(length - 6));
    ASSERT(PES_packet_length() == length - 6);
}
int64_t PESPacket::readTimeStamp(uint8_t* ptr) {
    int64_t raw = read40(ptr);
    return (bsm(raw, 33, 3) << 30) |
        (bsm(raw, 17, 15) << 15) |
        bsm(raw, 1, 15);
}

void PESPacket::writeTimeStamp(uint8_t* ptr, int64_t TS) {
    int64_t raw = 0;
    bms(raw, 3, 36, 4); // '0011'
    bms(raw, TS >> 30, 33, 3);
    bms(raw, 1, 32, 1); // marker_bit
    bms(raw, TS >> 15, 17, 15);
    bms(raw, 1, 16, 1); // marker_bit
    bms(raw, TS >> 0, 1, 15);
    bms(raw, 1, 0, 1); // marker_bit
    write40(ptr, raw);
}
TsPacketParser::TsPacketParser(AMTContext& ctx)
    : AMTObject(ctx)
    , syncOK(false) {}

/** @brief TSデータを入力 */
void TsPacketParser::inputTS(MemoryChunk data) {

    buffer.add(data);

    if (syncOK) {
        outPackets();
    }
    while (buffer.size() >= CHECK_PACKET_NUM * TS_PACKET_LENGTH) {
        // チェックするのに十分な量がある
        if (checkSyncByte(buffer.ptr(), CHECK_PACKET_NUM)) {
            syncOK = true;
            outPackets();
        } else {
            // ダメだったので1バイトスキップ
            syncOK = false;
            buffer.trimHead(1);
        }
    }
}

/** @brief 内部バッファをフラッシュ */
void TsPacketParser::flush() {
    while (buffer.size() >= TS_PACKET_LENGTH) {
        // 先頭パケットの同期コードが合っていれば出力する
        if (checkSyncByte(buffer.ptr(), 1)) {
            checkAndOutPacket(MemoryChunk(buffer.ptr(), TS_PACKET_LENGTH));
            buffer.trimHead(TS_PACKET_LENGTH);
        } else {
            buffer.trimHead(1);
        }
    }
}

/** @brief 残っているデータを全てクリア */
void TsPacketParser::reset() {
    buffer.clear();
    syncOK = false;
}

// numPacket個分のパケットの同期バイトが合っているかチェック
bool TsPacketParser::checkSyncByte(uint8_t* ptr, int numPacket) {
    for (int i = 0; i < numPacket; ++i) {
        if (ptr[TS_PACKET_LENGTH * i] != TS_SYNC_BYTE) {
            return false;
        }
    }
    return true;
}

// 「先頭と次のパケットの同期バイトを見て合っていれば出力」を繰り返す
void TsPacketParser::outPackets() {
    while (buffer.size() >= 2 * TS_PACKET_LENGTH &&
        checkSyncByte(buffer.ptr(), 2)) {
        checkAndOutPacket(MemoryChunk(buffer.ptr(), TS_PACKET_LENGTH));
        // onTsPacketでresetが呼ばれるかもしれないので注意
        buffer.trimHead(TS_PACKET_LENGTH);
    }
}

// パケットをチェックして出力
void TsPacketParser::checkAndOutPacket(MemoryChunk data) {
    TsPacket packet(data.data);
    if (packet.parse() && packet.check()) {
        onTsPacket(packet);
    }
}
PesParser::PesParser() : contCounter(0) {}

/** @brief TSパケット(チェック済み)を入力 */
/* virtual */ void PesParser::onTsPacket(int64_t clock, TsPacket packet) {

    // パケットの連続性カウンタをチェック
    int cc = packet.continuity_counter();
    if (cc != contCounter) {
        buffer.clear();
    }
    contCounter = (cc + 1) & 0xF;

    if (packet.has_payload()) {
        // データあり

        if (packet.payload_unit_start_indicator()) {
            // パケットスタート
            // ペイロード最初から始まるんだよね

            if (buffer.size() > 0) {
                // 前のパケットデータがある場合は出力
                checkAndOutPacket(clock, buffer.get());
                buffer.clear();
            }
        }

        MemoryChunk payload = packet.payload();
        buffer.add(payload);

        // 完了チェック
        PESConstantHeader header(buffer.get());
        if (header.parse()) {
            int PES_packet_length = header.PES_packet_length();
            int lengthIncludeHeader = PES_packet_length + 6;
            if (PES_packet_length != 0 && (int)buffer.size() >= lengthIncludeHeader) {
                // パケットのストア完了
                checkAndOutPacket(clock, MemoryChunk(buffer.ptr(), lengthIncludeHeader));
                buffer.trimHead(lengthIncludeHeader);
            }
        }
    }
}

// パケットをチェックして出力
void PesParser::checkAndOutPacket(int64_t clock, MemoryChunk data) {
    PESPacket packet(data);
    // フォーマットチェック
    if (packet.parse() && packet.check()) {
        // OK
        onPesPacket(clock, packet);
    }
}

Descriptor::Descriptor(uint8_t* data, int length) : MemoryChunk(data, length) {}

int Descriptor::tag() { return data[0]; }
int Descriptor::desc_length() { return data[1]; }
MemoryChunk Descriptor::payload() { return MemoryChunk(data + 2, desc_length()); }

std::vector<Descriptor> ParseDescriptors(MemoryChunk mc) {
    std::vector<Descriptor> list;
    int offset = 0;
    while (offset < (int)mc.length) {
        list.push_back(Descriptor(&mc.data[offset], mc.data[offset + 1]));
        offset += 2 + mc.data[offset + 1];
    }
    return list;
}
ServiceDescriptor::ServiceDescriptor(Descriptor desc) : desc(desc) {}

int ServiceDescriptor::service_type() { return payload_.data[0]; }

MemoryChunk ServiceDescriptor::service_provider_name() {
    return MemoryChunk(service_provider_name_ + 1, *service_provider_name_);
}
MemoryChunk ServiceDescriptor::service_name() {
    return MemoryChunk(service_name_ + 1, *service_name_);
}

bool ServiceDescriptor::parse() {
    payload_ = desc.payload();
    service_provider_name_ = &payload_.data[1];
    service_name_ = service_provider_name_ + 1 + *service_provider_name_;
    return (3 + *service_provider_name_ + *service_name_) <= (int)payload_.length;
}
ShortEventDescriptor::ShortEventDescriptor(Descriptor desc) : desc(desc) {}

int ShortEventDescriptor::ISO_639_language_code() { return read24(payload_.data); }

MemoryChunk ShortEventDescriptor::event_name() {
    return MemoryChunk(event_name_ + 1, *event_name_);
}
MemoryChunk ShortEventDescriptor::text() {
    return MemoryChunk(text_ + 1, *text_);
}

bool ShortEventDescriptor::parse() {
    payload_ = desc.payload();
    event_name_ = &payload_.data[3];
    text_ = event_name_ + 1 + *event_name_;
    return (5 + *event_name_ + *text_) <= (int)payload_.length;
}
StreamIdentifierDescriptor::StreamIdentifierDescriptor(Descriptor desc) : desc(desc) {}

int StreamIdentifierDescriptor::component_tag() { return component_tag_; }

bool StreamIdentifierDescriptor::parse() {
    MemoryChunk payload = desc.payload();
    if (payload.length != 1) return false;
    component_tag_ = payload.data[0];
    return true;
}
ContentElement::ContentElement(uint8_t* ptr) : ptr(ptr) {}

uint8_t ContentElement::content_nibble_level_1() { return bsm(ptr[0], 4, 4); }
uint8_t ContentElement::content_nibble_level_2() { return bsm(ptr[0], 0, 4); }
uint8_t ContentElement::user_nibble_1() { return bsm(ptr[1], 4, 4); }
uint8_t ContentElement::user_nibble_2() { return bsm(ptr[1], 0, 4); }
ContentDescriptor::ContentDescriptor(Descriptor desc) : desc(desc) {}

bool ContentDescriptor::parse() {
    MemoryChunk payload = desc.payload();
    if (payload.length % 2) return false;
    int offset = 0;
    while (offset < (int)payload.length) {
        elems.emplace_back(&payload.data[offset]);
        offset += 2;
    }
    return true;
}

int ContentDescriptor::numElems() const {
    return int(elems.size());
}

ContentElement ContentDescriptor::get(int i) const {
    return elems[i];
}

PsiConstantHeader::PsiConstantHeader(MemoryChunk mc) : MemoryChunk(mc) {}

uint8_t PsiConstantHeader::table_id() const { return data[0]; }
uint8_t PsiConstantHeader::section_syntax_indicator() const { return bsm(data[1], 7, 1); }
uint16_t PsiConstantHeader::section_length() const { return bsm(read16(&data[1]), 0, 12); }

bool PsiConstantHeader::parse() {
    if (length < 3) return false;
    return true;
}

bool PsiConstantHeader::check() const {
    return true; // 最後にCRCでチェックするので何もしない
}

PsiSection::PsiSection(AMTContext& ctx, MemoryChunk mc)
    : AMTObject(ctx), PsiConstantHeader(mc) {}

uint16_t PsiSection::id() { return read16(&data[3]); }
uint8_t PsiSection::version_number() { return bsm(data[5], 1, 5); }
uint8_t PsiSection::current_next_indicator() { return bsm(data[5], 0, 1); }
uint8_t PsiSection::section_number() { return data[6]; }
uint8_t PsiSection::last_section_number() { return data[7]; }

bool PsiSection::parse() {
    if (!PsiConstantHeader::parse()) return false;
    return true;
}

bool PsiSection::check() const {
    if (!PsiConstantHeader::check()) return false;
    if (length != section_length() + 3) return false;
    if (section_syntax_indicator()) {
        if (ctx.getCRC()->calc(data, (int)length, 0xFFFFFFFFUL)) return false;
    }
    return true;
}

MemoryChunk PsiSection::payload() {
    int payload_offset = section_syntax_indicator() ? 8 : 3;
    // CRC_32の4bytes引く
    return MemoryChunk(data + payload_offset, length - payload_offset - 4);
}

PATElement::PATElement(uint8_t* ptr) : ptr(ptr) {}

uint16_t PATElement::program_number() { return read16(ptr); }
uint16_t PATElement::PID() { return bsm(read16(&ptr[2]), 0, 13); }

bool PATElement::is_network_PID() { return (program_number() == 0); }

PAT::PAT(PsiSection section) : section(section) {}

uint16_t PAT::TSID() { return section.id(); }

bool PAT::parse() {
    payload_ = section.payload();
    return true;
}

bool PAT::check() const {
    if (section.table_id() != 0x00) return false;
    // これをチェックしないとCRCなしをOKとしてしまうので重要
    if (section.section_syntax_indicator() != 1) return false;
    return (payload_.length % 4) == 0;
}

int PAT::numElems() const {
    return (int)payload_.length / 4;
}

PATElement PAT::get(int i) const {
    ASSERT(i < numElems());
    return PATElement(payload_.data + i * 4);
}

PMTElement::PMTElement(uint8_t* ptr) : ptr(ptr) {}

uint8_t PMTElement::stream_type() { return *ptr; }
uint16_t PMTElement::elementary_PID() { return bsm(read16(&ptr[1]), 0, 13); }
uint16_t PMTElement::ES_info_length() { return bsm(read16(&ptr[3]), 0, 12); }
MemoryChunk PMTElement::descriptor() { return MemoryChunk(ptr + 5, ES_info_length()); }
int PMTElement::size() { return ES_info_length() + 5; }

PMT::PMT(PsiSection section) : section(section) {}

uint16_t PMT::program_number() { return section.id(); }
uint16_t PMT::PCR_PID() { return bsm(read16(&payload_.data[0]), 0, 13); }
uint16_t PMT::program_info_length() { return bsm(read16(&payload_.data[2]), 0, 12); }

bool PMT::parse() {
    payload_ = section.payload();
    int offset = program_info_length() + 4;
    while (offset < (int)payload_.length) {
        elems.emplace_back(&payload_.data[offset]);
        offset += elems.back().size();
    }
    return true;
}

bool PMT::check() const {
    if (section.table_id() != 0x02) return false;
    if (section.section_syntax_indicator() != 1) return false;
    return true;
}

int PMT::numElems() const {
    return int(elems.size());
}

PMTElement PMT::get(int i) const {
    return elems[i];
}

SDTElement::SDTElement(uint8_t* ptr) : ptr(ptr) {}

uint16_t SDTElement::service_id() { return read16(&ptr[0]); }
uint16_t SDTElement::descriptor_loop_length() { return bsm(read16(&ptr[3]), 0, 12); }
MemoryChunk SDTElement::descriptor() { return MemoryChunk(ptr + 5, descriptor_loop_length()); }
int SDTElement::size() { return descriptor_loop_length() + 5; }

SDT::SDT(PsiSection section) : section(section) {}

uint16_t SDT::TSID() { return section.id(); }
uint16_t SDT::original_network_id() { return read16(&payload_.data[0]); }

bool SDT::parse() {
    payload_ = section.payload();
    int offset = 3;
    while (offset < (int)payload_.length) {
        elems.emplace_back(&payload_.data[offset]);
        offset += elems.back().size();
    }
    return true;
}

bool SDT::check() const {
    if (section.section_syntax_indicator() != 1) return false;
    return true;
}

int SDT::numElems() const {
    return int(elems.size());
}

SDTElement SDT::get(int i) const {
    return elems[i];
}

JSTTime::JSTTime() {}
JSTTime::JSTTime(uint64_t time) : time(time) {}

void JSTTime::getDay(int& y, int& m, int& d) {
    MJDtoYMD((uint16_t)bsm(time, 24, 16), y, m, d);
}

void JSTTime::getTime(int& h, int& m, int& s) {
    int bcd = (int)bsm(time, 0, 24);
    int h1 = ((bcd >> 20) & 0xF);
    int h0 = ((bcd >> 16) & 0xF);
    int m1 = ((bcd >> 12) & 0xF);
    int m0 = ((bcd >> 8) & 0xF);
    int s1 = ((bcd >> 4) & 0xF);
    int s0 = ((bcd >> 0) & 0xF);
    h = h1 * 10 + h0;
    m = m1 * 10 + m0;
    s = s1 * 10 + s0;
}

/* static */ void JSTTime::MJDtoYMD(uint16_t mjd16, int& y, int& m, int& d) {
    // 2000年以前だったら上位1ビットを足す
    int mjd = (mjd16 < 51544) ? mjd16 + 65536 : mjd16;
    int ydash = int((mjd - 15078.2) / 365.25);
    int mdash = int((mjd - 14956.1 - int(ydash * 365.25)) / 30.6001);
    d = mjd - 14956 - int(ydash * 365.25) - int(mdash * 30.6001);
    int k = (mdash == 14 || mdash == 15) ? 1 : 0;
    y = ydash + k + 1900;
    m = mdash - 1 - k * 12;
}

TDT::TDT(PsiSection section) : section(section) {}

JSTTime TDT::JST_time() { return read40(&payload_.data[0]); }

bool TDT::parse() {
    payload_ = section.payload();
    return true;
}

bool TDT::check() const { return true; }

TOT::TOT(PsiSection section) : section(section) {}

JSTTime TOT::JST_time() { return read40(&payload_.data[0]); }

bool TOT::parse() {
    payload_ = section.payload();
    return true;
}

bool TOT::check() const {
    if (section.section_syntax_indicator() != 0) return false;
    // section_syntax_indicatorは0だがCRCがあるのでチェックする
    if (section.ctx.getCRC()->calc(section.data, (int)section.length, 0xFFFFFFFFUL)) return false;
    return true;
}

EITElement::EITElement(uint8_t* ptr) : ptr(ptr) {}

uint16_t EITElement::event_id() { return read16(&ptr[0]); }
JSTTime EITElement::start_time() { return read40(&ptr[2]); }
int32_t EITElement::duration() { return read24(&ptr[7]); }
uint8_t EITElement::running_status() { return bsm(ptr[10], 4, 3); }
uint8_t EITElement::free_CA_mode() { return bsm(ptr[10], 3, 1); }
uint16_t EITElement::descriptor_loop_length() { return bsm(read16(&ptr[10]), 0, 12); }
MemoryChunk EITElement::descriptor() { return MemoryChunk(ptr + 12, descriptor_loop_length()); }
int EITElement::size() { return descriptor_loop_length() + 12; }

EIT::EIT(PsiSection section) : section(section) {}

uint16_t EIT::service_id() { return section.id(); }
uint16_t EIT::transport_stream_id() { return read16(&payload_.data[0]); }
uint16_t EIT::original_network_id() { return read16(&payload_.data[2]); }
uint8_t EIT::segment_last_section_number() { return payload_.data[4]; }
uint8_t EIT::last_table_id() { return payload_.data[5]; }

bool EIT::parse() {
    payload_ = section.payload();
    int offset = 6;
    while (offset < (int)payload_.length) {
        elems.emplace_back(&payload_.data[offset]);
        offset += elems.back().size();
    }
    return true;
}

bool EIT::check() const {
    return true;
}

int EIT::numElems() const {
    return int(elems.size());
}

EITElement EIT::get(int i) const {
    return elems[i];
}
PsiParser::PsiParser(AMTContext& ctx)
    : AMTObject(ctx), packetClock(-1) {}

/** 状態リセット */
void PsiParser::clear() {
    buffer.clear();
}

/* virtual */ void PsiParser::onTsPacket(int64_t clock, TsPacket packet) {

    if (packet.has_payload()) {
        // データあり

        MemoryChunk payload = packet.payload();

        if (packet.payload_unit_start_indicator()) {
            // セクション開始がある
            int startPos = payload.data[0] + 1; // pointer field
            // 長さチェック
            if (startPos >= (int)payload.length) {
                return;
            }
            if (startPos > 1) {
                // 前のセクションの続きがある
                buffer.add(MemoryChunk(&payload.data[1], startPos - 1));
                checkAndOutSection();
            }
            buffer.clear();
            packetClock = clock;

            buffer.add(MemoryChunk(&payload.data[startPos], payload.length - startPos));
            checkAndOutSection();
        } else {
            buffer.add(MemoryChunk(&payload.data[0], payload.length));
            checkAndOutSection();
        }
    }
}

// パケットをチェックして出力
void PsiParser::checkAndOutSection() {
    // 完了チェック
    PsiConstantHeader header(buffer.get());
    if (header.parse()) {
        int lengthIncludeHeader = header.section_length() + 3;
        if ((int)buffer.size() >= lengthIncludeHeader) {
            // パケットのストア完了
            PsiSection section(ctx, MemoryChunk(buffer.ptr(), lengthIncludeHeader));
            // フォーマットチェック
            if (section.parse() && section.check()) {
                // OK
                onPsiSection(packetClock, section);
            }
            buffer.trimHead(lengthIncludeHeader);
        }
    }
}

PsiUpdatedDetector::PsiUpdatedDetector(AMTContext&ctx)
    : PsiParser(ctx) {}
/* virtual */ void PsiUpdatedDetector::onPsiSection(int64_t clock, PsiSection section) {
    if (section != curSection.get()) {
        curSection.clear();
        curSection.add(section);
        onTableUpdated(clock, section);
    }
}
PidHandlerTable::PidHandlerTable()
    : table() {}

// 固定PIDハンドラを登録。必ず最初に呼び出すこと
//（clearしても削除されない）
void PidHandlerTable::addConstant(int pid, TsPacketHandler* handler) {
    constHandlers[pid] = handler;
    table[pid] = handler;
}

/** @brief 番号pid位置にhandlerをセット
* 同じテーブル中で同じハンドラは１つのpidにしか紐付けできない
* ハンドラが別のpidにひも付けされている場合解除されてから新しくセットされる
*/
bool PidHandlerTable::add(int pid, TsPacketHandler* handler, bool overwrite) {
    // PATは変更不可
    if (pid == 0 || pid > MAX_PID) {
        return false;
    }
    if (!overwrite && table[pid] != nullptr) {
        return false;
    }
    if (table[pid] == handler) {
        // すでにセットされている
        return true;
    }
    auto it = handlers.find(handler);
    if (it != handlers.end()) {
        table[it->second] = NULL;
    }
    if (table[pid] != NULL) {
        handlers.erase(table[pid]);
    }
    table[pid] = handler;
    handlers[handler] = pid;
    return true;
}

TsPacketHandler* PidHandlerTable::get(int pid) {
    return table[pid];
}

void PidHandlerTable::clear() {
    for (auto pair : handlers) {
        auto it = constHandlers.find(pair.second);
        if (it != constHandlers.end()) {
            // 固定ハンドラがあればそれをセット
            table[pair.second] = it->second;
        } else {
            table[pair.second] = NULL;
        }
    }
    handlers.clear();
}

std::vector<int> PidHandlerTable::getSetPids() const {
    std::vector<int> pids;
    for (auto pair : constHandlers) {
        pids.push_back(pair.first);
    }
    for (auto pair : handlers) {
        pids.push_back(pair.second);
    }
    return pids;
}

PMTESInfo::PMTESInfo() {}
PMTESInfo::PMTESInfo(int stype, int pid)
    : stype(stype), pid(pid) {}
TsPacketSelector::TsPacketSelector(AMTContext& ctx)
    : AMTObject(ctx)
    , waitingNewVideo(false)
    , TSID_(-1)
    , SID_(-1)
    , videoEs(-1, -1)
    , PsiParserPAT(ctx, *this)
    , PsiParserPMT(ctx, *this)
    , PsiParserTDT(ctx, *this)
    , pmtPid(-1)
    , videoDelegator(*this)
    , captionDelegator(*this)
    , startClock(-1) {
    curHandlerTable = new PidHandlerTable();
    initHandlerTable(curHandlerTable);
    nextHandlerTable = new PidHandlerTable();
    initHandlerTable(nextHandlerTable);
}

TsPacketSelector::~TsPacketSelector() {
    delete curHandlerTable;
    delete nextHandlerTable;
    for (AudioDelegator* ad : audioDelegators) {
        delete ad;
    }
}

void TsPacketSelector::setHandler(TsPacketSelectorHandler* handler) {
    selectorHandler = handler;
}

// 表示用
void TsPacketSelector::setStartClock(int64_t startClock) {
    this->startClock = startClock;
}

// PSIパーサ内部バッファをクリア
void TsPacketSelector::resetParser() {
    PsiParserPAT.clear();
    PsiParserPMT.clear();
}

void TsPacketSelector::inputTsPacket(int64_t clock, TsPacket packet) {

    currentClock = clock;
    if (waitingNewVideo && packet.PID() == videoEs.pid) {
        // 待っていた映像パケット来たのでテーブル変更
        waitingNewVideo = false;
        swapHandlerTable();
        selectorHandler->onPidTableChanged(videoEs, audioEs, captionEs);
    }
    TsPacketHandler* handler = curHandlerTable->get(packet.PID());
    if (handler != NULL) {
        handler->onTsPacket(clock, packet);
    }

}
TsPacketSelector::PATDelegator::PATDelegator(AMTContext&ctx, TsPacketSelector& this_) : PsiUpdatedDetector(ctx), this_(this_) {}
/* virtual */ void TsPacketSelector::PATDelegator::onTableUpdated(int64_t clock, PsiSection section) {
    this_.onPatUpdated(section);
}
TsPacketSelector::PMTDelegator::PMTDelegator(AMTContext&ctx, TsPacketSelector& this_) : PsiUpdatedDetector(ctx), this_(this_) {}
/* virtual */ void TsPacketSelector::PMTDelegator::onTableUpdated(int64_t clock, PsiSection section) {
    this_.onPmtUpdated(section);
}
TsPacketSelector::TDTDelegator::TDTDelegator(AMTContext&ctx, TsPacketSelector& this_) : PsiUpdatedDetector(ctx), this_(this_) {}
/* virtual */ void TsPacketSelector::TDTDelegator::onTableUpdated(int64_t clock, PsiSection section) {
    this_.onTdtUpdated(clock, section);
}
TsPacketSelector::VideoDelegator::VideoDelegator(TsPacketSelector& this_) : this_(this_) {}
/* virtual */ void TsPacketSelector::VideoDelegator::onTsPacket(int64_t clock, TsPacket packet) {
    this_.onVideoPacket(clock, packet);
}
TsPacketSelector::AudioDelegator::AudioDelegator(TsPacketSelector& this_, int audioIdx)
    : this_(this_), audioIdx(audioIdx) {}
/* virtual */ void TsPacketSelector::AudioDelegator::onTsPacket(int64_t clock, TsPacket packet) {
    this_.onAudioPacket(clock, packet, audioIdx);
}
TsPacketSelector::CaptionDelegator::CaptionDelegator(TsPacketSelector& this_) : this_(this_) {}
/* virtual */ void TsPacketSelector::CaptionDelegator::onTsPacket(int64_t clock, TsPacket packet) {
    this_.onCaptionPacket(clock, packet);
}

void TsPacketSelector::initHandlerTable(PidHandlerTable* table) {
    table->addConstant(0x0000, &PsiParserPAT);
    table->addConstant(0x0014, &PsiParserTDT);
}

void TsPacketSelector::onPatUpdated(PsiSection section) {
    if (selectorHandler == NULL) {
        return;
    }
    PAT pat(section);
    if (section.current_next_indicator() && pat.parse() && pat.check()) {
        int patTSID = pat.TSID();
        std::vector<int> sids;
        std::vector<int> pids;
        for (int i = 0; i < pat.numElems(); ++i) {
            PATElement elem = pat.get(i);
            if (!elem.is_network_PID()) {
                sids.push_back(elem.program_number());
                pids.push_back(elem.PID());
            }
        }
        if (TSID_ != patTSID) {
            // TSが変わった
            curHandlerTable->clear();
            PsiParserPMT.clear();
            TSID_ = patTSID;
        }
        int progidx = selectorHandler->onPidSelect(patTSID, sids);
        if (progidx >= (int)sids.size()) {
            throw InvalidOperationException("選択したサービスインデックスは範囲外です");
        }
        if (progidx >= 0) {
            int sid = sids[progidx];
            int pid = pids[progidx];
            if (SID_ != sid) {
                // サービス選択が変わった
                curHandlerTable->clear();
                PsiParserPMT.clear();
                SID_ = sid;
            }
            pmtPid = pid;
            curHandlerTable->add(pmtPid, &PsiParserPMT);
        }
    }
}

void TsPacketSelector::onPmtUpdated(PsiSection section) {
    if (selectorHandler == NULL) {
        return;
    }
    PMT pmt(section);
    if (section.current_next_indicator() && pmt.parse() && pmt.check()) {

        printPMT(pmt);

        // 映像、音声、字幕を探す
        PMTESInfo videoEs(-1, -1);
        std::vector<PMTESInfo> audioEs;
        PMTESInfo captionEs(-1, -1);
        for (int i = 0; i < pmt.numElems(); ++i) {
            PMTElement elem = pmt.get(i);
            uint8_t stream_type = elem.stream_type();
            if (isVideo(stream_type) && videoEs.stype == -1) {
                videoEs.stype = stream_type;
                videoEs.pid = elem.elementary_PID();
            } else if (isAudio(stream_type)) {
                audioEs.emplace_back(stream_type, elem.elementary_PID());
            } else if (isCaption(stream_type)) {
                auto descs = ParseDescriptors(elem.descriptor());
                for (int i = 0; i < (int)descs.size(); ++i) {
                    if (descs[i].tag() == 0x52) { // ストリーム識別記述子
                        StreamIdentifierDescriptor streamdesc(descs[i]);
                        if (streamdesc.parse()) {
                            int ct = streamdesc.component_tag();
                            if (ct == 0x30 || ct == 0x87) { // TvCaptionMod2を参考にした
                                captionEs.stype = stream_type;
                                captionEs.pid = elem.elementary_PID();
                            }
                            break;
                        }
                    }
                }
            }
        }
        if (videoEs.pid == -1) {
            // 映像ストリームがない
            ctx.warn("PMT 映像ストリームがありません");
            return;
        }
        if (audioEs.size() == 0) {
            ctx.warn("PMT オーディオストリームがありません");
        }

        // 
        PidHandlerTable *table = curHandlerTable;
        if (videoEs.pid != this->videoEs.pid) {
            // 映像ストリームが変わる場合
            waitingNewVideo = true;
            table = nextHandlerTable;
            if (this->videoEs.pid != -1) {
                ctx.info("PMT 映像ストリームの変更を検知");
            }
        }

        this->videoEs = videoEs;
        this->audioEs = audioEs;
        this->captionEs = captionEs;

        table->add(videoEs.pid, &videoDelegator);
        ensureAudioDelegators(int(audioEs.size()));
        for (int i = 0; i < int(audioEs.size()); ++i) {
            table->add(audioEs[i].pid, audioDelegators[i]);
        }
        if (captionEs.pid != -1) {
            table->add(captionEs.pid, &captionDelegator);
        }

        selectorHandler->onPmtUpdated(pmt.PCR_PID());
        if (table == curHandlerTable) {
            selectorHandler->onPidTableChanged(videoEs, audioEs, captionEs);
        }
    }
}

void TsPacketSelector::onTdtUpdated(int64_t clock, PsiSection section) {
    if (selectorHandler == NULL || clock == -1) {
        return;
    }
    switch (section.table_id()) {
    case 0x70: // TDT
    {
        TDT tdt(section);
        if (tdt.parse() && tdt.check()) {
            selectorHandler->onTime(clock, tdt.JST_time());
        }
    }
    break;
    case 0x73: // TOT
    {
        TOT tot(section);
        if (tot.parse() && tot.check()) {
            selectorHandler->onTime(clock, tot.JST_time());
        }
    }
    break;
    }
}

void TsPacketSelector::onVideoPacket(int64_t clock, TsPacket packet) {
    if (selectorHandler != NULL) {
        selectorHandler->onVideoPacket(clock, packet);
    }
}

void TsPacketSelector::onAudioPacket(int64_t clock, TsPacket packet, int audioIdx) {
    if (selectorHandler != NULL) {
        selectorHandler->onAudioPacket(clock, packet, audioIdx);
    }
}

void TsPacketSelector::onCaptionPacket(int64_t clock, TsPacket packet) {
    if (selectorHandler != NULL) {
        selectorHandler->onCaptionPacket(clock, packet);
    }
}

void TsPacketSelector::swapHandlerTable() {
    std::swap(curHandlerTable, nextHandlerTable);
    nextHandlerTable->clear();

    // PMTを引き継ぐ
    curHandlerTable->add(pmtPid, &PsiParserPMT);
}

void TsPacketSelector::ensureAudioDelegators(int numAudios) {
    while ((int)audioDelegators.size() < numAudios) {
        int audioIdx = int(audioDelegators.size());
        audioDelegators.push_back(new AudioDelegator(*this, audioIdx));
    }
}

bool TsPacketSelector::isVideo(uint8_t stream_type) {
    switch (stream_type) {
    case 0x02: // MPEG2-VIDEO
    case 0x1B: // H.264/AVC
        //	case 0x24: // H.265/HEVC
        return true;
    }
    return false;
}

bool TsPacketSelector::isAudio(uint8_t stream_type) {
    // AAC以外未対応
    return (stream_type == 0x0F);
}

bool TsPacketSelector::isCaption(uint8_t stream_type) {
    return (stream_type == 0x06);
}

/* static */ const char* TsPacketSelector::streamTypeString(int stream_type) {
    switch (stream_type) {
    case 0x00:
        return "ECM";
    case 0x02: // ITU-T Rec. H.262 and ISO/IEC 13818-2 (MPEG-2 higher rate interlaced video) in a packetized stream
        return "MPEG2-VIDEO";
    case 0x04: // ISO/IEC 13818-3 (MPEG-2 halved sample rate audio) in a packetized stream
        return "MPEG2-AUDIO";
    case 0x06: // ITU-T Rec. H.222 and ISO/IEC 13818-1 (MPEG-2 packetized data) privately defined (i.e., DVB subtitles / VBI and AC - 3)
        return "字幕";
    case 0x0D: // ISO/IEC 13818-6 DSM CC tabled data
        return "データカルーセル";
    case 0x0F: // ISO/IEC 13818-7 ADTS AAC (MPEG-2 lower bit-rate audio) in a packetized stream
        return "ADTS AAC";
    case 0x11: // ISO/IEC 14496-3 (MPEG-4 LOAS multi-format framed audio) in a packetized stream
        return "MPEG4 AAC";
    case 0x1B: // ITU-T Rec. H.264 and ISO/IEC 14496-10 (lower bit-rate video) in a packetized stream
        return "H.264/AVC";
    case 0x24: // ITU-T Rec. H.265 and ISO/IEC 23008-2 (Ultra HD video) in a packetized stream
        return "H.265/HEVC";
    }
    return nullptr;
}

/* static */ const char* TsPacketSelector::componentTagString(int component_tag) {
    if (component_tag >= 0x00 && component_tag <= 0x0F) return "MPEG2 映像";
    if (component_tag >= 0x10 && component_tag <= 0x2F) return "MPEG2 AAC 音声";
    if (component_tag >= 0x30 && component_tag <= 0x37) return "字幕";
    if (component_tag >= 0x38 && component_tag <= 0x3F) return "文字スーパー";
    if (component_tag >= 0x40 && component_tag <= 0x7F) return "データ";
    switch (component_tag) {
    case 0x81:
    case 0x82:
        return "簡易動画 映像";
    case 0x83:
    case 0x84:
    case 0x85:
    case 0x86:
    case 0x8C:
    case 0x8D:
    case 0x8E:
    case 0x8F:
        return "MPEG2 AAC 音声";
    case 0x87:
        return "字幕";
    case 0x80:
    case 0x8B:
        return "データ";
    case 0x89:
    case 0x8A:
        return "イベントメッセージ";
    }
    return nullptr;
}

void TsPacketSelector::printPMT(const PMT& pmt) {
    if (currentClock == -1 || startClock == -1) {
        ctx.info("[PMT更新]");
    } else {
        double sec = (currentClock - startClock) / 27000000.0;
        int minutes = (int)(sec / 60);
        sec -= minutes * 60;
        ctx.infoF("[PMT更新] ストリーム時刻: %d分%.2f秒", minutes, sec);
    }

    const char* content = NULL;
    for (int i = 0; i < pmt.numElems(); ++i) {
        PMTElement elem = pmt.get(i);
        // コンポーネントタグを取得
        int component_tag = -1;
        auto descs = ParseDescriptors(elem.descriptor());
        for (int i = 0; i < (int)descs.size(); ++i) {
            if (descs[i].tag() == 0x52) { // ストリーム識別記述子
                StreamIdentifierDescriptor streamdesc(descs[i]);
                if (streamdesc.parse()) {
                    component_tag = streamdesc.component_tag();
                }
            }
        }
        const char* content = streamTypeString(elem.stream_type());
        const char* tag = componentTagString(component_tag);
        if (content != NULL) {
            if (tag != NULL) {
                ctx.infoF("PID: 0x%04x TYPE: %s TAG: %s(0x%02x)", elem.elementary_PID(), content, tag, component_tag);
            } else if (component_tag != -1) {
                ctx.infoF("PID: 0x%04x TYPE: %s TAG: 不明(0x%02x)", elem.elementary_PID(), content, component_tag);
            } else {
                ctx.infoF("PID: 0x%04x TYPE: %s", elem.elementary_PID(), content);
            }
        } else {
            ctx.infoF("PID: 0x%04x TYPE: Unknown (0x%04x)", elem.elementary_PID(), elem.stream_type());
        }
    }
}
