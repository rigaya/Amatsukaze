#pragma once

#include <cstdint>
#include <vector>
#include <stdexcept>

// LOAS/LATMの単一program/layer音声を、AACの圧縮データを変えずADTSへ包み直す。
// 複数layerやADTSで表現できない構成は、音声を壊して続行せず明示的に拒否する。
class LatmToAdts {
    class Bits {
        const uint8_t* data;
        size_t size;
        size_t pos = 0;
    public:
        Bits(const uint8_t* data, size_t size) : data(data), size(size * 8) {}
        unsigned read(unsigned n) {
            if (n > 32 || n > size - pos) throw std::runtime_error("LATM音声のフレームが途中で切れています");
            unsigned value = 0;
            for (unsigned i = 0; i < n; i++, pos++) value = (value << 1) | ((data[pos / 8] >> (7 - pos % 8)) & 1);
            return value;
        }
    };
    std::vector<uint8_t> pending;
    bool latm = false;
    bool detected = false;
    bool configured = false;
    unsigned profile = 0, frequency = 0, channels = 0, subframes = 0;
    int64_t pendingPTS = -1;
    static void require(bool condition) {
        if (!condition) throw std::runtime_error("このLATM音声構成はADTSへの無劣化変換に対応していません");
    }
    void convert(const uint8_t* data, size_t size, std::vector<uint8_t>& output) {
        Bits bits(data, size);
        if (!bits.read(1)) {
            configured = false;
            // audioMuxVersion=0、同期した単一program/layerのみを扱う。
            require(bits.read(1) == 0);
            require(bits.read(1) == 1);
            subframes = bits.read(6);
            // 複数サブフレームはPES跨ぎのPTSを一意に対応させられないため拒否する。
            require(subframes == 0);
            require(bits.read(4) == 0);
            require(bits.read(3) == 0);
            const unsigned objectType = bits.read(5);
            frequency = bits.read(4);
            channels = bits.read(4);
            // AAC Main/LC/SSR/LTPの通常の1024サンプル構成。
            require(objectType >= 1 && objectType <= 4 && frequency < 13 && channels > 0 && channels <= 7);
            profile = objectType - 1;
            require(bits.read(1) == 0); // frameLengthFlag
            require(bits.read(1) == 0); // dependsOnCoreCoder
            require(bits.read(1) == 0); // extensionFlag
            require(bits.read(3) == 0); // frameLengthType
            bits.read(8); // latmBufferFullness
            require(bits.read(1) == 0); // otherDataPresent
            if (bits.read(1)) bits.read(8); // crcCheckPresent
            configured = true;
        }
        // 録画先頭が設定のないフレームの場合、次のStreamMuxConfigまで待つ。
        if (!configured) return;
        for (unsigned frame = 0; frame <= subframes; frame++) {
            unsigned length = 0, part;
            do { part = bits.read(8); length += part; } while (part == 255);
            constexpr unsigned HEADER_SIZE = 7;
            constexpr unsigned MAX_ADTS_SIZE = 8191;
            require(length > 0 && length <= MAX_ADTS_SIZE - HEADER_SIZE);
            const unsigned total = length + HEADER_SIZE;
            const uint8_t header[] = { 0xff, 0xf1,
                static_cast<uint8_t>((profile << 6) | (frequency << 2) | (channels >> 2)),
                static_cast<uint8_t>(((channels & 3) << 6) | (total >> 11)),
                static_cast<uint8_t>(total >> 3), static_cast<uint8_t>((total << 5) | 0x1f), 0xfc };
            output.insert(output.end(), header, header + HEADER_SIZE);
            for (unsigned i = 0; i < length; i++) output.push_back(static_cast<uint8_t>(bits.read(8)));
        }
    }
public:
    void reset() { pending.clear(); latm = configured = detected = false; pendingPTS = -1; }
    // outputとPTSは、この呼び出しで完成したフレーム列を表す。
    void input(const uint8_t* data, size_t size, int64_t& pts, std::vector<uint8_t>& output, bool* continuedFrame = nullptr) {
        output.clear();
        const bool hadPending = !pending.empty();
        if (continuedFrame) *continuedFrame = hadPending;
        if (!hadPending) pendingPTS = pts;
        if (size) pending.insert(pending.end(), data, data + size);
        if (pending.size() < 2) { pts = pendingPTS; return; }
        // 判定は最初の入力だけで行う。ADTSフレーム途中から始まるPESでは、
        // 圧縮データ中の偶然のLOAS同期語を形式変更と誤認してはいけない。
        if (!detected) {
            latm = pending[0] == 0x56 && (pending[1] & 0xe0) == 0xe0;
            detected = true;
        }
        if (!latm) { output.swap(pending); pts = pendingPTS; return; }
        const int64_t inputPTS = pts;
        pts = pendingPTS;
        size_t consumed = 0;
        while (pending.size() - consumed >= 3) {
            const uint8_t* frame = pending.data() + consumed;
            if (frame[0] != 0x56 || (frame[1] & 0xe0) != 0xe0) { consumed++; configured = false; continue; }
            const size_t length = ((frame[1] & 0x1f) << 8) | frame[2];
            if (pending.size() - consumed < length + 3) break;
            convert(frame + 3, length, output);
            consumed += length + 3;
        }
        pending.erase(pending.begin(), pending.begin() + consumed);
        if (consumed) pendingPTS = inputPTS;
    }
};
