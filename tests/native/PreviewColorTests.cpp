#include "../../Amatsukaze/PreviewColor.h"
#include <iostream>
#include <stdexcept>
#include <random>

namespace {
void check(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }

// ピッチの余白を異常値で埋め、10bitの黒・白・中間調を規格式の倍精度計算値と照合する。
void checkGray(trimadjust::PreviewToneMap mode) {
    const int codes[] = {64,256,502,721,940};
    const int mobius[] = {0,75,181,234,255}, bt2446[] = {0,42,100,171,255};
    trimadjust::PreviewColorConverter converter(mode);
    for (int i = 0; i < 5; i++) {
        uint16_t y[] = {uint16_t(codes[i]),uint16_t(codes[i]),65535,65535,
                        uint16_t(codes[i]),uint16_t(codes[i]),65535,65535};
        uint16_t u[] = {512,65535}, v[] = {512,65535};
        const uint8_t* planes[] = {reinterpret_cast<uint8_t*>(y),reinterpret_cast<uint8_t*>(u),reinterpret_cast<uint8_t*>(v)};
        const int pitch[] = {8,4,4};
        const auto& out = converter.convert(planes,pitch,2,2,10,true);
        const int expected = mode == trimadjust::PreviewToneMap::Mobius ? mobius[i] : bt2446[i];
        for (int j = 0; j < 4; j++) check(std::abs(int(out[j])-expected) <= 1,"HLGの無彩色の明るさが参照値と一致する");
        check(out[4] == 128 && out[5] == 128,"HLGの無彩色に色が付かない");
    }
}
void checkMatrix() {
    // 色差が同じで輝度だけ異なる2x2。期待値は独立した倍精度の2020逆行列と601行列で求める。
    for (int bits : {8,10,12,16}) {
        const int factor = 1 << (bits-8);
        uint16_t y16[] = {uint16_t(40*factor),uint16_t(95*factor),uint16_t(150*factor),uint16_t(220*factor)};
        uint16_t u16[] = {uint16_t(145*factor)}, v16[] = {uint16_t(115*factor)};
        uint8_t y8[] = {40,95,150,220}, u8[] = {145}, v8[] = {115};
        const uint8_t* planes[] = {bits == 8 ? y8 : reinterpret_cast<uint8_t*>(y16),
            bits == 8 ? u8 : reinterpret_cast<uint8_t*>(u16),bits == 8 ? v8 : reinterpret_cast<uint8_t*>(v16)};
        const int pitch[] = {bits == 8 ? 2 : 4,bits == 8 ? 1 : 2,bits == 8 ? 1 : 2};
        trimadjust::PreviewColorConverter converter;
        const auto& out = converter.convert(planes,pitch,2,2,bits,false);
        double cb = 0, cr = 0;
        auto quantize = [](double x) { return int((std::max)(0.,(std::min)(255.,x))+.5); };
        for (int i = 0; i < 4; i++) {
            const double y = (y8[i]-16.)/219., u=(145.-128.)/224., v=(115.-128.)/224.;
            const double r=y+2*(1-.2627)*v, b=y+2*(1-.0593)*u;
            const double g=(y-.2627*r-.0593*b)/.6780;
            const double jy=.299*r+.587*g+.114*b;
            check(std::abs(int(out[i])-quantize(jy*255)) <= 1,"非HLGの2020→601行列とレンジ変換が一致する");
            cb += (b-jy)/(2*(1-.114)); cr += (r-jy)/(2*(1-.299));
        }
        check(std::abs(int(out[4])-quantize(128+255*cb/4)) <= 1,"色差を丸める前に4画素で平均する");
        check(std::abs(int(out[5])-quantize(128+255*cr/4)) <= 1,"Crのブロック平均が一致する");
    }
}
void checkConstantLuminance() {
    // BT.2020一定輝度の原色を10bitへ丸めた値。非一定輝度の逆行列では一致しない。
    const uint16_t colors[][3] = {{505,280,960},{786,132,83},{247,960,403}};
    const int expected[][3] = {{76,85,255},{150,44,21},{29,255,107}};
    trimadjust::PreviewColorConverter converter;
    for (int i=0; i<3; i++) {
        uint16_t y[] = {colors[i][0],colors[i][0],colors[i][0],colors[i][0]};
        const uint8_t* planes[] = {reinterpret_cast<uint8_t*>(y),
            reinterpret_cast<const uint8_t*>(&colors[i][1]),reinterpret_cast<const uint8_t*>(&colors[i][2])};
        const int pitch[] = {4,2,2};
        const auto& out=converter.convert(planes,pitch,2,2,10,false,true);
        check(std::abs(int(out[0])-expected[i][0]) <= 2,"一定輝度の原色のYが一致する");
        check(std::abs(int(out[4])-expected[i][1]) <= 2,"一定輝度の原色のCbが一致する");
        check(std::abs(int(out[5])-expected[i][2]) <= 2,"一定輝度の原色のCrが一致する");
        // 一定輝度の例外経路も422を維持し、奇数高さで縦の隣接行を参照しない。
        uint16_t y422[]={colors[i][0],colors[i][0],colors[i][0],colors[i][0],colors[i][0],colors[i][0]};
        uint16_t u422[]={colors[i][1],colors[i][1],colors[i][1]},v422[]={colors[i][2],colors[i][2],colors[i][2]};
        const uint8_t* planes422[]={reinterpret_cast<uint8_t*>(y422),reinterpret_cast<uint8_t*>(u422),reinterpret_cast<uint8_t*>(v422)};
        const auto& out422=converter.convert(planes422,pitch,2,3,10,false,true,true);
        check(out422.size()==12,"一定輝度422のサイズが正しい");
        for(int row=0;row<3;row++) {
            check(std::abs(int(out422[row*2])-expected[i][0])<=2,"一定輝度422の各行のYが一致する");
            check(std::abs(int(out422[6+row])-expected[i][1])<=2,"一定輝度422の各行のCbが一致する");
            check(std::abs(int(out422[9+row])-expected[i][2])<=2,"一定輝度422の各行のCrが一致する");
        }
    }
}
void checkSimdAgreement() {
    if (!trimadjust::PreviewColorConverter::hasAvx2Fma()) {
        std::cout << "AVX2またはFMAが非対応のためHLG比較テストを省略\n";
        return;
    }
    std::mt19937 random(2446);
    auto run=[&](auto sampleTag,int bits) {
        using Sample=decltype(sampleTag);
        for(int width : {2,6,8,10,14,16,18,30,32,34,66}) for(int height : {2,6}) {
            // SIMD境界、右端の余り、非整列の行と異なるピッチをまとめて確認する。
            const int yPitch=width+5,uPitch=width/2+3,vPitch=width/2+7;
            std::vector<Sample> y(yPitch*height),u(uPitch*height/2),v(vPitch*height/2);
            const uint8_t* planes[]={reinterpret_cast<uint8_t*>(y.data()),reinterpret_cast<uint8_t*>(u.data()),reinterpret_cast<uint8_t*>(v.data())};
            const int pitch[]={int(yPitch*sizeof(Sample)),int(uPitch*sizeof(Sample)),int(vPitch*sizeof(Sample))};
            const unsigned mask=(1u<<bits)-1u;
            for(auto& value:y) value=Sample(random()&mask);
            for(auto& value:u) value=Sample(random()&mask);
            for(auto& value:v) value=Sample(random()&mask);
            trimadjust::PreviewColorConverter scalar(trimadjust::PreviewToneMap::BT2446APreview,false),simd;
            const auto& a=scalar.convert(planes,pitch,width,height,bits,true);
            const auto& b=simd.convert(planes,pitch,width,height,bits,true);
            check(a.size()==b.size(),"スカラーとAVX2の出力サイズが一致する");
            for(size_t i=0;i<a.size();i++) check(std::abs(int(a[i])-int(b[i]))<=1,
                "スカラーとAVX2の差は8bitの1段階以内に収まる");
        }
    };
    run(uint8_t{},8);
    for(int bits : {10,12,16}) run(uint16_t{},bits);
}
void checkIntegerConversion() {
    std::mt19937 random(709601);
    struct Matrix { trimadjust::PreviewMatrix matrix; double kr,kb; };
    const Matrix matrices[]={{trimadjust::PreviewMatrix::BT601,.299,.114},
        {trimadjust::PreviewMatrix::BT709,.2126,.0722},{trimadjust::PreviewMatrix::BT2020,.2627,.0593},
        {trimadjust::PreviewMatrix::FCC,.30,.11},{trimadjust::PreviewMatrix::SMPTE240M,.212,.087}};
    auto run=[&](auto sampleTag,int bits) {
        using Sample=decltype(sampleTag);
        const int factor=1<<(bits-8);
        const unsigned mask=(1u<<bits)-1u;
        for(const auto& matrix:matrices) for(bool yuv422:{false,true}) for(int width:{2,14,16,18,30,32,34}) {
            const int height=yuv422?3:6,chromaHeight=yuv422?1:2;
            const int yp=width+3,up=width/2+5,vp=width/2+7;
            std::vector<Sample> y(yp*height),u(up*height/chromaHeight),v(vp*height/chromaHeight);
            const uint8_t* planes[]={reinterpret_cast<uint8_t*>(y.data()),reinterpret_cast<uint8_t*>(u.data()),reinterpret_cast<uint8_t*>(v.data())};
            const int pitch[]={int(yp*sizeof(Sample)),int(up*sizeof(Sample)),int(vp*sizeof(Sample))};
            trimadjust::PreviewColorConverter scalar(trimadjust::PreviewToneMap::BT2446APreview,false),simd;
            // 全コード範囲の乱数に加え、限定レンジ端点と符号反転する16bit最大値を確認する。
            for(int pattern=0;pattern<6;pattern++) {
                for(auto& value:y) value=Sample(pattern==0 ? random()&mask : pattern==1 ? 0 : pattern==2 ? mask : (pattern==3?16:235)*factor);
                for(auto& value:u) value=Sample(pattern==0 ? random()&mask : pattern==1 ? 0 : pattern==2 ? mask : (pattern==4?240:16)*factor);
                for(auto& value:v) value=Sample(pattern==0 ? random()&mask : pattern==1 ? 0 : pattern==2 ? mask : (pattern==5?240:16)*factor);
                const auto& a=scalar.convertInteger(planes,pitch,width,height,bits,matrix.matrix,yuv422);
                const auto& b=simd.convertInteger(planes,pitch,width,height,bits,matrix.matrix,yuv422);
                check(a==b,"整数スカラーとAVX2は全バイト一致する");
                auto reference=[&](int x,int row) {
                    const double yy=(double(y[row*yp+x])/factor-16)/219;
                    const double uu=(double(u[(row/chromaHeight)*up+x/2])/factor-128)/224;
                    const double vv=(double(v[(row/chromaHeight)*vp+x/2])/factor-128)/224;
                    // 合成係数を使わず、独立した倍精度RGB経由の参照を計算する。
                    const double r=yy+2*(1-matrix.kr)*vv,b=yy+2*(1-matrix.kb)*uu;
                    const double g=(yy-matrix.kr*r-matrix.kb*b)/(1-matrix.kr-matrix.kb);
                    const double jy=.299*r+.587*g+.114*b;
                    return std::array<double,3>{255*jy,128+255*(b-jy)/1.772,128+255*(r-jy)/1.402};
                };
                auto quantize=[](double x) { return int((std::max)(0.,(std::min)(255.,x))+.5); };
                const size_t ySize=size_t(width)*height,cSize=ySize/(2*chromaHeight);
                for(int row=0;row<height;row++) for(int x=0;x<width;x++) {
                    const auto ref=reference(x,row);
                    check(std::abs(int(a[size_t(row)*width+x])-quantize(ref[0]))<=1,
                        "Q14の輝度は倍精度参照との差が1段階以内");
                    if(x%2==0 && row%chromaHeight==0) {
                        const size_t ci=size_t(row/chromaHeight)*(width/2)+x/2;
                        check(std::abs(int(a[ySize+ci])-quantize(ref[1]))<=1,"Q14のCbは倍精度参照との差が1段階以内");
                        check(std::abs(int(a[ySize+cSize+ci])-quantize(ref[2]))<=1,"Q14のCrは倍精度参照との差が1段階以内");
                    }
                }
            }
        }
    };
    run(uint8_t{},8);
    for(int bits:{10,12,14,16}) run(uint16_t{},bits);
}
}
int main() {
    try {
        checkGray(trimadjust::PreviewToneMap::Mobius);
        checkGray(trimadjust::PreviewToneMap::BT2446A);
        checkMatrix();
        checkConstantLuminance();
        checkSimdAgreement();
        checkIntegerConversion();
        for (auto mode : {trimadjust::PreviewToneMap::Mobius,trimadjust::PreviewToneMap::BT2446A,
                         trimadjust::PreviewToneMap::BT2446APreview}) {
            trimadjust::PreviewColorConverter converter(mode);
            uint16_t y[4],u=512,v=512;
            const uint8_t* planes[] = {reinterpret_cast<uint8_t*>(y),reinterpret_cast<uint8_t*>(&u),reinterpret_cast<uint8_t*>(&v)};
            const int pitch[] = {4,2,2};
            int previous = -1;
            for (int code = 0; code < 1024; code++) {
                for (auto& pixel : y) pixel=uint16_t(code);
                const auto& out=converter.convert(planes,pitch,2,2,10,true);
                check(out[0] >= previous,"黒未満・白超を含むHLG階調が単調増加する"); previous=out[0];
                check(out[4] == 128 && out[5] == 128,"全階調で無彩色を保持する");
            }
            for (int yy : {64,502,940}) for (int uu : {64,512,960}) for (int vv : {64,512,960}) {
                for(auto& pixel:y) pixel=uint16_t(yy);
                u=uint16_t(uu); v=uint16_t(vv);
                const auto& out=converter.convert(planes,pitch,2,2,10,true);
                const double yyOut=out[0]/255., cb=(out[4]-128.)/255., cr=(out[5]-128.)/255.;
                const double r=yyOut+1.402*cr, b=yyOut+1.772*cb, g=(yyOut-.299*r-.114*b)/.587;
                for (double channel : {r,g,b}) check(channel >= -2./255 && channel <= 1+2./255,
                    "HLGの色域外の入力は量子化誤差以内でSDR色域へ収まる");
            }
            bool rejected=false;
            try { converter.convert(planes,pitch,3,2,10,true); } catch(const std::invalid_argument&) { rejected=true; }
            check(rejected,"奇数寸法を拒否する");
        }
        {
            // HLG75%の無彩色は90nit。sRGBでは約243となり、最大白255には余裕が残る。
            trimadjust::PreviewColorConverter converter;
            uint16_t y[] = {721,721,721,721}, u=512,v=512;
            const uint8_t* planes[] = {reinterpret_cast<uint8_t*>(y),reinterpret_cast<uint8_t*>(&u),reinterpret_cast<uint8_t*>(&v)};
            const int pitch[] = {4,2,2};
            const auto& out=converter.convert(planes,pitch,2,2,10,true);
            check(std::abs(int(out[0])-243)<=1,"既定の補正はHLG基準白をSDRの90nitへ合わせる");
            check(out[4]==128 && out[5]==128,"基準白補正で色が付かない");
            int previous=out[0];
            for(int code=722;code<=940;code++) {
                for(auto& sample:y) sample=uint16_t(code);
                const int current=converter.convert(planes,pitch,2,2,10,true)[0];
                check(current>=previous,"基準白より明るい領域を逆転させない"); previous=current;
            }
            check(previous==255,"最大白まで階調を残す");
        }
        {
            // 色域内の淡い色では、補正前後の線形RGB比が量子化誤差以内で一致する。
            uint16_t y[] = {502,502,502,502},u=550,v=500;
            const uint8_t* planes[] = {reinterpret_cast<uint8_t*>(y),reinterpret_cast<uint8_t*>(&u),reinterpret_cast<uint8_t*>(&v)};
            const int pitch[] = {4,2,2};
            auto chromaticity = [](const std::vector<uint8_t>& data) {
                const double y=data[0]/255.,u=(data[4]-128.)/255.,v=(data[5]-128.)/255.;
                const double r=y+1.402*v,b=y+1.772*u,g=(y-.299*r-.114*b)/.587;
                std::array<double,3> rgb={r,g,b};
                for(auto& x:rgb) x=x<=.04045 ? x/12.92 : std::pow((x+.055)/1.055,2.4);
                const double total=rgb[0]+rgb[1]+rgb[2];
                for(auto& x:rgb) x/=total;
                return rgb;
            };
            trimadjust::PreviewColorConverter standard(trimadjust::PreviewToneMap::BT2446A), preview;
            const auto a=chromaticity(standard.convert(planes,pitch,2,2,10,true));
            const auto b=chromaticity(preview.convert(planes,pitch,2,2,10,true));
            for(int i=0;i<3;i++) check(std::abs(a[i]-b[i])<.015,"明るさ補正だけで色域内の色度を変えない");
        }
        check(std::abs(trimadjust::PreviewColorConverter::mobiusCurve(.3f)-.3f)<1e-6,"Mobiusの接続点を保持する");
        check(std::abs(trimadjust::PreviewColorConverter::mobiusCurve(10.f)-1.f)<1e-6,"1000nitをSDRの白へ写す");
        std::cout << "プレビュー色変換テスト成功\n";
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
