#include "../../Amatsukaze/LogoGopDecoder.h"
#include <cstdio>
#include <string>
extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

namespace {
using namespace logo::gopdecode;
void CheckTest(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct ParametersDeleter { void operator()(AVCodecParameters* p) const { avcodec_parameters_free(&p); } };
struct Source {
    std::unique_ptr<AVCodecParameters, ParametersDeleter> parameters{avcodec_parameters_alloc()};
    std::vector<Packet> packets;
};

Source Encode(bool closed) {
    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
    CheckTest(encoder != nullptr, "MPEG-2エンコーダーがない");
    Codec codec(avcodec_alloc_context3(encoder));
    codec->width = 128;
    codec->height = 96;
    codec->pix_fmt = AV_PIX_FMT_YUV420P;
    codec->time_base = AVRational{1, 25};
    codec->framerate = AVRational{25, 1};
    codec->gop_size = 12;
    codec->max_b_frames = 2;
    codec->bit_rate = 500000;
    codec->thread_count = 1;
    codec->flags |= AV_CODEC_FLAG_BITEXACT;
    if (closed) codec->flags |= AV_CODEC_FLAG_CLOSED_GOP;
    Check(av_opt_set_int(codec->priv_data, "sc_threshold", 1000000000, 0), "シーンチェンジ設定失敗");
    Check(avcodec_open2(codec.get(), encoder, nullptr), "テスト映像エンコーダー初期化失敗");
    Source source;
    Check(avcodec_parameters_from_context(source.parameters.get(), codec.get()), "テスト映像情報コピー失敗");
    Frame frame(av_frame_alloc());
    frame->format = codec->pix_fmt;
    frame->width = codec->width;
    frame->height = codec->height;
    Check(av_frame_get_buffer(frame.get(), 32), "テスト画像確保失敗");
    auto receive = [&]() {
        for (;;) {
            Packet packet(av_packet_alloc(), PacketDeleter());
            const int result = avcodec_receive_packet(codec.get(), packet.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
            Check(result, "テスト映像パケット受信失敗");
            packet->pos = int64_t(source.packets.size()) * 188;
            source.packets.push_back(std::move(packet));
        }
    };
    for (int i = 0; i < 137; i++) {
        Check(av_frame_make_writable(frame.get()), "テスト画像書き込み準備失敗");
        for (int plane = 0; plane < 3; plane++) {
            const int width = plane ? 64 : 128, height = plane ? 48 : 96;
            for (int y = 0; y < height; y++) for (int x = 0; x < width; x++) {
                frame->data[plane][y * frame->linesize[plane] + x] = uint8_t((x * 7 + y * 3 + i * 13 + plane * 59) & 255);
            }
        }
        frame->pts = i;
        Check(avcodec_send_frame(codec.get(), frame.get()), "テスト画像送信失敗");
        receive();
    }
    Check(avcodec_send_frame(codec.get(), nullptr), "テスト映像終端送信失敗");
    receive();
    return source;
}

struct Snapshot {
    int64_t pts;
    std::vector<uint8_t> pixels;
    bool operator==(const Snapshot& other) const { return pts == other.pts && pixels == other.pixels; }
};

Snapshot Copy(AVFrame* frame) {
    Snapshot out;
    out.pts = frame->pts;
    const auto format = AVPixelFormat(frame->format);
    out.pixels.resize(av_image_get_buffer_size(format, frame->width, frame->height, 1));
    Check(av_image_copy_to_buffer(out.pixels.data(), int(out.pixels.size()), frame->data, frame->linesize,
        format, frame->width, frame->height, 1), "テスト画像コピー失敗");
    return out;
}

std::vector<Snapshot> Sequential(const Source& source, size_t start = 0) {
    const AVCodec* decoder = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
    Codec codec(avcodec_alloc_context3(decoder));
    Check(avcodec_parameters_to_context(codec.get(), source.parameters.get()), "逐次デコーダー設定失敗");
    codec->thread_count = 2;
    Check(avcodec_open2(codec.get(), decoder, nullptr), "逐次デコーダー初期化失敗");
    Frame frame(av_frame_alloc());
    std::vector<Snapshot> out;
    auto receive = [&]() {
        for (;;) {
            const int result = avcodec_receive_frame(codec.get(), frame.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
            Check(result, "逐次フレーム受信失敗");
            out.push_back(Copy(frame.get()));
            av_frame_unref(frame.get());
        }
    };
    for (size_t i = start; i < source.packets.size(); i++) {
        Check(avcodec_send_packet(codec.get(), source.packets[i].get()), "逐次パケット送信失敗");
        receive();
    }
    Check(avcodec_send_packet(codec.get(), nullptr), "逐次終端送信失敗");
    receive();
    return out;
}

std::vector<Snapshot> Parallel(const Source& source, logo::LogoGopDecodeSettings settings, size_t start = 0,
    int stopAt = 100000, int readErrorAt = -1, int frameErrorAt = -1) {
    size_t next = start;
    std::vector<Snapshot> out;
    logo::RunLogoGopDecode(source.parameters.get(), AVRational{1,25}, settings,
        [&](AVPacket* packet) {
            if (int(next) == readErrorAt) throw std::runtime_error("入力例外の検査");
            if (next >= source.packets.size()) return false;
            Check(av_packet_ref(packet, source.packets[next++].get()), "入力パケット参照失敗");
            return true;
        }, [&](AVFrame* frame, int64_t) {
            if (int(out.size()) == frameErrorAt) throw std::runtime_error("出力例外の検査");
            out.push_back(Copy(frame));
            return int(out.size()) < stopAt;
        });
    return out;
}
}

int main() {
    try {
        // 対応範囲外のフィールドピクチャーを、画素を出力する前に判別する。
        for (int structure : {1,2,3}) {
            Packet packet(av_packet_alloc(), PacketDeleter());
            Check(av_new_packet(packet.get(), 8), "ヘッダー検査領域確保失敗");
            const uint8_t bytes[] = {0,0,1,0xb5,0x8f,0xff,uint8_t(0xfc | structure),0};
            std::memcpy(packet->data, bytes, sizeof(bytes));
            CheckTest(Inspect(*packet).fieldPicture == (structure != 3), "ピクチャー構造の判定が異なる");
        }
        for (bool closed : {false, true}) {
            Source source = Encode(closed);
            logo::LogoGopDecodeSettings originalSettings;
            originalSettings.gopsPerJob = 1;
            CheckTest(Parallel(source, originalSettings) == Sequential(source), "元の表示PTSと画素が一致しない");
            for (int timestamps = 0; timestamps < 3; timestamps++) {
                for (size_t i = 0; i < source.packets.size(); i++) {
                    // 画素の選別にtimestampを使わないことを、欠落・重複・wrap相当の値で確認する。
                    source.packets[i]->pts = timestamps == 0 ? int64_t(i) : timestamps == 1 ? AV_NOPTS_VALUE : (i < 60 ? (1LL << 33) - 20 + int64_t(i % 10) : int64_t(i % 10));
                    source.packets[i]->dts = timestamps == 1 ? AV_NOPTS_VALUE : int64_t(i);
                }
                const auto reference = Sequential(source);
                CheckTest(reference.size() == 137, "基準のフレーム数が異なる");
                for (int workers : {1,2,4}) for (int gops : {1,3,8}) {
                    logo::LogoGopDecodeSettings settings;
                    settings.decoders = workers;
                    settings.gopsPerJob = gops;
                    settings.outputQueueDepth = 1;
                    CheckTest(Parallel(source, settings) == reference, "表示順・PTS・全YUV画素が逐次版と異なる");
                }
            }
            logo::LogoGopDecodeSettings settings;
            settings.gopsPerJob = 1;
            settings.outputQueueDepth = 1;
            for (int stopAt : {1,14,40}) CheckTest(int(Parallel(source, settings, 0, stopAt).size()) == stopAt, "キャンセル位置が異なる");
            for (bool inputError : {false,true}) {
                bool caught = false;
                try { Parallel(source, settings, 0, 100000, inputError ? 50 : -1, inputError ? -1 : 17); }
                catch (const std::runtime_error& ex) { caught = std::string(ex.what()) == (inputError ? "入力例外の検査" : "出力例外の検査"); }
                CheckTest(caught, "例外を呼び出し元へ返していない");
            }
            source.packets.resize(2);
            CheckTest(Parallel(source, settings) == Sequential(source), "短い入力の終端が異なる");
            source.packets.clear();
            CheckTest(Parallel(source, settings).empty(), "空入力からフレームが出た");
        }
        std::puts("GOP並列復号: Open/Closed GOP、PTS欠落/不連続、端数、キャンセル、例外の検証成功");
        return 0;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "%s\n", ex.what());
        return 1;
    }
}
