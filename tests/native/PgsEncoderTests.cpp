#include "PgsEncoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace amatsukaze::pgs;

namespace {
void Check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
unsigned Read16(const std::vector<uint8_t>& bytes, size_t offset) {
    Check(offset + 2 <= bytes.size(), "16bitフィールドが途中で終了した");
    return (unsigned(bytes[offset]) << 8) | bytes[offset + 1];
}
uint32_t Read32(const std::vector<uint8_t>& bytes, size_t offset) {
    Check(offset + 4 <= bytes.size(), "32bitフィールドが途中で終了した");
    return (uint32_t(bytes[offset]) << 24) | (uint32_t(bytes[offset + 1]) << 16) |
        (uint32_t(bytes[offset + 2]) << 8) | bytes[offset + 3];
}
struct Segment {
    uint32_t pts;
    uint8_t type;
    std::vector<uint8_t> payload;
};
std::vector<Segment> Parse(const std::vector<uint8_t>& sup) {
    std::vector<Segment> segments;
    for (size_t pos = 0; pos < sup.size();) {
        Check(pos + 13 <= sup.size(), "SUPヘッダが途中で終了した");
        Check(sup[pos] == 'P' && sup[pos + 1] == 'G', "SUPマジック不一致");
        Check(Read32(sup, pos + 6) == 0, "DTSは0であること");
        const size_t end = pos + 13 + Read16(sup, pos + 11);
        Check(end <= sup.size(), "セグメント長がファイル末尾を超えた");
        segments.push_back({Read32(sup, pos + 2), sup[pos + 10],
            {sup.begin() + pos + 13, sup.begin() + end}});
        pos = end;
    }
    return segments;
}
struct Picture {
    uint32_t pts;
    int rectangles;
    std::vector<Rgba> pixels;
};
std::vector<Picture> Decode(const std::vector<uint8_t>& sup, int width, int height) {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_HDMV_PGS_SUBTITLE);
    Check(codec != nullptr, "FFmpegのpgssubデコーダがない");
    AVCodecContext* context = avcodec_alloc_context3(codec);
    Check(context != nullptr, "デコーダ確保失敗");
    struct Cleanup {
        AVCodecContext*& context;
        ~Cleanup() { avcodec_free_context(&context); }
    } cleanup{context};
    context->err_recognition = AV_EF_EXPLODE;
    context->width = width;
    context->height = height;
    Check(avcodec_open2(context, codec, nullptr) == 0, "デコーダ初期化失敗");
    std::vector<Picture> pictures;
    std::vector<uint8_t> packetBytes;
    for (const auto& segment : Parse(sup)) {
        packetBytes.push_back(segment.type);
        packetBytes.push_back(uint8_t(segment.payload.size() >> 8));
        packetBytes.push_back(uint8_t(segment.payload.size()));
        packetBytes.insert(packetBytes.end(), segment.payload.begin(), segment.payload.end());
        if (segment.type != 0x80) continue;
        AVPacket* packet = av_packet_alloc();
        Check(packet != nullptr, "パケット確保失敗");
        Check(av_new_packet(packet, int(packetBytes.size())) == 0, "パケット画像確保失敗");
        std::memcpy(packet->data, packetBytes.data(), packetBytes.size());
        packet->pts = segment.pts;
        AVSubtitle subtitle{};
        struct FreeSubtitle {
            AVSubtitle& subtitle;
            ~FreeSubtitle(){avsubtitle_free(&subtitle);}
        } freeSubtitle{subtitle};
        int got = 0;
        const int result = avcodec_decode_subtitle2(context, &subtitle, &got, packet);
        av_packet_free(&packet);
        Check(result >= 0, "pgssub復号失敗: " + std::to_string(result));
        if (got) {
            Picture picture{segment.pts, int(subtitle.num_rects), std::vector<Rgba>(size_t(width) * height, {0,0,0,0})};
            for (unsigned i = 0; i < subtitle.num_rects; i++) {
                const AVSubtitleRect* rect = subtitle.rects[i];
                Check(rect->type == SUBTITLE_BITMAP, "画像字幕ではない");
                Check(rect->x >= 0 && rect->y >= 0 && rect->x + rect->w <= width && rect->y + rect->h <= height,
                    "復号画像がキャンバス範囲外");
                for (int y = 0; y < rect->h; y++) for (int x = 0; x < rect->w; x++) {
                    const auto index = rect->data[0][size_t(y) * rect->linesize[0] + x];
                    uint32_t color;
                    std::memcpy(&color, rect->data[1] + size_t(index) * 4, 4);
                    picture.pixels[size_t(rect->y + y) * width + rect->x + x] =
                        {uint8_t(color >> 16), uint8_t(color >> 8), uint8_t(color), uint8_t(color >> 24)};
                }
            }
            pictures.push_back(std::move(picture));
        }
        packetBytes.clear();
    }
    Check(packetBytes.empty(), "ENDを持たない表示セット");
    return pictures;
}
Region Solid(int x, int y, int width, int height, Rgba color) {
    return {x, y, width, height, std::vector<Rgba>(size_t(width) * height, color)};
}
std::vector<Rgba> Canvas(int width, int height, const std::vector<Region>& regions) {
    std::vector<Rgba> pixels(size_t(width) * height, {0,0,0,0});
    for (const auto& region : regions) for (int y = 0; y < region.height; y++) for (int x = 0; x < region.width; x++) {
        const auto color = region.pixels[size_t(y) * region.width + x];
        auto& target = pixels[size_t(region.y + y) * width + region.x + x];
        const double sourceAlpha = color.a / 255.0;
        const double targetAlpha = target.a / 255.0;
        const double outputAlpha = sourceAlpha + targetAlpha * (1 - sourceAlpha);
        if (outputAlpha == 0) continue;
        const auto channel = [&](uint8_t source, uint8_t destination) {
            return uint8_t(std::lround((source * sourceAlpha + destination * targetAlpha * (1 - sourceAlpha)) / outputAlpha));
        };
        target = {channel(color.r,target.r),channel(color.g,target.g),channel(color.b,target.b),uint8_t(std::lround(outputAlpha*255))};
    }
    return pixels;
}
void Compare(const std::vector<Rgba>& expected, const std::vector<Rgba>& actual, bool quantized) {
    Check(expected.size() == actual.size(), "復号画素数不一致");
    double error = 0;
    size_t count = 0;
    int maxError = 0;
    for (size_t i = 0; i < expected.size(); i++) {
        const auto a = expected[i];
        const auto b = actual[i];
        Check((a.a == 0) == (b.a == 0), "透明画素の位置不一致: " + std::to_string(i));
        if (!a.a) continue;
        const int differences[] = {int(a.r)-b.r, int(a.g)-b.g, int(a.b)-b.b, int(a.a)-b.a};
        for (int d : differences) {
            maxError = std::max(maxError, std::abs(d));
            error += double(d) * d;
            count++;
        }
    }
    if (!quantized) Check(maxError <= 2, "255色以下の誤差が±2を超えた: " + std::to_string(maxError));
    else {
        Check(count != 0, "PSNRの評価画素がない");
        const double psnr = error == 0 ? 100 : 10 * std::log10(255.0 * 255.0 * count / error);
        std::printf("  減色PSNR: %.2fdB\n", psnr);
        Check(psnr >= 30.0, "減色PSNRが30dB未満");
    }
}
void CheckWindowDefinitions(const std::vector<Segment>& segments) {
    bool epoch = false;
    std::vector<uint8_t> epochWindows;
    for (const auto& segment : segments) {
        if (segment.type == 0x16) {
            Check(segment.payload.size() >= 11, "PCSヘッダが不足している");
            epoch = segment.payload[7] == 0x80;
            if (!epoch) Check(segment.payload[10] == 0, "消去PCSのオブジェクト数が0ではない");
        } else if (segment.type == 0x17) {
            if (epoch) epochWindows = segment.payload;
            else Check(segment.payload == epochWindows, "消去WDSが直前エポックの定義と異なる");
        }
    }
}
void RoundTrip(int height, const std::vector<Region>& regions, int rectangles, bool quantized = false) {
    const int width = 1920;
    const auto sup = PgsEncoder::Encode(width, height, {{90000, 180000, regions}});
    CheckWindowDefinitions(Parse(sup));
    const auto pictures = Decode(sup, width, height);
    Check(pictures.size() == 2, "開始と消去の復号結果は2件");
    Check(pictures[0].pts == 90000 && pictures[1].pts == 180000, "復号時刻不一致");
    Check(pictures[0].rectangles == rectangles, "領域結合後のオブジェクト数不一致");
    Compare(Canvas(width, height, regions), pictures[0].pixels, quantized);
    Check(pictures[1].rectangles == 0, "終了時に字幕が消去されない");
    Check(std::all_of(pictures[1].pixels.begin(), pictures[1].pixels.end(),
        [](const Rgba& color) { return color.a == 0; }), "消去後に画像が残っている");
}
void Colors() {
    for (int height : {480, 576, 577, 1080}) {
        Region region = Solid(7, 11, 64, 8, {0,0,0,255});
        const Rgba colors[] = {{255,0,0,255}, {0,255,0,255}, {0,0,255,255}, {255,255,255,255},
            {0,0,0,255}, {128,128,128,255}, {30,70,170,128}, {230,180,20,64}};
        for (size_t i = 0; i < region.pixels.size(); i++) region.pixels[i] = colors[i % 8];
        RoundTrip(height, {region}, 1);
    }
}
void Gradient() {
    Region region = Solid(30, 40, 128, 4, {255,255,255,255});
    for (int x = 0; x < region.width; x++) for (int y = 0; y < region.height; y++)
        region.pixels[size_t(y) * region.width + x] = {230,210,170,uint8_t(x * 2 + 1)};
    RoundTrip(1080, {region}, 1);
    region = Solid(30, 40, 64, 64, {0,0,0,255});
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++)
        region.pixels[size_t(y) * 64 + x] = {uint8_t(x * 4),uint8_t(y * 4),uint8_t((x+y)*2),uint8_t(128 + (x % 32)*4)};
    RoundTrip(1080, {region}, 1, true);
}
void Regions() {
    const Region a = Solid(10,20,20,10,{240,40,40,255});
    const Region b = Solid(80,50,20,10,{40,240,40,255});
    RoundTrip(480, {a,b}, 2);
    RoundTrip(1080, {a,b,Solid(105,50,10,10,{40,40,240,255})}, 2);
    RoundTrip(480, {a,Solid(20,25,20,10,{40,240,40,255})}, 1);
    RoundTrip(1080, {Solid(10,20,20,10,{240,40,40,80}),Solid(20,25,20,10,{40,240,40,120})}, 1);
    Region trim = Solid(100,100,16,10,{99,88,77,0});
    for (int y = 3; y < 7; y++) for (int x = 4; x < 12; x++)
        trim.pixels[size_t(y)*16+x] = {255,255,255,255};
    const auto sup = PgsEncoder::Encode(1920,1080,{{90000,180000,{trim}}});
    const auto segments = Parse(sup);
    const auto ods = std::find_if(segments.begin(),segments.end(),[](const Segment& s){return s.type==0x15;});
    Check(ods != segments.end() && Read16(ods->payload,7)==8 && Read16(ods->payload,9)==4,
        "透明な行列がトリミングされていない");
    RoundTrip(1080,{trim},1);
}
std::vector<uint8_t> ReadRle(const std::vector<uint8_t>& rle, int width, int height) {
    std::vector<uint8_t> pixels;
    size_t pos = 0;
    for (int y = 0; y < height; y++) {
        size_t column = 0;
        bool ended = false;
        while (pos < rle.size()) {
            unsigned color = rle[pos++];
            unsigned length = 1;
            if (color == 0) {
                Check(pos < rle.size(), "RLE制御バイト欠落");
                unsigned flags = rle[pos++];
                if (flags == 0) { ended = true; break; }
                length = flags & 0x3f;
                if (flags & 0x40) {
                    Check(pos < rle.size(), "RLE長さの下位バイト欠落");
                    length = (length << 8) | rle[pos++];
                }
                if (flags & 0x80) {
                    Check(pos < rle.size(), "RLE色バイト欠落");
                    color = rle[pos++];
                }
            }
            Check(length != 0 && column + length <= size_t(width), "RLEが行の範囲を超えた");
            pixels.insert(pixels.end(),length,uint8_t(color));
            column += length;
        }
        Check(ended && column == size_t(width), "RLE行末または行幅不一致");
    }
    Check(pos == rle.size(), "RLE末尾に余剰データ");
    return pixels;
}
void Rle() {
    for (int width : {1,63,64,16383,16384}) for (uint8_t color : {uint8_t(0),uint8_t(7)}) {
        const std::vector<uint8_t> pixels(size_t(width)*2,color);
        const auto rle = PgsEncoder::EncodeRle(pixels,width,2);
        Check(ReadRle(rle,width,2)==pixels, "RLE境界往復不一致");
    }
    struct GoldenRle { int width; uint8_t color; std::vector<uint8_t> expected; };
    const GoldenRle golden[] = {
        {1,7,{7,0,0}}, {1,0,{0,1,0,0}}, {63,7,{0,0xbf,7,0,0}},
        {64,7,{0,0xc0,0x40,7,0,0}}, {16383,7,{0,0xff,0xff,7,0,0}},
        {16384,7,{0,0xff,0xff,7,7,0,0}}, {64,0,{0,0x40,0x40,0,0}}
    };
    for (const auto& item : golden)
        Check(PgsEncoder::EncodeRle(std::vector<uint8_t>(item.width,item.color),item.width,1)==item.expected,
            "RLEゴールデン不一致: " + std::to_string(item.width));
    const std::vector<uint8_t> pixels{1,0,0,2,2,3,0,3,3,0};
    Check(ReadRle(PgsEncoder::EncodeRle(pixels,5,2),5,2)==pixels,"混在RLE往復不一致");
}
void RleCompatibility() {
    const auto reference = [](const std::vector<uint8_t>& pixels, int width, int height) {
        std::vector<uint8_t> encoded;
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width;) {
                const uint8_t color = pixels[size_t(y) * width + x];
                int length = 1;
                while (length < 16383 && x + length < width && pixels[size_t(y) * width + x + length] == color) length++;
                if (color && length <= 2) encoded.insert(encoded.end(), length, color);
                else {
                    encoded.push_back(0);
                    const uint8_t flags = color ? 0x80 : 0;
                    if (length < 64) encoded.push_back(uint8_t(flags | length));
                    else { encoded.push_back(uint8_t(flags | 0x40 | (length >> 8))); encoded.push_back(uint8_t(length)); }
                    if (color) encoded.push_back(color);
                }
                x += length;
            }
            encoded.insert(encoded.end(), 2, 0);
        }
        return encoded;
    };
    uint32_t random = 12345;
    for (int width : {1,7,8,31,32,33,63,64,65,1919,1920,16383,16384}) {
        std::vector<uint8_t> pixels(size_t(width) * 4);
        for (size_t start = 0; start < pixels.size();) {
            random = random * 1664525u + 1013904223u;
            const size_t count = std::min(size_t((random >> 8) % 96 + 1), pixels.size() - start);
            std::fill_n(pixels.begin() + start, count, uint8_t(random & 7));
            start += count;
        }
        Check(PgsEncoder::EncodeRle(pixels,width,4)==reference(pixels,width,4),"SIMDのRLEが逐次参照とバイト不一致");
    }
}
void ParallelCompatibility() {
    std::vector<Event> events;
    for (int i = 0; i < 7; i++) {
        auto region = Solid(10,20,1025,64,{77,88,99,0});
        for (int y = 1; y < 63; y++) for (int x = 3; x < 1022; x++)
            region.pixels[size_t(y) * region.width + x] = {uint8_t(i*20),90,180,uint8_t((x+y)%3 ? 255 : 128)};
        events.push_back({90000 + int64_t(i)*180000,180000 + int64_t(i)*180000,{std::move(region)}});
    }
    const auto check = [&] {
        std::vector<uint8_t> expected;
        uint16_t number = 0;
        for (const auto& event : events) {
            auto single = PgsEncoder::Encode(1920,1080,{event});
            for (size_t pos = 0; pos < single.size();) {
                if (single[pos+10] == 0x16) {
                    single[pos+18] = uint8_t(number >> 8); single[pos+19] = uint8_t(number);
                    number++;
                }
                pos += 13 + Read16(single,pos+11);
            }
            expected.insert(expected.end(),single.begin(),single.end());
        }
        Check(PgsEncoder::Encode(1920,1080,events)==expected,"イベント並列化が逐次出力とバイト不一致");
    };
    check();
    // 隣接イベントでは、前イベントの消去を省略した連番を確認する。
    for (size_t i = 1; i < events.size(); i++) events[i].start90k = events[i-1].end90k;
    const auto segments = Parse(PgsEncoder::Encode(1920,1080,events));
    unsigned number = 0;
    for (const auto& segment : segments) if (segment.type == 0x16)
        Check(Read16(segment.payload,5)==number++,"並列処理でcomposition_numberが乱れた");
    Check(number==events.size()+1,"並列処理で隣接イベントの消去省略が崩れた");
    events[3].regions[0].pixels.pop_back();
    bool threw = false;
    try { PgsEncoder::Encode(1920,1080,events); } catch (const std::invalid_argument&) { threw = true; }
    Check(threw,"並列処理中の不正画像の例外が伝播しない");
}
void Ods() {
    Region region = Solid(20,30,512,256,{0,0,0,255});
    for (int y=0;y<region.height;y++) for(int x=0;x<region.width;x++)
        region.pixels[size_t(y)*region.width+x] = x%2 ? Rgba{255,255,255,255} : Rgba{0,0,0,255};
    const auto segments = Parse(PgsEncoder::Encode(1920,1080,{{90000,180000,{region}}}));
    std::vector<Segment> ods;
    for(const auto& s:segments) if(s.type==0x15) ods.push_back(s);
    Check(ods.size()>=3,"大きなODSが分割されていない");
    size_t encodedLength=0;
    const auto& first=ods.front().payload;
    Check(first.size()>=11,"ODS先頭の幅高さフィールド欠落");
    const unsigned declared=(unsigned(first[4])<<16)|(unsigned(first[5])<<8)|first[6];
    for(size_t i=0;i<ods.size();i++){
        const auto& p=ods[i].payload;
        Check(p.size()<=65535 && p.size()>=4,"ODS断片長が不正");
        Check((p[3]&0xc0)==(uint8_t((i==0?0x80:0)|(i+1==ods.size()?0x40:0))),"ODS first/lastフラグ不一致");
        Check(Read16(p,0)==Read16(first,0) && p[2]==first[2],"ODS断片のID/version不一致");
        encodedLength+=p.size()-(i==0?7:4);
    }
    Check(encodedLength==declared,"object_data_lengthは幅高さ4byteを含むこと");
    RoundTrip(1080,{region},1);
}
void Golden() {
    const auto segments=Parse(PgsEncoder::Encode(720,480,{{90000,180000,{Solid(10,20,2,1,{255,255,255,255})}}}));
    const std::vector<uint8_t> types{0x16,0x17,0x14,0x15,0x80,0x16,0x17,0x80};
    Check(segments.size()==types.size(),"固定ケースのセグメント数不一致");
    for(size_t i=0;i<types.size();i++) {
        Check(segments[i].type==types[i],"表示セット順序不一致");
        Check(segments[i].pts==(i<5?90000u:180000u),"SUP時刻不一致");
    }
    Check(segments[0].payload==std::vector<uint8_t>({0x02,0xd0,0x01,0xe0,0x10,0,0,0x80,0,0,1,0,0,0,0,0,10,0,20}),"開始PCSゴールデン不一致");
    Check(segments[1].payload==std::vector<uint8_t>({1,0,0,10,0,20,0,2,0,1}),"開始WDSゴールデン不一致");
    Check(segments[4].payload.empty() && segments[7].payload.empty(),"ENDペイロードは空であること");
    Check(segments[5].payload==std::vector<uint8_t>({0x02,0xd0,0x01,0xe0,0x10,0,1,0,0,0,0}),"消去PCSゴールデン不一致");
    Check(segments[6].payload==std::vector<uint8_t>({1,0,0,10,0,20,0,2,0,1}),"消去WDSゴールデン不一致");
    CheckWindowDefinitions(segments);
    const Region region=Solid(10,20,2,1,{255,255,255,255});
    const auto adjacent=Parse(PgsEncoder::Encode(720,480,{{90000,180000,{region}},{180000,270000,{region}}}));
    int pcsCount=0;
    for(const auto& s:adjacent) if(s.type==0x16){
        Check(Read16(s.payload,5)==unsigned(pcsCount++),"composition_number連番不一致");
        if(s.pts==180000) Check(s.payload[10]==1 && s.payload[7]==0x80,"同時刻に不要な消去PCS");
    }
    Check(pcsCount==3,"隣接イベントでは消去を省略すること");
    CheckWindowDefinitions(adjacent);
    // エポックごとにウィンドウ数・座標が変わっても、消去はそのエポックの定義を繰り返す。
    const auto changing = PgsEncoder::Encode(720,480,{{90000,180000,{region}},
        {270000,360000,{Solid(30,40,3,2,{255,0,0,255}),Solid(80,90,4,3,{0,255,0,255})}},
        {450000,540000,{}}});
    CheckWindowDefinitions(Parse(changing));
    const auto pictures = Decode(changing,720,480);
    Check(pictures.size()==6,"複数エポックの表示または消去が欠落した");
    Check(pictures[0].rectangles==1 && pictures[2].rectangles==2,"変更したウィンドウの表示に失敗した");
    for (size_t i : {size_t(1),size_t(3),size_t(4),size_t(5)}) {
        Check(pictures[i].rectangles==0,"消去または空エポックで画像が残った");
        Check(std::all_of(pictures[i].pixels.begin(),pictures[i].pixels.end(),
            [](const Rgba& color){return color.a==0;}),"消去後に非透明画素が残った");
    }
}
void PaletteBoundary() {
    Region region=Solid(10,20,255,1,{0,0,0,255});
    for (int x=0;x<255;x++) region.pixels[x]={uint8_t(x),90,140,255};
    const auto segments=Parse(PgsEncoder::Encode(1920,1080,{{90000,180000,{region}}}));
    const auto palette=std::find_if(segments.begin(),segments.end(),[](const Segment& item){return item.type==0x14;});
    Check(palette!=segments.end() && palette->payload.size()==2+256*5,"255色は減色せず透明色とともに保持すること");
    Check(palette->payload[2]==0 && palette->payload[6]==0,"パレットindex0は完全透明であること");
    RoundTrip(1080,{region},1);
    Region a=Solid(10,20,128,1,{0,0,0,255});
    Region b=Solid(10,30,128,1,{0,0,0,255});
    for(int x=0;x<128;x++){a.pixels[x]={uint8_t(x),90,140,255};b.pixels[x]={uint8_t(x+128),90,140,255};}
    RoundTrip(1080,{a,b},2,true);
    for(const auto& segment:Parse(PgsEncoder::Encode(1920,1080,{{90000,180000,{a,b}}})))
        if(segment.type==0x14) Check(segment.payload.size()<=2+256*5,"2領域の共有パレットが256色を超えた");
}
void PaletteNearestCompatibility() {
    const auto coordinates = [](const Rgba& color) {
        const double opacity = color.a / 255.0;
        return std::array<double,4>{color.r*opacity,color.g*opacity,color.b*opacity,double(color.a)};
    };
    uint32_t random = 0x5274631u;
    for (int trial = 0; trial < 12; trial++) {
        std::vector<std::vector<Rgba>> images(2, std::vector<Rgba>(37*31));
        for (auto& image : images) for (size_t i = 0; i < image.size(); i++) {
            random = random * 1664525u + 1013904223u;
            image[i] = {uint8_t(random),uint8_t(random>>8),uint8_t(random>>16),
                uint8_t(i%16 == 0 ? 0 : (trial%3 == 0 ? 255 : 1+(random>>24)%255))};
            if (i%7 == 0 && i) image[i] = image[i-1];
        }
        const auto palette = MakePalette(images,{{37,31},{37,31}},trial%2 ? 480 : 1080);
        Check(palette.entries.size()==256,"最近傍互換テストで255色への減色が発生していない");
        std::vector<std::array<double,4>> positions;
        for (const auto& entry : palette.entries) positions.push_back(coordinates(entry.rgba));
        for (size_t image = 0; image < images.size(); image++) for (size_t pixel = 0; pixel < images[image].size(); pixel++) {
            const auto color = images[image][pixel];
            uint8_t expected = 0;
            if (color.a) {
                const auto position = coordinates(color);
                double best = std::numeric_limits<double>::max();
                for (size_t index = 1; index < positions.size(); index++) {
                    double distance = 0;
                    for (size_t axis = 0; axis < 4; axis++) {
                        const double difference = position[axis] - positions[index][axis];
                        // FMAで積和をまとめず、従来の軸順の乗算・加算を参照にする。
                        volatile double squared = difference * difference;
                        distance += squared;
                    }
                    if (distance < best) { best = distance; expected = uint8_t(index); }
                }
            }
            Check(palette.images[image].pixels[pixel]==expected,"減色の最近傍indexが逐次参照と不一致");
        }
    }
}
template<class Function> void Throws(Function action, const char* message) {
    bool threw=false;
    try { action(); } catch(const std::exception&) { threw=true; }
    Check(threw,message);
}
void Validation() {
    const Region region=Solid(10,20,2,1,{255,255,255,255});
    for(int dimension:{0,-1,4097,65536,std::numeric_limits<int>::min()}) {
        Throws([&]{PgsEncoder::Encode(dimension,480,{});},"不正なキャンバス幅を受理した");
        Throws([&]{PgsEncoder::Encode(720,dimension,{});},"不正なキャンバス高さを受理した");
    }
    std::vector<Region> invalid;
    Region modified=region;modified.x=-1;invalid.push_back(modified);
    modified=region;modified.y=-1;invalid.push_back(modified);
    modified=region;modified.x=719;invalid.push_back(modified);
    modified=region;modified.y=480;invalid.push_back(modified);
    modified=region;modified.width=std::numeric_limits<int>::min();invalid.push_back(modified);
    modified=region;modified.width=0;invalid.push_back(modified);
    modified=region;modified.height=-1;invalid.push_back(modified);
    modified=region;modified.pixels.pop_back();invalid.push_back(modified);
    modified=region;modified.pixels.push_back({1,2,3,4});invalid.push_back(modified);
    for(const auto& bad:invalid) Throws([&]{PgsEncoder::Encode(720,480,{{0,1,{bad}}});},"不正な領域を受理した");
    Throws([&]{PgsEncoder::Encode(720,480,{{-1,1,{region}}});},"負の開始時刻を受理した");
    Throws([&]{PgsEncoder::Encode(720,480,{{1,1,{region}}});},"長さ0のイベントを受理した");
    Throws([&]{PgsEncoder::Encode(720,480,{{2,1,{region}}});},"負の表示区間を受理した");
    Throws([&]{PgsEncoder::Encode(720,480,{{0,2,{region}},{1,3,{region}}});},"重複イベントを受理した");
    Throws([&]{PgsEncoder::Encode(720,480,{{2,3,{region}},{0,1,{region}}});},"非時系列イベントを受理した");
    Throws([&]{PgsEncoder::Encode(4096,480,{{0,1,{Solid(0,0,4097,1,{255,255,255,255})}}});},"幅4096超のオブジェクトを受理した");
    Throws([&]{PgsEncoder::Encode(720,4096,{{0,1,{Solid(0,0,1,4097,{255,255,255,255})}}});},"高さ4096超の領域を受理した");
    Throws([&]{PgsEncoder::EncodeRle({1},2,1);},"不正なRLE画素数を受理した");
}
void EmptyAndWrap() {
    Check(PgsEncoder::Encode(720,480,{}).empty(),"イベントなしでSUPが生成された");
    for(const auto& regions:std::vector<std::vector<Region>>{{},{Solid(10,20,10,10,{100,90,80,0})}}) {
        const auto sup=PgsEncoder::Encode(720,480,{{90000,180000,regions}});
        const auto emptySegments=Parse(sup);
        CheckWindowDefinitions(emptySegments);
        Check(emptySegments.size()==6 && emptySegments[0].type==0x16 && emptySegments[0].payload.size()==11
            && emptySegments[0].payload[7]==0x80,"空画像のEpoch Start表示セット欠落");
        for(const auto& segment:emptySegments) {
            Check(segment.type==0x16 || segment.type==0x17 || segment.type==0x80,"空画像にパレットまたはODSを出した");
            if(segment.type==0x16) Check(segment.payload[10]==0,"透明画像にオブジェクトを出した");
            if(segment.type==0x17) Check(segment.payload==std::vector<uint8_t>({0}),"透明画像にウィンドウを出した");
        }
        const auto emptyPictures=Decode(sup,720,480);
        Check(emptyPictures.size()==2,"空画像の表示と消去の復号結果欠落");
        for(const auto& picture:emptyPictures) Check(picture.rectangles==0,"全透明イベントの復号で領域を出した");
    }
    const int64_t time=int64_t(std::numeric_limits<uint32_t>::max())+123;
    const auto segments=Parse(PgsEncoder::Encode(720,480,{{time,time+90,{Solid(10,20,2,1,{255,255,255,255})}}}));
    for(size_t i=0;i<segments.size();i++)
        Check(segments[i].pts==uint32_t(i<5?time:time+90),"32bitPTS折り返し不一致");
    Check(segments.front().pts==uint32_t(time) && segments.back().pts==uint32_t(time+90),"表示区間の32bitPTS折り返し不一致");
    std::vector<Event> events;
    for(int i=0;i<65538;i++) events.push_back({i,i+1,{}});
    unsigned composition=0;
    for(const auto& s:Parse(PgsEncoder::Encode(720,480,events))) if(s.type==0x16) {
        Check(Read16(s.payload,5)==uint16_t(composition),"composition_numberの16bit折り返し不一致");
        composition++;
    }
    Check(composition>=65538,"composition_number折り返しを実行できていない");
}
void FileOutput() {
    const auto name="amatsukaze-pgs-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".sup";
    const auto path=std::filesystem::temp_directory_path()/name;
    struct RemoveFile {
        std::filesystem::path path;
        ~RemoveFile(){std::error_code error;std::filesystem::remove(path,error);}
    } cleanup{path};
    const std::vector<Event> events{{90000,180000,{Solid(10,20,2,1,{255,255,255,255})}}};
    PgsEncoder::WriteFile(path,720,480,events);
    std::ifstream file(path,std::ios::binary);
    Check(bool(file),"WriteFile出力を開けない");
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
    Check(bytes==PgsEncoder::Encode(720,480,events),"WriteFileとメモリ出力が一致しない");
    Throws([&]{PgsEncoder::WriteFile(path.parent_path()/(name+"-missing")/"missing.sup",720,480,events);},"存在しない親ディレクトリへの書き込みが成功した");
}
}
int main() {
    struct Test { const char* name; void (*run)(); };
    const Test tests[]={{"colors",Colors},{"gradient",Gradient},{"regions",Regions},{"rle",Rle},{"rle_compatibility",RleCompatibility},{"parallel_compatibility",ParallelCompatibility},{"ods",Ods},{"golden",Golden},{"palette_boundary",PaletteBoundary},{"palette_nearest_compatibility",PaletteNearestCompatibility},{"validation",Validation},{"empty_wrap",EmptyAndWrap},{"file_output",FileOutput}};
    int failed=0;
    for(const auto& test:tests) {
        try { test.run(); std::printf("[PASS] pgs_%s\n",test.name); }
        catch(const std::exception& e){failed++;std::fprintf(stderr,"[FAIL] pgs_%s: %s\n",test.name,e.what());}
    }
    std::printf("実行: %zu件, 失敗: %d件\n",sizeof(tests)/sizeof(tests[0]),failed);
    return failed?1:0;
}
