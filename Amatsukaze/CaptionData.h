#pragma once

/*
* ARIB Caption
* このソースコードはTVCaptionMod2やCaption.dllのコードを流用しています。
* ライセンスはオリジナル版の以下ライセンスに従います。
------引用開始------
●EpgDataCap_Bon、TSEpgView_Sample、NetworkRemocon、Caption、TSEpgViewServerの
ソースの取り扱いについて
　特にGPLとかにはしないので自由に改変してもらって構わないです。
　改変して公開する場合は改変部分のソースぐらいは一緒に公開してください。
　（強制ではないので別に公開しなくてもいいです）
　EpgDataCap.dllの使い方の参考にしてもらうといいかも。

●EpgDataCap.dll、Caption.dllの取り扱いについて
    フリーソフトに組み込む場合は特に制限は設けません。ただし、dllはオリジナルのまま
    組み込んでください。
    このdllを使用したことによって発生した問題について保証は一切行いません。
　商用、シェアウェアなどに組み込むのは不可です。
------引用終了------
*/
#pragma once

#include "common.h"
#include <stdint.h>
#include <string>
#include <vector>
#include <memory>
#include "rgy_osdep.h"
#include "CaptionDef.h"
#include "StreamUtils.h"
#include "TranscodeSetting.h"

inline bool operator==(const CLUT_DAT_DLL &a, const CLUT_DAT_DLL &b) {
    return a.ucR == b.ucR && a.ucG == b.ucG && a.ucB == b.ucB && a.ucAlpha == b.ucAlpha;
}

struct CaptionFormat {

    enum STYLE {
        UNDERLINE = 1,
        SHADOW = 2,
        BOLD = 4,
        ITALIC = 8,

        BOTTOM = 0x10,
        RIGHT = 0x20,
        TOP = 0x40,
        LEFT = 0x80,
    };

    static int GetStyle(const CAPTION_CHAR_DATA_DLL &style);

    int pos;
    float charW, charH;
    float width, height;
    CLUT_DAT_DLL textColor;
    CLUT_DAT_DLL backColor;
    int style;
    int sizeMode;

    bool IsUnderline() const;
    bool IsShadow() const;
    bool IsBold() const;
    bool IsItalic() const;
    bool IsHighLightBottom() const;
    bool IsHighLightRight() const;
    bool IsHighLightTop() const;
    bool IsHighLightLeft() const;
    int GetFlushMode() const;
};

struct CaptionLine {
    std::wstring text;
    int planeW;
    int planeH;
    float posX;
    float posY;
    std::vector<CaptionFormat> formats;

    void Write(const File& file) const;

    static std::unique_ptr<CaptionLine> Read(const File& file);
};

struct CaptionItem {
    int64_t PTS;
    int langIndex;
    int waitTime;
    // nullだとクリア
    std::unique_ptr<CaptionLine> line;

    void Write(const File& file) const;

    static CaptionItem Read(const File& file);
};

// 半角置換可能文字リスト
// 記号はJISX0213 1面1区のうちグリフが用意されている可能性が十分高そうなものだけ
extern const LPCWSTR HALF_F_LIST;
extern const LPCWSTR HALF_T_LIST;
extern const LPCWSTR HALF_R_LIST;

BOOL CalcMD5FromDRCSPattern(std::vector<char>& hash, const DRCS_PATTERN_DLL *pPattern);

void SaveDRCSImage(const tstring& filename, const DRCS_PATTERN_DLL* pData);

int StrlenWoLoSurrogate(LPCWSTR str);

struct DRCSOutInfo {
    tstring filename;
    double elapsed;
};

class CaptionDLLParser : public AMTObject {
public:
    CaptionDLLParser(AMTContext& ctx);

    // 最初の１つだけ処理する
    CaptionItem ProcessCaption(int64_t PTS, int langIndex,
        const CAPTION_DATA_DLL* capList, int capCount, DRCS_PATTERN_DLL* pDrcsList, int drcsCount);

    virtual DRCSOutInfo getDRCSOutPath(int64_t PTS, const std::string& md5) = 0;

private:

    // 拡縮後の文字サイズを得る
    static void GetCharSize(float *pCharW, float *pCharH, float *pDirW, float *pDirH, const CAPTION_CHAR_DATA_DLL &charData);

    void AddText(CaptionLine& line, const std::wstring& text,
        float charW, float charH, float width, float height, const CAPTION_CHAR_DATA_DLL &style);

    // 字幕本文を1行だけ処理する
    std::unique_ptr<CaptionLine> ShowCaptionData(int64_t PTS,
        const CAPTION_DATA_DLL &caption, const DRCS_PATTERN_DLL *pDrcsList, DWORD drcsCount);
};



