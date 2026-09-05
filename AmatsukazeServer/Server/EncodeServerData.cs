using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.IO;
using System.Linq;
using System.Runtime.Serialization;
using System.Text;
using System.Globalization;
using System.Threading.Tasks;
using Amatsukaze.Lib;

namespace Amatsukaze.Server
{
    [DataContract]
    public class BitrateSetting : IExtensibleDataObject
    {
        [DataMember]
        public double A { get; set; }
        [DataMember]
        public double B { get; set; }
        [DataMember]
        public double H264 { get; set; }

        public ExtensionDataObject ExtensionData { get; set; }
    }

    public enum VideoStreamFormat
    {
        Unknown, MPEG2, H264, H265, AV1
    }

    public enum EncoderType
    {
        x264 = 0,
        x265,
        QSVEnc,
        NVEnc,
        VCEEnc,
        SVTAV1,
        x262
    }

    public enum AudioEncoderType
    {
        NeroAac,
        Qaac,
        Fdkaac,
        OpusEnc
    }

    public enum DecoderType
    {
        Default = 0,
        QSV,
        CUVID
    }

    public enum FormatType
    {
        MP4 = 0,
        MKV,
        M2TS,
        TS,
        TSREPLACE,
    }

    // 字幕モード
    public enum SubtitleMode
    {
        Arib = 0,
        WhisperFallback = 1,
        WhisperAlways = 2
    }

    // Whisper モデル選択
    public enum WhisperModelType
    {
        Auto = 0,
        Unspecified,
        Small,
        Medium,
        LargeV1,
        LargeV2,
        LargeV3,
        LargeV3Turbo,

        AutoCurrent = LargeV3Turbo,
    }

    public enum FinishAction
    {
        None, Suspend, Hibernate, Shutdown
    }

    // サーバに要求する情報の識別フラグ
    [Flags]
    public enum ServerRequest
    {
        Setting = 1,
        Queue = (1 << 1),
        Log = (1 << 2),
        CheckLog = (1 << 3),
        Console = (1 << 4),
        State = (1 << 5),
        FreeSpace = (1 << 6),
        ServiceSetting = (1 << 7),
    }

    // ServerRequestのデバッグ用拡張メソッド
    public static class ServerRequestExtensions
    {
        public static string ToDebugString(this ServerRequest req)
        {
            var flags = new List<string>();
            
            if ((req & ServerRequest.Setting) != 0) flags.Add("Setting");
            if ((req & ServerRequest.Queue) != 0) flags.Add("Queue");
            if ((req & ServerRequest.Log) != 0) flags.Add("Log");
            if ((req & ServerRequest.CheckLog) != 0) flags.Add("CheckLog");
            if ((req & ServerRequest.Console) != 0) flags.Add("Console");
            if ((req & ServerRequest.State) != 0) flags.Add("State");
            if ((req & ServerRequest.FreeSpace) != 0) flags.Add("FreeSpace");
            if ((req & ServerRequest.ServiceSetting) != 0) flags.Add("ServiceSetting");
            
            return flags.Count > 0 ? string.Join(" | ", flags) : "None";
        }
    }

    public struct ReqResource
    {
        public int CPU, HDD, GPU;

        public int Canonical()
        {
            return (CPU << 16) + (HDD << 8) + GPU;
        }

        public static ReqResource FromCanonical(int c)
        {
            return new ReqResource()
            {
                CPU = (c >> 16) & 0xFF,
                HDD = (c >> 8) & 0xFF,
                GPU = c & 0xFF
            };
        }
    }

    public class Resource
    {
        public ReqResource Req;
        public int GpuIndex;
        public int EncoderIndex;
    }

    public enum DeblockStrength
    {
        Strong, Medium, Weak, Weaker
    }

    public enum DeinterlaceAlgorithm
    {
        KFM, D3DVP, QTGMC, Yadif, AutoVfr
    }

    public enum D3DVPGPU
    {
        Auto, Intel, NVIDIA, Radeon
    }

    public enum FilterFPS
    {
        VFR, CFR24, CFR30, CFR60, SVP, VFR30
    }

    public enum FilterOption
    {
        None, Setting, Custom, QSVEncFilter, NVEncFilter, VCEEncFilter
    }

    public enum QTGMCPreset
    {
        Auto, Faster, Fast, Medium, Slow, Slower
    }

    public enum EncoderFilterDeinterlace
    {
        Afs, KFM, NNEDI, Yadif, Bwdif, Decomb, IVTC
    }

    public enum EncoderFilterAfsPreset
    {
        Default, Triple, Double, Anime, Cinema, MinAfterimg, Fps24, Fps30
    }

    public enum EncoderFilterKfmMode
    {
        VFR, Fps60, Fps24
    }

    public enum EncoderFilterDeintMode
    {
        Normal, Bob
    }

    public enum EncoderFilterDenoise
    {
        PMD, KNN, NLMeans, HQDN3D, DenoiseDct, Smooth, FFT3D, Convolution3D, MSmooth
    }

    public enum EncoderFilterEdge
    {
        EdgeLevel, Unsharp, WarpSharp, MSharpen
    }

    public enum EncoderFilterOutputDepth
    {
        Bit8, Bit10
    }

    [DataContract]
    public class FilterSetting : IExtensibleDataObject
    {
        [DataMember]
        public bool EnableCUDA;
        [DataMember]
        public bool EnableDeblock;
        [DataMember]
        public int DeblockQuality;
        [DataMember]
        public DeblockStrength DeblockStrength;
        [DataMember]
        public bool DeblockSharpen;
        [DataMember]
        public bool EnableDeinterlace;
        [DataMember]
        public DeinterlaceAlgorithm DeinterlaceAlgorithm;
        [DataMember]
        public D3DVPGPU D3dvpGpu;
        [DataMember]
        public QTGMCPreset QtgmcPreset;
        [DataMember]
        public bool KfmEnableNr;
        [DataMember]
        public bool KfmEnableUcf;
        [DataMember]
        public bool KfmVfr120fps;
        [DataMember]
        public FilterFPS KfmFps;
        [DataMember]
        public FilterFPS YadifFps;
        [DataMember]
        public int AutoVfrParallel;
        [DataMember]
        public bool AutoVfrFast;
        [DataMember]
        public bool AutoVfr30F;
        [DataMember]
        public bool AutoVfr60F;
        [DataMember]
        public bool AutoVfr24A;
        [DataMember]
        public bool AutoVfr30A;
        [DataMember]
        public bool AutoVfrCrop;
        [DataMember]
        public int AutoVfrSkip;
        [DataMember]
        public int AutoVfrRef;
        [DataMember]
        public bool EnableResize;
        [DataMember]
        public int ResizeWidth;
        [DataMember]
        public int ResizeHeight;
        [DataMember]
        public bool EnableTemporalNR;
        [DataMember]
        public bool EnableDeband;
        [DataMember]
        public bool EnableEdgeLevel;
        [DataMember]
        public int SvtAv1BitDepth;

        public ExtensionDataObject ExtensionData { get; set; }
    }

    [DataContract]
    public class EncoderFilterSetting : IExtensibleDataObject
    {
        [DataMember]
        public bool EnableDeinterlace;
        [DataMember]
        public EncoderFilterDeinterlace DeinterlaceAlgorithm;
        [DataMember]
        public EncoderFilterAfsPreset AfsPreset;
        [DataMember]
        public EncoderFilterKfmMode KfmMode;
        [DataMember]
        public EncoderFilterDeintMode NnediMode;
        [DataMember]
        public EncoderFilterDeintMode YadifMode;
        [DataMember]
        public EncoderFilterDeintMode BwdifMode;
        [DataMember]
        public bool EnableDenoise;
        [DataMember]
        public EncoderFilterDenoise DenoiseAlgorithm;
        [DataMember]
        public double KnnStrength;
        [DataMember]
        public double NlmeansSigma;
        [DataMember]
        public double PmdStrength;
        [DataMember]
        public double DenoiseDctSigma;
        [DataMember]
        public double Fft3dSigma;
        [DataMember]
        public double Convolution3dThresh;
        [DataMember]
        public int SmoothQP;
        [DataMember]
        public int MsmoothStrength;
        [DataMember]
        public bool EnableResize;
        [DataMember]
        public int ResizeWidth;
        [DataMember]
        public int ResizeHeight;
        [DataMember]
        public bool EnableEdgeEnhance;
        [DataMember]
        public EncoderFilterEdge EdgeAlgorithm;
        [DataMember]
        public double UnsharpWeight;
        [DataMember]
        public double EdgeLevelStrength;
        [DataMember]
        public double WarpSharpDepth;
        [DataMember]
        public double MSharpenStrength;
        [DataMember]
        public bool EnableDeband;
        [DataMember]
        public bool EnableOutputDepth;
        [DataMember]
        public EncoderFilterOutputDepth OutputDepth;

        public ExtensionDataObject ExtensionData { get; set; }
    }

    // プロファイル設定データ
    public class ProfileSetting : IExtensibleDataObject
    {
        [DataMember]
        public string Name { get; set; }
        [DataMember]
        public DateTime LastUpdate { get; set; }
        [DataMember]
        public EncoderType EncoderType { get; set; }
        [DataMember]
        public string X264Option { get; set; }
        [DataMember]
        public string X265Option { get; set; }
        [DataMember]
        public string QSVEncOption { get; set; }
        [DataMember]
        public string NVEncOption { get; set; }
        [DataMember]
        public string VCEEncOption { get; set; }
        [DataMember]
        public string QSVEncFilterOption { get; set; }
        [DataMember]
        public string NVEncFilterOption { get; set; }
        [DataMember]
        public string VCEEncFilterOption { get; set; }
        [DataMember]
        public string SVTAV1Option { get; set; }
        [DataMember]
        public string X262Option { get; set; }

        [DataMember]
        public bool ForceSAR { get; set; }
        [DataMember]
        public int ForceSARWidth { get; set; }
        [DataMember]
        public int ForceSARHeight { get; set; }

        [DataMember]
        public DecoderType Mpeg2Decoder { get; set; }
        [DataMember]
        public DecoderType H264Deocder { get; set; }
        [DataMember]
        public DecoderType HEVCDecoder { get; set; }
        [DataMember]
        public FormatType OutputFormat { get; set; }

        [DataMember]
        public bool UseMKVWhenSubExists { get; set; }

        [DataMember]
        public bool TsreplaceRemoveTypeD { get; set; }

        [DataMember]
        public bool TsreplaceMuxTsTempFile { get; set; }

        [DataMember]
        public bool TSReplaceVideo { get; set; }

        [DataMember]
        public string FilterPath { get; set; }
        [DataMember]
        public string PostFilterPath { get; set; }

        [DataMember]
        public bool TwoPass { get; set; }
        [DataMember]
        public bool SplitSub { get; set; }
        [DataMember]
        public int MinOutputDuration { get; set; }
        [DataMember]
        public int OutputMask { get; set; }
        [DataMember]
        public bool Mpeg2Partial { get; set; }
        [DataMember]
        public bool AutoBuffer { get; set; }
        [DataMember]
        public BitrateSetting Bitrate { get; set; }
        [DataMember]
        public double BitrateCM { get; set; }
        [DataMember]
        public double CMQualityOffset { get; set; }

        [DataMember]
        public string JLSCommandFile { get; set; }
        [DataMember]
        public string JLSOption { get; set; }
        [DataMember]
        public string ChapterExeOption { get; set; }
        [DataMember]
        public bool EnableJLSOption { get; set; }
        [DataMember]
        public bool DisableChapter { get; set; }
        [DataMember]
        public bool OutputChapter { get; set; }
        [DataMember]
        public bool DisableSubs { get; set; }
        [DataMember]
        public bool EnableWebVTT { get; set; }

        // 字幕生成モード
        [DataMember]
        public SubtitleMode SubMode { get; set; }
        // Whisperのモデル
        [DataMember]
        public WhisperModelType WhisperModel { get; set; }
        // Whisperの追加オプション
        [DataMember]
        public string WhisperOption { get; set; }
        // Whisperを映像エンコードと並列実行するかどうか
        [DataMember]
        public bool WhisperParallel { get; set; }
        [DataMember]
        public bool IgnoreNoDrcsMap { get; set; }
        [DataMember]
        public bool LooseLogoDetection { get; set; }
        [DataMember]
        public bool IgnoreNoLogo { get; set; }
        [DataMember]
        public bool NoLogoInCM { get; set; }
        [DataMember]
        public bool NoDelogo { get; set; }
        [DataMember]
        public bool ParallelLogoAnalysis { get; set; }
        [DataMember]
        public bool SystemAviSynthPlugin { get; set; }
        [DataMember]
        public bool EnableNicoJK { get; set; }
        [DataMember]
        public bool IgnoreNicoJKError { get; set; }
        [DataMember]
        public bool NicoJK18 { get; set; }
        [DataMember]
        public bool NicoJKLog { get; set; }
        [DataMember]
        public bool[] NicoJKFormats { get; set; }
        [DataMember]
        public bool DisableMoveInputFile { get; set; }
        [DataMember]
        public bool MoveEDCBFiles { get; set; }
        [DataMember]
        public bool NoRemoveTmp { get; set; }
        [DataMember]
        public bool DisableLogFile { get; set; }
        [DataMember]
        public bool SaveProfileText { get; set; }

        [DataMember]
        public bool EnableMaxFadeLength { get; set; }
        [DataMember]
        public int MaxFadeLength { get; set; }

        [DataMember]
        public bool EnableRename { get; set; }
        [DataMember]
        public string RenameFormat { get; set; }
        [DataMember]
        public bool EnableGunreFolder { get; set; }
        [DataMember]
        public ReqResource[] ReqResources { get; set; }

        [DataMember]
        public bool EnablePmtCut { get; set; }
        [DataMember]
        public double PmtCutHeadRate { get; set; }
        [DataMember]
        public double PmtCutTailRate { get; set; }

        [DataMember]
        public bool IgnoreEncodeAffinity { get; set; }

        [DataMember]
        public string PreBatchFile { get; set; }
        [DataMember]
        public string PreEncodeBatchFile { get; set; }
        [DataMember]
        public string PostBatchFile { get; set; }

        [DataMember]
        public FilterOption FilterOption { get; set; }
        [DataMember]
        public FilterSetting FilterSetting { get; set; }
        [DataMember]
        public EncoderFilterSetting EncoderFilterSetting { get; set; }
        [DataMember]
        public int NumEncodeBufferFrames { get; set; }
        [DataMember]
        public int EncoderParallel { get; set; }
        /// <summary>追加オプションをコンテナに記録する（mp4/mkv出力時のみ有効）</summary>
        [DataMember]
        public bool MuxerAddEncoderCmd { get; set; }
        /// <summary>SAR比をエンコーダではなくコンテナのみに記録する（mp4/mkv出力時のみ有効）</summary>
        [DataMember]
        public bool SARInContainerOnly { get; set; }

        [DataMember]
        public string AdditionalEraseLogo { get; set; }

        [DataMember]
        public bool EnableAudioEncode { get; set; }
        [DataMember]
        public AudioEncoderType AudioEncoderType { get; set; }
        [DataMember]
        public string NeroAacOption { get; set; }
        [DataMember]
        public string QaacOption { get; set; }
        [DataMember]
        public string FdkaacOption { get; set; }
        [DataMember]
        public string OpusEncOption { get; set; }
        [DataMember]
        public bool EnableAudioBitrate { get; set; }
        [DataMember]
        public int AudioBitrateInKbps { get; set; }

        public ExtensionDataObject ExtensionData { get; set; }

        public int NicoJKFormatMask
        {
            get
            {
                int mask = 0;
                for (int i = NicoJKFormats.Length - 1; i >= 0; --i)
                {
                    mask <<= 1;
                    mask |= NicoJKFormats[i] ? 1 : 0;
                }
                return mask;
            }
        }
    }

    // 文字列リソース（列挙体に対応する文字列配列）
    public static class ProfileSettingExtensions
    {
        public static string[] EncoderList { get; } = new string[] { "x264", "x265", "QSVEnc", "NVEnc", "VCEEnc", "SVT-AV1", "x262" };
        public static string[] Mpeg2DecoderList { get; } = new string[] { "デフォルト", "QSV", "CUVID" };
        public static string[] H264DecoderList { get; } = new string[] { "デフォルト", "QSV", "CUVID" };
        public static string[] HEVCDecoderList { get; } = new string[] { "デフォルト", "QSV", "CUVID" };
        public static string[] FormatList { get; } = new string[] { "MP4", "MKV", "M2TS", "TS", "TS (replace)" };
        public static string[] SubtitleModeList { get; } = new string[] { "標準", "tsに字幕がない場合Whisperで生成", "常にWhisperで生成" };
        public static string[] WhisperModelList { get; } = new string[] { "自動", "未指定", "small", "medium", "large-v1", "large-v2", "large-v3", "large-v3-turbo" };
        public static string[] AudioEncoderList { get; } = new string[] { "NeroAAC", "qaac", "fdkaac", "opusenc" };
        public static string[] DeinterlaceAlgorithmNames { get; } = new string[] { "KFM", "D3DVP", "QTGMC", "Yadif", "AutoVfr" };
        public static string[] D3DVPGPUList { get; } = new string[] { "自動", "Intel", "NVIDIA", "Radeon" };
        public static string[] QTGMCPresetList { get; } = new string[] { "自動", "Faster", "Fast", "Medium", "Slow", "Slower" };
        // FilterFPS列挙体の順序: VFR, CFR24, CFR30, CFR60, SVP, VFR30
        public static string[] FilterFPSList { get; } = new string[] { "VFR", "24fps", "30fps", "60fps", "SVPによる60fps化", "VFR(30fps上限)" };
        // KFMはCFR30を使用しない
        public static FilterFPS[] FilterFPSListKfm { get; } = new FilterFPS[] { FilterFPS.VFR, FilterFPS.VFR30, FilterFPS.CFR24, FilterFPS.CFR60, FilterFPS.SVP };
        // YadifはCFR系のみ
        public static FilterFPS[] FilterFPSListYadif { get; } = new FilterFPS[] { FilterFPS.CFR24, FilterFPS.CFR30, FilterFPS.CFR60 };
        public static string[] FilterFPSEnumNamesKfm { get; } = FilterFPSListKfm.Select(fps => fps.ToString()).ToArray();
        public static string[] FilterFPSEnumNamesYadif { get; } = FilterFPSListYadif.Select(fps => fps.ToString()).ToArray();
        public static string[] FilterFPSNameListKfm { get; } = FilterFPSListKfm.Select(GetFilterFPSString).ToArray();
        public static string[] FilterFPSNameListYadif { get; } = FilterFPSListYadif.Select(GetYadifFPSString).ToArray();
        public static string[] DeblockStrengthList { get; } = new string[] { "強", "中", "弱", "低ビットレート用弱" };
        public static string[] DeblockQualityList { get; } = new string[] { "高(4)", "中(3)", "低(2)" };
        public static int[] DeblockQualityListData { get; } = new int[] { 4, 3, 2 };
        public static string[] VFRFpsList { get; } = new string[] { "60fps", "120fps" };

        // OutputMaskのマッピング（マスク値から名前へ）
        public static string GetOutputMaskName(int mask)
        {
            switch (mask)
            {
                case 1: return "通常";
                case 2: return "CMをカット";
                case 4: return "CMのみ";
                case 6: return "本編とCMを分離";
                case 8: return "前後のCMのみカット";
                default: return mask.ToString();
            }
        }

        // FilterFPSから文字列への変換（KFM用）
        public static string GetFilterFPSString(FilterFPS fps)
        {
            switch (fps)
            {
                case FilterFPS.VFR: return "VFR";
                case FilterFPS.VFR30: return "VFR(30fps上限)";
                case FilterFPS.CFR24: return "24fps";
                case FilterFPS.CFR60: return "60fps";
                case FilterFPS.SVP: return "SVPによる60fps化";
                default: return fps.ToString();
            }
        }

        // FilterFPSから文字列への変換（Yadif用）
        public static string GetYadifFPSString(FilterFPS fps)
        {
            switch (fps)
            {
                case FilterFPS.CFR24: return "24fps";
                case FilterFPS.CFR30: return "30fps";
                case FilterFPS.CFR60: return "60fps";
                default: return fps.ToString();
            }
        }

        // DeblockQualityから文字列への変換
        public static string GetDeblockQualityString(int quality)
        {
            int idx = Array.IndexOf(DeblockQualityListData, quality);
            if (idx >= 0 && idx < DeblockQualityList.Length)
            {
                return DeblockQualityList[idx];
            }
            return quality.ToString();
        }

        // エンコーダオプションを取得
        public static string GetEncoderOption(ProfileSetting profile)
        {
            switch (profile.EncoderType)
            {
                case EncoderType.x264: return profile.X264Option;
                case EncoderType.x265: return profile.X265Option;
                case EncoderType.QSVEnc: return profile.QSVEncOption;
                case EncoderType.NVEnc: return profile.NVEncOption;
                case EncoderType.VCEEnc: return profile.VCEEncOption;
                case EncoderType.SVTAV1: return profile.SVTAV1Option;
                case EncoderType.x262: return profile.X262Option;
                default: return null;
            }
        }

        // エンコーダフィルタが有効ならそのEncoderType、無効ならnull
        public static EncoderType? GetFilterEncoderType(FilterOption opt)
        {
            switch (opt)
            {
                case FilterOption.QSVEncFilter: return EncoderType.QSVEnc;
                case FilterOption.NVEncFilter: return EncoderType.NVEnc;
                case FilterOption.VCEEncFilter: return EncoderType.VCEEnc;
                default: return null;
            }
        }

        // エンコーダのオプションに埋め込む数値の文字列化
        // double既定の書式では小さい値が指数表記 (5E-05) になってしまうため、それを避ける
        private static string FormatEncoderFilterValue(double value)
        {
            return value.ToString("0.##########", CultureInfo.InvariantCulture);
        }

        public static string BuildEncoderFilterOptions(EncoderFilterSetting s, int outputDepthOverride = 0)
        {
            if (s == null)
            {
                return "";
            }

            var options = new List<string>();
            if (s.EnableDeinterlace)
            {
                switch (s.DeinterlaceAlgorithm)
                {
                    case EncoderFilterDeinterlace.Afs:
                        string afsPreset;
                        switch (s.AfsPreset)
                        {
                            case EncoderFilterAfsPreset.Triple: afsPreset = "triple"; break;
                            case EncoderFilterAfsPreset.Double: afsPreset = "double"; break;
                            case EncoderFilterAfsPreset.Anime: afsPreset = "anime"; break;
                            case EncoderFilterAfsPreset.Cinema: afsPreset = "cinema"; break;
                            case EncoderFilterAfsPreset.MinAfterimg: afsPreset = "min_afterimg"; break;
                            case EncoderFilterAfsPreset.Fps24: afsPreset = "24fps"; break;
                            case EncoderFilterAfsPreset.Fps30: afsPreset = "30fps"; break;
                            default: afsPreset = "default"; break;
                        }
                        options.Add("--vpp-afs preset=" + afsPreset);
                        break;
                    case EncoderFilterDeinterlace.KFM:
                        string kfmMode;
                        switch (s.KfmMode)
                        {
                            case EncoderFilterKfmMode.Fps60: kfmMode = "60"; break;
                            case EncoderFilterKfmMode.Fps24: kfmMode = "24"; break;
                            default: kfmMode = "vfr"; break;
                        }
                        options.Add("--vpp-kfm mode=" + kfmMode);
                        break;
                    case EncoderFilterDeinterlace.NNEDI:
                        options.Add("--vpp-nnedi field=" +
                            (s.NnediMode == EncoderFilterDeintMode.Bob ? "bob" : "auto"));
                        break;
                    case EncoderFilterDeinterlace.Yadif:
                        options.Add("--vpp-yadif mode=" +
                            (s.YadifMode == EncoderFilterDeintMode.Bob ? "bob" : "auto"));
                        break;
                    case EncoderFilterDeinterlace.Bwdif:
                        options.Add("--vpp-bwdif mode=" +
                            (s.BwdifMode == EncoderFilterDeintMode.Bob ? "bob" : "frame"));
                        break;
                    case EncoderFilterDeinterlace.Decomb:
                        options.Add("--vpp-decomb");
                        break;
                    case EncoderFilterDeinterlace.IVTC:
                        options.Add("--vpp-ivtc");
                        break;
                }
            }

            if (s.EnableDenoise)
            {
                switch (s.DenoiseAlgorithm)
                {
                    case EncoderFilterDenoise.KNN:
                        options.Add("--vpp-knn strength=" + FormatEncoderFilterValue(s.KnnStrength));
                        break;
                    case EncoderFilterDenoise.NLMeans:
                        options.Add("--vpp-nlmeans sigma=" + FormatEncoderFilterValue(s.NlmeansSigma));
                        break;
                    case EncoderFilterDenoise.PMD:
                        options.Add("--vpp-pmd strength=" + FormatEncoderFilterValue(s.PmdStrength));
                        break;
                    case EncoderFilterDenoise.HQDN3D:
                        options.Add("--vpp-hqdn3d");
                        break;
                    case EncoderFilterDenoise.DenoiseDct:
                        options.Add("--vpp-denoise-dct sigma=" + FormatEncoderFilterValue(s.DenoiseDctSigma));
                        break;
                    case EncoderFilterDenoise.Smooth:
                        options.Add("--vpp-smooth qp=" + s.SmoothQP.ToString(CultureInfo.InvariantCulture));
                        break;
                    case EncoderFilterDenoise.FFT3D:
                        options.Add("--vpp-fft3d sigma=" + FormatEncoderFilterValue(s.Fft3dSigma));
                        break;
                    case EncoderFilterDenoise.Convolution3D:
                        string convolutionThreshold = FormatEncoderFilterValue(s.Convolution3dThresh);
                        options.Add("--vpp-convolution3d ythresh=" + convolutionThreshold +
                            ",cthresh=" + convolutionThreshold);
                        break;
                    case EncoderFilterDenoise.MSmooth:
                        options.Add("--vpp-msmooth strength=" + s.MsmoothStrength.ToString(CultureInfo.InvariantCulture));
                        break;
                }
            }

            if (s.EnableResize)
            {
                options.Add("--output-res " + s.ResizeWidth.ToString(CultureInfo.InvariantCulture) + "x" +
                    s.ResizeHeight.ToString(CultureInfo.InvariantCulture));
            }

            if (s.EnableEdgeEnhance)
            {
                switch (s.EdgeAlgorithm)
                {
                    case EncoderFilterEdge.Unsharp:
                        options.Add("--vpp-unsharp weight=" + FormatEncoderFilterValue(s.UnsharpWeight));
                        break;
                    case EncoderFilterEdge.EdgeLevel:
                        options.Add("--vpp-edgelevel strength=" + FormatEncoderFilterValue(s.EdgeLevelStrength));
                        break;
                    case EncoderFilterEdge.WarpSharp:
                        options.Add("--vpp-warpsharp depth=" + FormatEncoderFilterValue(s.WarpSharpDepth));
                        break;
                    case EncoderFilterEdge.MSharpen:
                        options.Add("--vpp-msharpen strength=" + FormatEncoderFilterValue(s.MSharpenStrength));
                        break;
                }
            }

            if (s.EnableDeband)
            {
                options.Add("--vpp-deband");
            }

            if (outputDepthOverride == 8 || outputDepthOverride == 10)
            {
                // エンコーダ側の要求ビット深度が優先される場合（svt-av1の入力ビット深度指定）
                options.Add("--output-depth " + outputDepthOverride.ToString(CultureInfo.InvariantCulture));
            }
            else if (s.EnableOutputDepth)
            {
                options.Add("--output-depth " +
                    (s.OutputDepth == EncoderFilterOutputDepth.Bit10 ? "10" : "8"));
            }
            return string.Join(" ", options);
        }

        // コマンドラインに埋め込むオプション文字列を正規化する
        // 入力欄は複数行入力を許可しているため、改行が混入するとコマンドラインが壊れて
        // 改行以降のオプションがエンコーダに渡らない。改行は空白に置き換えておく
        public static string NormalizeCommandLineOption(string option)
        {
            if (string.IsNullOrEmpty(option))
            {
                return option;
            }
            return option.Replace('\r', ' ').Replace('\n', ' ').Trim();
        }

        // FilterOptionに対応するエンコーダフィルタのオプション文字列（無効時はnull）
        public static string GetFilterEncoderOption(ProfileSetting profile)
        {
            string customOption;
            switch (profile.FilterOption)
            {
                case FilterOption.QSVEncFilter: customOption = profile.QSVEncFilterOption; break;
                case FilterOption.NVEncFilter: customOption = profile.NVEncFilterOption; break;
                case FilterOption.VCEEncFilter: customOption = profile.VCEEncFilterOption; break;
                default: return null;
            }

            return string.Join(" ", new[]
            {
                BuildEncoderFilterOptions(profile.EncoderFilterSetting, GetEncoderFilterOutputDepthOverride(profile)),
                customOption
            }.Where(option => !string.IsNullOrWhiteSpace(option)).Select(NormalizeCommandLineOption));
        }

        // エンコーダフィルタの出力ビット深度を本エンコーダ側の要求で上書きする場合の値（上書きしない場合は0）
        // svt-av1は入力ビット深度を明示指定する必要があるため、例外的にエンコーダフィルタの出力ビット深度設定より優先する
        public static int GetEncoderFilterOutputDepthOverride(ProfileSetting profile)
        {
            if (profile == null || profile.EncoderType != EncoderType.SVTAV1)
            {
                return 0;
            }
            var bitDepth = profile.FilterSetting?.SvtAv1BitDepth ?? 0;
            return (bitDepth == 8 || bitDepth == 10) ? bitDepth : 0;
        }

        // リソース文字列を生成
        public static string GetResourceString(ProfileSetting profile)
        {
            var sb = new StringBuilder();
            if (profile.ReqResources != null)
            {
                foreach (var res in profile.ReqResources)
                {
                    sb.Append(res.CPU).Append(":")
                        .Append(res.HDD).Append(":")
                        .Append(res.GPU).AppendLine();
                }
            }
            return sb.ToString();
        }

        // ToLongString()拡張メソッド
        public static string ToLongString(this ProfileSetting profile)
        {
            var sb = new StringBuilder();

            // KeyValueヘルパー関数
            Action<string, string> keyValue = (key, value) =>
            {
                sb.Append(key).Append(": ").Append(value).AppendLine();
            };

            Action<string, bool> keyValueBool = (key, value) =>
            {
                sb.Append(key).Append(": ").Append(value ? "Yes" : "No").AppendLine();
            };

            Action<string, string> keyTable = (key, value) =>
            {
                sb.Append(key).Append(": ").AppendLine();
                foreach (var line in value.Split(new[] { "\r\n", "\r", "\n" }, StringSplitOptions.None)
                    .Where(s => !string.IsNullOrWhiteSpace(s))
                    .Select(s => "\t" + s))
                {
                    sb.Append(line).AppendLine();
                }
            };

            keyValue("プロファイル名", profile.Name);
            keyValue("更新日時", profile.LastUpdate == DateTime.MinValue
                ? "不明"
                : profile.LastUpdate.ToString("yyyy年MM月dd日 hh:mm:ss", CultureInfo.InvariantCulture));
            keyValue("エンコーダ", EncoderList[(int)profile.EncoderType]);
            keyValue("エンコーダ追加オプション", GetEncoderOption(profile) ?? "");
            if (profile.EncoderType == EncoderType.SVTAV1 && profile.ForceSAR)
            {
                keyValue("SAR比上書き", string.Format("{0}:{1}", profile.ForceSARWidth, profile.ForceSARHeight));
            }
            keyValue("エンコード分割並列数", profile.EncoderParallel.ToString());
            keyValueBool("追加オプションをコンテナに記録する", profile.MuxerAddEncoderCmd);
            keyValueBool("SAR比をコンテナにのみ記録する", profile.SARInContainerOnly);
            keyValue("JoinLogoScpコマンドファイル", profile.JLSCommandFile ?? "チャンネル設定に従う");
            keyValue("JoinLogoScpオプション", profile.JLSOption ?? "チャンネル設定に従う");
            keyValue("chapter_exeオプション", profile.ChapterExeOption ?? "");

            keyValue("MPEG2デコーダ", Mpeg2DecoderList[(int)profile.Mpeg2Decoder]);
            keyValue("H264デコーダ", H264DecoderList[(int)profile.H264Deocder]);
            keyValue("HEVCデコーダ", HEVCDecoderList[(int)profile.HEVCDecoder]);
            keyValue("出力フォーマット", FormatList[(int)profile.OutputFormat]);
            keyValueBool("出力フォーマット-字幕がある時MKV出力する", profile.UseMKVWhenSubExists);
            keyValue("出力選択", GetOutputMaskName(profile.OutputMask));
            keyValueBool("カット境界再エンコード", profile.Mpeg2Partial);
            keyValueBool("SCRenameによるリネームを行う", profile.EnableRename);
            keyValue("SCRename書式", profile.RenameFormat ?? "");
            keyValueBool("ジャンルごとにフォルダ分け", profile.EnableGunreFolder);

            keyValue("実行前バッチ", profile.PreBatchFile ?? "なし");
            keyValue("エンコ前バッチ", profile.PreEncodeBatchFile ?? "なし");
            keyValue("実行後バッチ", profile.PostBatchFile ?? "なし");

            if (profile.FilterOption == FilterOption.None)
            {
                keyValue("フィルタ", "なし");
            }
            else if (profile.FilterOption == FilterOption.Setting)
            {
                keyValueBool("フィルタ-CUDAで処理", profile.FilterSetting.EnableCUDA);
                keyValueBool("フィルタ-インターレース解除", profile.FilterSetting.EnableDeinterlace);
                if (profile.FilterSetting.EnableDeinterlace)
                {
                    keyValue("フィルタ-インターレース解除方法", DeinterlaceAlgorithmNames[(int)profile.FilterSetting.DeinterlaceAlgorithm]);
                    switch (profile.FilterSetting.DeinterlaceAlgorithm)
                    {
                        case DeinterlaceAlgorithm.KFM:
                            keyValueBool("フィルタ-SMDegrainによるNR", profile.FilterSetting.KfmEnableNr);
                            keyValueBool("フィルタ-DecombUCF", profile.FilterSetting.KfmEnableUcf);
                            keyValue("フィルタ-出力fps", GetFilterFPSString(profile.FilterSetting.KfmFps));
                            keyValue("フィルタ-VFRフレームタイミング", VFRFpsList[profile.FilterSetting.KfmVfr120fps ? 1 : 0]);
                            break;
                        case DeinterlaceAlgorithm.D3DVP:
                            keyValue("フィルタ-使用GPU", D3DVPGPUList[(int)profile.FilterSetting.D3dvpGpu]);
                            break;
                        case DeinterlaceAlgorithm.QTGMC:
                            keyValue("フィルタ-QTGMCプリセット", QTGMCPresetList[(int)profile.FilterSetting.QtgmcPreset]);
                            break;
                        case DeinterlaceAlgorithm.Yadif:
                            keyValue("フィルタ-出力fps", GetYadifFPSString(profile.FilterSetting.YadifFps));
                            break;
                        case DeinterlaceAlgorithm.AutoVfr:
                            keyValueBool("フィルタ-30fpsを使用する", profile.FilterSetting.AutoVfr30F);
                            keyValueBool("フィルタ-60fpsを使用する", profile.FilterSetting.AutoVfr60F);
                            keyValue("フィルタ-SKIP", profile.FilterSetting.AutoVfrSkip.ToString());
                            keyValue("フィルタ-REF", profile.FilterSetting.AutoVfrRef.ToString());
                            keyValueBool("フィルタ-CROP", profile.FilterSetting.AutoVfrCrop);
                            break;
                    }
                }
                keyValueBool("フィルタ-デブロッキング", profile.FilterSetting.EnableDeblock);
                if (profile.FilterSetting.EnableDeblock)
                {
                    keyValue("フィルタ-デブロッキング強度", DeblockStrengthList[(int)profile.FilterSetting.DeblockStrength]);
                    keyValue("フィルタ-デブロッキング品質", GetDeblockQualityString(profile.FilterSetting.DeblockQuality));
                    keyValueBool("フィルタ-デブロッキングシャープ化", profile.FilterSetting.DeblockSharpen);
                }
                keyValueBool("フィルタ-リサイズ", profile.FilterSetting.EnableResize);
                if (profile.FilterSetting.EnableResize)
                {
                    keyValue("フィルタ-リサイズ-縦", profile.FilterSetting.ResizeWidth.ToString());
                    keyValue("フィルタ-リサイズ-横", profile.FilterSetting.ResizeHeight.ToString());
                }
                keyValueBool("フィルタ-時間軸安定化", profile.FilterSetting.EnableTemporalNR);
                keyValueBool("フィルタ-バンディング低減", profile.FilterSetting.EnableDeband);
                keyValueBool("フィルタ-エッジ強調", profile.FilterSetting.EnableEdgeLevel);
            }
            else if (GetFilterEncoderType(profile.FilterOption) is EncoderType filterEncoderType)
            {
                keyValue("エンコーダフィルタ", filterEncoderType.ToString());
                keyValue("エンコーダフィルタ-オプション", GetFilterEncoderOption(profile) ?? "");
            }
            else
            {
                keyValue("メインフィルタ", profile.FilterPath ?? "");
                keyValue("ポストフィルタ", profile.PostFilterPath ?? "");
            }

            keyValueBool("2パス", profile.TwoPass);
            keyValue("CMビットレート倍率", profile.BitrateCM.ToString());
            keyValue("CM品質オフセット", profile.CMQualityOffset.ToString());
            keyValueBool("自動ビットレート指定", profile.AutoBuffer);
            keyValue("自動ビットレート係数", string.Format("{0}:{1}:{2}", profile.Bitrate.A, profile.Bitrate.B, profile.Bitrate.H264));
            keyValueBool("ニコニコ実況コメントを有効にする", profile.EnableNicoJK);
            keyValueBool("ニコニコ実況コメントのエラーを無視する", profile.IgnoreNicoJKError);
            keyValueBool("NicoJKログから優先的にコメントを取得する", profile.NicoJKLog);
            keyValueBool("NicoJK18サーバからコメントを取得する", profile.NicoJK18);
            keyValue("コメント出力フォーマット", profile.NicoJKFormatMask.ToString());
            keyValueBool("入力ファイルの移動を無効にする", profile.DisableMoveInputFile);
            keyValueBool("関連ファイル(*.err,*.program.txt)も処理", profile.MoveEDCBFiles);
            keyValueBool("チャプターを無効にする", profile.DisableChapter);
            keyValueBool("チャプターを出力する", profile.OutputChapter);
            keyValueBool("字幕を無効にする", profile.DisableSubs);
            if (!profile.DisableSubs)
            {
                keyValueBool("WebVTTを生成する", profile.EnableWebVTT);
                keyValue("字幕モード", SubtitleModeList[(int)profile.SubMode]);
                if (profile.SubMode != SubtitleMode.Arib)
                {
                    var wm = profile.WhisperModel;
                    if (wm == WhisperModelType.Auto)
                    {
                        wm = WhisperModelType.AutoCurrent;
                    }
                    keyValue("whisper-model", (wm == WhisperModelType.Unspecified) ? "なし" : WhisperModelList[(int)wm]);
                    keyValue("whisper-option", profile.WhisperOption ?? "なし");
                    keyValueBool("Whisper並列実行", profile.WhisperParallel);
                }
            }
            keyValueBool("マッピングにないDRCS外字は無視する", profile.IgnoreNoDrcsMap);
            keyValueBool("ロゴ検出判定しきい値を低くする", profile.LooseLogoDetection);
            keyValueBool("ロゴ検出に失敗しても処理を続行する", profile.IgnoreNoLogo);
            keyValueBool("CM解析でロゴを使用しない", profile.NoLogoInCM);
            keyValueBool("ロゴ消ししない", profile.NoDelogo);
            keyValueBool("並列ロゴ解析", profile.ParallelLogoAnalysis);
            keyValueBool("メインフォーマット以外は結合しない", profile.SplitSub);
            keyValue("出力する最短区間（秒）", profile.MinOutputDuration > 0
                ? profile.MinOutputDuration.ToString()
                : "既定値 (5)");
            keyValueBool("システムにインストールされているAviSynthプラグインを有効にする", profile.SystemAviSynthPlugin);
            keyValueBool("ログファイルを出力先に生成しない", profile.DisableLogFile);
            keyValueBool("一時ファイルを削除せずに残す", profile.NoRemoveTmp);
            keyValue("PMT更新によるCM認識", profile.EnablePmtCut
                ? string.Format("{0}:{1}", profile.PmtCutHeadRate, profile.PmtCutTailRate) : "なし");
            keyValue("ロゴ最長フェードフレーム数指定", profile.EnableMaxFadeLength ? profile.MaxFadeLength.ToString() : "なし");
            keyValueBool("tsreplaceでTypeDを削除する", profile.TsreplaceRemoveTypeD);
            keyValueBool("tsreplaceでts一時ファイルを作成しmuxを高速化", profile.TsreplaceMuxTsTempFile);
            keyValueBool("tsreplaceでビデオを置換する", profile.TSReplaceVideo);
            keyValueBool("JoinLogoScpオプションを有効にする", profile.EnableJLSOption);
            keyValueBool("エンコードアフィニティを無視する", profile.IgnoreEncodeAffinity);
            keyValue("エンコードバッファフレーム数", profile.NumEncodeBufferFrames.ToString());
            keyValue("追加ロゴ消去", profile.AdditionalEraseLogo ?? "なし");
            keyValueBool("音声エンコードを有効にする", profile.EnableAudioEncode);
            if (profile.EnableAudioEncode)
            {
                keyValue("音声エンコーダ", AudioEncoderList[(int)profile.AudioEncoderType]);
                keyValue("NeroAACオプション", profile.NeroAacOption ?? "なし");
                keyValue("qaacオプション", profile.QaacOption ?? "なし");
                keyValue("fdkaacオプション", profile.FdkaacOption ?? "なし");
                keyValue("opusencオプション", profile.OpusEncOption ?? "なし");
                keyValueBool("音声ビットレートを有効にする", profile.EnableAudioBitrate);
                if (profile.EnableAudioBitrate)
                {
                    keyValue("音声ビットレート", profile.AudioBitrateInKbps.ToString() + "kbps");
                }
            }
            keyTable("スケジューリングリソース設定", GetResourceString(profile));

            return sb.ToString();
        }
    }

    // プロファイルの更新情報
    [DataContract]
    public class ProfileUpdate
    {
        [DataMember]
        public UpdateType Type { get; set; }
        [DataMember]
        public ProfileSetting Profile { get; set; }
        [DataMember]
        public string NewName { get; set; }
    }

    // 基本設定データ
    [DataContract]
    public class Setting : IExtensibleDataObject
    {
        [DataMember]
        public string AmatsukazePath { get; set; }
        [DataMember]
        public string X264Path { get; set; }
        [DataMember]
        public string X265Path { get; set; }
        [DataMember]
        public string QSVEncPath { get; set; }
        [DataMember]
        public string NVEncPath { get; set; }
        [DataMember]
        public string VCEEncPath { get; set; }
        [DataMember]
        public string SVTAV1Path { get; set; }
        [DataMember]
        public string X262Path { get; set; }
        [DataMember]
        public string MuxerPath { get; set; }
        [DataMember]
        public string MKVMergePath { get; set; }
        [DataMember]
        public string MP4BoxPath { get; set; }
        [DataMember]
        public string TimelineEditorPath { get; set; }
        [DataMember]
        public string ChapterExePath { get; set; }
        [DataMember]
        public string JoinLogoScpPath { get; set; }
        [DataMember]
        public string TsReadExPath { get; set; }
        [DataMember]
        public string B24ToVttPath { get; set; }
        [DataMember]
        public string PsisiarcPath { get; set; }
        [DataMember]
        public string NicoConvASSPath { get; set; }
        [DataMember]
        public string TsMuxeRPath { get; set; }
        [DataMember]
        public string TsReplacePath { get; set; }
        [DataMember]
        public string SCRenamePath { get; set; }
        [DataMember]
        public string AutoVfrPath { get; set; }
        [DataMember]
        public string NeroAacEncPath { get; set; }
        [DataMember]
        public string QaacPath { get; set; }
        [DataMember]
        public string FdkaacPath { get; set; }
        [DataMember]
        public string OpusEncPath { get; set; }

        // Whisper 実行ファイルパス
        [DataMember]
        public string WhisperPath { get; set; }

        [DataMember]
        public string WorkPath { get; set; }
        [DataMember]
        public string AlwaysShowDisk { get; set; }

        [DataMember]
        public int NumParallel { get; set; }
        [DataMember]
        public int NumParallelLogoAnalysis { get; set; }
        [DataMember]
        public int AffinitySetting { get; set; } // 中身はProcessGroupKind
        [DataMember]
        public int ProcessPriority { get; set; } // 0が"通常以下"

        [DataMember]
        public bool ClearWorkDirOnStart { get; set; }
        [DataMember]
        public bool DeleteTaskWorkDirOnQueueRemove { get; set; }
        [DataMember]
        public bool HideOneSeg { get; set; }
        [DataMember]
        public int ListStyle { get; set; }
        [DataMember]
        public bool SupressSleep { get; set; }
        [DataMember]
        public bool LogoPendAsError { get; set; }
        [DataMember]
        public bool AutoLogoPendingDisabled { get; set; }
        [DataMember]
        public int AutoLogoPendingDivX { get; set; }
        [DataMember]
        public int AutoLogoPendingDivY { get; set; }
        [DataMember]
        public int AutoLogoPendingSearchFrames { get; set; }
        [DataMember]
        public int AutoLogoPendingBlockSize { get; set; }
        [DataMember]
        public int AutoLogoPendingThreshold { get; set; }
        [DataMember]
        public int AutoLogoPendingMarginX { get; set; }
        [DataMember]
        public int AutoLogoPendingMarginY { get; set; }
        [DataMember]
        public int AutoLogoPendingThreadN { get; set; }
        [DataMember]
        public bool AutoLogoPendingDetailedDebug { get; set; }
        [DataMember]
        public bool DumpFilter { get; set; }
        // CM解析のみ実行時にTrim・分割点情報を入力ディレクトリにコピーする
        [DataMember]
        public bool CopyTrimAVS { get; set; }
        [DataMember]
        public bool SchedulingEnabled { get; set; }
        [DataMember]
        public int NumGPU { get; set; }
        [DataMember]
        public int[] MaxGPUResources { get; set; } // 長さは常に16

        [DataMember]
        public bool EnableX265VFRTimeFactor { get; set; }
        [DataMember]
        public double X265VFRTimeFactor { get; set; }

        [DataMember]
        public bool PauseOnStarted { get; set; }
        [DataMember]
        public bool PrintTimePrefix { get; set; }
        [DataMember]
        public bool EnableShutdownAction { get; set; }
        [DataMember]
        public bool NoActionExe { get; set; }
        [DataMember]
        public List<string> NoActionExeList { get; set; }

        [DataMember]
        public bool EnableRunHours { get; set; }
        [DataMember]
        public bool RunHoursSuspendEncoders { get; set; }
        [DataMember]
        public bool[] RunHours { get; set; }
        [DataMember]
        public string ConsoleFontFamilyName { get; set; }

        [DataMember]
        public bool DeleteOldLogs { get; set; }

        [DataMember]
        public int DeleteOldLogsDays { get; set; }

        [DataMember]
        public bool ExclusiveBatExec { get; set; }

        [DataMember]
        public bool ExecuteBatchAfterQueue { get; set; }
        [DataMember]
        public string BatchFileAfterQueuePath { get; set; }

        [DataMember]
        public int TrimAdjustPreviewScaleMode { get; set; } = 1;

        [DataMember]
        public string TrimAdjustShortcutPrevEditPoint { get; set; }
        [DataMember]
        public string TrimAdjustShortcutBack4 { get; set; }
        [DataMember]
        public string TrimAdjustShortcutBack3 { get; set; }
        [DataMember]
        public string TrimAdjustShortcutBack2 { get; set; }
        [DataMember]
        public string TrimAdjustShortcutBack1 { get; set; }
        [DataMember]
        public string TrimAdjustShortcutForward1 { get; set; }
        [DataMember]
        public string TrimAdjustShortcutForward2 { get; set; }
        [DataMember]
        public string TrimAdjustShortcutForward3 { get; set; }
        [DataMember]
        public string TrimAdjustShortcutForward4 { get; set; }
        [DataMember]
        public string TrimAdjustShortcutNextEditPoint { get; set; }
        [DataMember]
        public string TrimAdjustShortcutToggleEditPoint { get; set; }

        [DataMember]
        public int TrimAdjustMoveFramesBack4 { get; set; }
        [DataMember]
        public int TrimAdjustMoveFramesBack3 { get; set; }
        [DataMember]
        public int TrimAdjustMoveFramesBack2 { get; set; }
        [DataMember]
        public int TrimAdjustMoveFramesForward2 { get; set; }
        [DataMember]
        public int TrimAdjustMoveFramesForward3 { get; set; }
        [DataMember]
        public int TrimAdjustMoveFramesForward4 { get; set; }

        // 再投入時に元のCM解析タスクを削除するか
        [DataMember]
        public bool TrimAdjustDeleteCmTask { get; set; }

        // 自動更新チェックを行う（未設定は有効として扱う）
        [DataMember]
        public bool? UpdateCheckEnabled { get; set; }
        // 更新チェック間隔（時間、0 は既定値の 24 時間として扱う）
        [DataMember]
        public int UpdateCheckIntervalHours { get; set; }
        // 更新チェックから除外する対象 ID
        [DataMember]
        public List<string> UpdateDisabledTargets { get; set; }
        // 更新時に使用するプロキシ（空ならシステム設定に従う）
        [DataMember]
        public string UpdateProxy { get; set; }

        public ExtensionDataObject ExtensionData { get; set; }

        public string ActualWorkPath
        {
            get
            {
                return string.IsNullOrEmpty(WorkPath) ? "./" : WorkPath;
            }
        }

        public System.Diagnostics.ProcessPriorityClass ProcessPriorityClass {
            get {
                return new System.Diagnostics.ProcessPriorityClass[]
                {
                    System.Diagnostics.ProcessPriorityClass.Idle,
                    System.Diagnostics.ProcessPriorityClass.BelowNormal,
                    System.Diagnostics.ProcessPriorityClass.Normal
                }[ProcessPriority + 1];
            }
        }
    }

    // 基本設定データ
    [DataContract]
    public class UIState : IExtensibleDataObject
    {
        [DataMember]
        public string LastUsedProfile { get; set; }
        [DataMember]
        public string LastOutputPath { get; set; }
        [DataMember]
        public string LastAddQueueBat { get; set; }
        [DataMember]
        public List<string> OutputPathHistory { get; set; }
        [DataMember]
        public DateTime? LastUpdateCheckedAt { get; set; }

        public ExtensionDataObject ExtensionData { get; set; }
    }

    // ロゴ設定
    [DataContract]
    public class LogoSetting : IExtensibleDataObject
    {
        [DataMember]
        public bool Exists { get; set; }
        [DataMember]
        public string FileName { get; set; }
        [DataMember]
        public string LogoName { get; set; }
        [DataMember]
        public int ServiceId { get; set; }
        [DataMember]
        public bool Enabled { get; set; }
        [DataMember]
        public DateTime From { get; set; }
        [DataMember]
        public DateTime To { get; set; }

        public bool CanUse(DateTime tstime)
        {
            return Exists && Enabled &&
                (tstime == DateTime.MinValue || (From <= tstime && tstime <= To));
        }

        public ExtensionDataObject ExtensionData { get; set; }

        public static readonly string NO_LOGO = "### NO LOGO ###";
    }

    // チャンネル設定の1チャンネル分
    [DataContract]
    public class ServiceSettingElement : IExtensibleDataObject
    {
        [DataMember]
        public int ServiceId { get; set; }
        [DataMember]
        public string ServiceName { get; set; }
        [DataMember]
        public bool DisableCMCheck { get; set; }
        [DataMember]
        public string JLSCommand { get; set; }
        [DataMember]
        public string JLSOption { get; set; }
        [DataMember]
        public List<LogoSetting> LogoSettings { get; set; }

        public ExtensionDataObject ExtensionData { get; set; }
    }

    // チャンネル設定（全チャンネル分）
    [DataContract]
    public class ServiceSetting : IExtensibleDataObject
    {
        [DataMember]
        public Dictionary<int, ServiceSettingElement> ServiceMap { get; set; }

        public ExtensionDataObject ExtensionData { get; set; }
    }

    public enum ProcMode
    {
        Batch,
        AutoBatch,
        Test,
        DrcsCheck,
        CMCheck,
    }

    [DataContract]
    public class AddQueueItem
    {
        [DataMember]
        public string Path; // フルパス
    }

    [DataContract]
    public class OutputInfo
    {
        [DataMember]
        public string DstPath { get; set; }
        [DataMember]
        public string Profile { get; set; }
        [DataMember]
        public int Priority { get; set; }
    }

    [DataContract]
    public class AddQueueRequest
    {
        [DataMember]
        public string DirPath { get; set; }
        [DataMember]
        public List<AddQueueItem> Targets { get; set; }
        [DataMember]
        public ProcMode Mode { get; set; }
        [DataMember]
        public List<OutputInfo> Outputs { get; set; }
        [DataMember]
        public string RequestId { get; set; }
        [DataMember]
        public string AddQueueBat { get; set; }

        /// <summary>キュー追加時に各ターゲットへ付与するタグ（null の場合は空リスト）。</summary>
        [DataMember]
        public List<string> Tags { get; set; }

        public bool IsBatch { get { return Mode == ProcMode.Batch || Mode == ProcMode.AutoBatch; } }
        public bool IsCheck { get { return Mode == ProcMode.DrcsCheck || Mode == ProcMode.CMCheck; } }
    }

    public enum QueueState
    {
        Queue,          // キュー状態
        Encoding,       // エンコード中
        Complete,       // 完了
        Failed,         // 失敗
        PreFailed,      // エンコード始める前に失敗
        LogoPending,    // ペンディング
        Canceled,       // キャンセルされた
    }

    public enum AutoLogoResultState
    {
        None,
        Success,
        Failed,
    }

    public enum GenreSpace
    {
        ARIB = 0,
        CS = 2,
    }

    // ジャンル
    [DataContract]
    public class GenreItem
    {
        // Level1==0xE(拡張)かどうかで入れられる値が変わる
        // 拡張でない場合
        //   Space は GenreSpace.ARIB
        //   Level1,Level2は通常のLevel1,Level2
        // 拡張の場合
        //   Space は Level2 + 1
        //   Level1,Level2はUser1,User2
        // SpaceはGenreSpaceが対応しているが、事業者定義の値であるため
        // GenreSpaceにない値が使用されることがあるのでint型とする
        // （GenreSpaceにするとシリアライズでエラーとなるので）
        [DataMember]
        public int Space;
        [DataMember]
        public int Level1;
        [DataMember]
        public int Level2;
    }

    [DataContract]
    public class QueueItem
    {
        [DataMember]
        public int Id { get; set; }
        [DataMember]
        public ProcMode Mode { get; set; }
        [DataMember]
        public ProfileSetting Profile { get; set; }
        [DataMember]
        public string SrcPath { get; set; }
        [DataMember]
        public string DstPath { get; set; }
        [DataMember]
        public QueueState State { get; set; }
        [DataMember]
        public int Priority { get; set; }
        [DataMember]
        public DateTime AddTime { get; set; }

        [DataMember]
        public VideoStreamFormat StreamFormat { get; set; }

        [DataMember]
        public int ServiceId { get; set; }
        [DataMember]
        public string ServiceName { get; set; }
        [DataMember]
        public int ImageWidth { get; set; }
        [DataMember]
        public int ImageHeight { get; set; }
        [DataMember]
        public DateTime TsTime { get; set; }
        [DataMember]
        public DateTime EITStartTime { get; set; }

        [DataMember]
        public string FailReason { get; set; }
        [DataMember]
        public string JlsCommand { get; set; }

        [DataMember]
        public string ProfileName { get; set; }
        [DataMember]
        public string EventName { get; set; }
        [DataMember]
        public List<GenreItem> Genre { get; set; }

        [DataMember]
        public string ActualDstPath { get; set; }
        [DataMember]
        public int ConsoleId { get; set; }

        [DataMember]
        public DateTime EncodeStart { get; set; }
        [DataMember]
        public TimeSpan EncodeTime { get; set; }

        [DataMember]
        public List<string> Tags { get; set; }

        // カット調整後に再利用するCM解析タスクの一時フォルダ
        [DataMember]
        public string ResumeDir { get; set; }

        [DataMember]
        public AutoLogoResultState AutoLogoResult { get; set; }
        [DataMember]
        public string AutoLogoLastMessage { get; set; }

        [DataMember]
        public bool AutoLogoQueued { get; set; }
        [DataMember]
        public bool AutoLogoInProgress { get; set; }

        // 内部処理順決定用パラメータ
        public int Order { get; set; }

        public DateTime EncodeFinish
        {
            get
            {
                if(EncodeStart == DateTime.MinValue || EncodeTime == TimeSpan.Zero)
                {
                    return DateTime.MinValue;
                }
                return EncodeStart + EncodeTime;
            }
        }

        public bool IsBatch { get { return Mode == ProcMode.Batch || Mode == ProcMode.AutoBatch; } }
        public bool IsCheck { get { return Mode == ProcMode.DrcsCheck || Mode == ProcMode.CMCheck; } }
        public bool IsTest { get { return Mode == ProcMode.Test; } }

        public string DirName { 
            get { 
                var dir = Path.GetDirectoryName(SrcPath);
                var delimiter = SrcPath.Contains('\\') ? '\\' : '/';
                return dir?.Replace(Path.DirectorySeparatorChar, delimiter);
            } 
        }
        public string FileName { get { return Path.GetFileName(SrcPath); } }

        public bool IsActive {
            get {
                return State == QueueState.Encoding ||
                    State == QueueState.LogoPending ||
                    State == QueueState.Queue;
            }
        }

        public bool IsOneSeg {
            get {
                return ImageWidth <= 320 || ImageHeight <= 260;
            }
        }

        public void Reset()
        {
            State = QueueState.LogoPending;
            EncodeStart = new DateTime();
            EncodeTime = new TimeSpan();
            ClearAutoLogoTransientState();
        }

        public void ClearAutoLogoTransientState()
        {
            AutoLogoQueued = false;
            AutoLogoInProgress = false;
        }

        public void ResetAutoLogoAttempt()
        {
            AutoLogoResult = AutoLogoResultState.None;
            AutoLogoLastMessage = null;
            ClearAutoLogoTransientState();
        }
    }

    [DataContract]
    public class QueueData
    {
        [DataMember]
        public List<QueueItem> Items { get; set; }
    }

    public enum UpdateType
    {
        Add, Remove, Update, Clear, Move
    }

    [DataContract]
    public class QueueUpdate
    {
        [DataMember]
        public UpdateType Type { get; set; }
        [DataMember]
        public QueueItem Item { get; set; }
        [DataMember]
        public int Position { get; set; }
    }

    public enum ChangeItemType
    {
        ResetState,      // リトライ
        UpdateProfile,// プロファイルを更新してリトライ
        Duplicate,      // 同じタスクを投入
        Cancel,     // キャンセル
        Priority,   // 優先度変更
        Profile,    // プロファイル変更
        RemoveItem, // アイテム削除
        RemoveCompleted,  // 完了項目を削除
        ForceStart, // アイテムを強制的に開始
        RemoveSourceFile, // ソースTSを削除（通常or自動追加の完了した項目のみ有効）
        Move,       // リスト上で移動
    }

    [DataContract]
    public class ChangeItemData
    {
        [DataMember]
        public int ItemId { get; set; }
        [DataMember]
        public int workerId { get; set; }
        [DataMember]
        public ChangeItemType ChangeType { get; set; }
        [DataMember]
        public int Priority { get; set; }
        [DataMember]
        public string Profile { get; set; }
        [DataMember]
        public ProcMode? Mode { get; set; }
        [DataMember]
        public int Position { get; set; }
        [DataMember]
        public string RequestId { get; set; }
    }

    [DataContract]
    public class AudioDiff
    {
        [DataMember]
        public int TotalSrcFrames { get; set; }
        [DataMember]
        public int TotalOutFrames { get; set; }
        [DataMember]
        public int TotalOutUniqueFrames { get; set; }
        [DataMember]
        public double NotIncludedPer { get; set; }
        [DataMember]
        public double AvgDiff { get; set; }
        [DataMember]
        public double MaxDiff { get; set; }
        [DataMember]
        public double MaxDiffPos { get; set; }
    }

    [DataContract]
    public class ErrorCount : IExtensibleDataObject
    {
        [DataMember]
        public string Name;
        [DataMember]
        public int Count;

        public ExtensionDataObject ExtensionData { get; set; }
    }

    public enum CheckType
    {
        DRCS, // DRCSサーチ
        CM    // CM解析
    }

    [DataContract]
    public class CheckLogItem : IExtensibleDataObject
    {
        [DataMember]
        public CheckType Type { get; set; }
        [DataMember]
        public string SrcPath { get; set; }
        [DataMember]
        public bool Success { get; set; }
        [DataMember]
        public DateTime CheckStartDate { get; set; }
        [DataMember]
        public DateTime CheckFinishDate { get; set; }
        [DataMember]
        public string Profile { get; set; }
        [DataMember]
        public string ServiceName { get; set; }
        [DataMember]
        public int ServiceId { get; set; }
        [DataMember]
        public DateTime TsTime { get; set; }
        [DataMember]
        public string Reason { get; set; }

        public ExtensionDataObject ExtensionData { get; set; }

        public string DisplayType {
            get {
                if (Type == CheckType.CM) return "CM解析";
                else if (Type == CheckType.DRCS) return "DRCSチェック";
                else return "不明";
            }
        }
        public TimeSpan EncodeDuration { get { return CheckFinishDate - CheckStartDate; } }
        public string DisplayResult { get { return Success ? "〇" : "×"; } }
        public string DisplaySrcDirectory { get { return Path.GetDirectoryName(SrcPath); } }
        public string DisplaySrcFileName { get { return Path.GetFileName(SrcPath); } }
        public string DisplayEncodeStart { get { return CheckStartDate.ToGUIString(); } }
        public string DisplayEncodeFinish { get { return CheckFinishDate.ToGUIString(); } }
        public string DisplayEncodeDuration { get { return EncodeDuration.ToGUIString(); } }
        public string DisplayReason { get { return Reason; } }
        public string DisplayService { get { return ServiceName + "(" + ServiceId + ")"; } }
        public string DisplayTsTime { get { return (TsTime == DateTime.MinValue) ? "不明" : TsTime.ToString("yyyy年M月d日", CultureInfo.InvariantCulture); } }
    }

    [DataContract]
    public class CheckLogData : IExtensibleDataObject
    {
        [DataMember]
        public List<CheckLogItem> Items { get; set; }

        public ExtensionDataObject ExtensionData { get; set; }
    }

    [DataContract]
    public class LogItem : IExtensibleDataObject
    {
        [DataMember]
        public string SrcPath { get; set; }
        [DataMember]
        public bool Success { get; set; }
        [DataMember]
        public string DstPath { get; set; } // 拡張子を含まないメイン出力先パス
        [DataMember]
        public List<string> OutPath { get; set; } // 実際に出力されたファイル
        [DataMember]
        public DateTime EncodeStartDate { get; set; }
        [DataMember]
        public DateTime EncodeFinishDate { get; set; }
        [DataMember]
        public TimeSpan SrcVideoDuration { get; set; }
        [DataMember]
        public TimeSpan OutVideoDuration { get; set; }
        [DataMember]
        public long SrcFileSize { get; set; }
        [DataMember]
        public long IntVideoFileSize { get; set; }
        [DataMember]
        public long OutFileSize { get; set; }
        [DataMember]
        public AudioDiff AudioDiff { get; set; }
        [DataMember]
        public string Reason { get; set; }
        [DataMember]
        public string MachineName { get; set; }
        [DataMember]
        public List<string> LogoFiles { get; set; }

        [DataMember]
        public bool Chapter { get; set; }
        [DataMember]
        public bool NicoJK { get; set; }
        [DataMember]
        public bool TrimAVS { get; set; }
        [DataMember]
        public int OutputMask { get; set; }
        [DataMember]
        public string ServiceName { get; set; }
        [DataMember]
        public int ServiceId { get; set; }
        [DataMember]
        public DateTime TsTime { get; set; }

        [DataMember]
        public int Incident { get; set; }
        [DataMember]
        public List<ErrorCount> Error { get; set; }

        [DataMember]
        public string Profile { get; set; }

        [DataMember]
        public List<string> Tags { get; set; }

        public ExtensionDataObject ExtensionData { get; set; }

        // アクセサ
        public TimeSpan EncodeDuration { get { return EncodeFinishDate - EncodeStartDate; } }
        public string DisplayResult { get { return Success ? ((Incident > 0) ? "△" : "〇") : "×"; } }
        public string DisplaySrcDirectory { get { return Path.GetDirectoryName(SrcPath); } }
        public string DisplaySrcFileName { get { return Path.GetFileName(SrcPath); } }
        public string DisplayOutDirectory { get { return (OutPath != null && OutPath.Count > 0) ? Path.GetDirectoryName(OutPath[0]) : "-"; } }
        public IEnumerable<string> DisplayOutFile { get { return (OutPath == null) ? null : OutPath.Select(s => Path.GetFileName(s)); } }
        public string DisplayOutNum { get { return (OutPath == null) ? "-" : OutPath.Count.ToString(); } }
        public string DisplayNumIncident { get { return Incident.ToString(); } }
        public string DisplayEncodeStart { get { return EncodeStartDate.ToGUIString(); } }
        public string DisplayEncodeFinish { get { return EncodeFinishDate.ToGUIString(); } }
        public string DisplayEncodeDuration { get { return EncodeDuration.ToGUIString(); } }
        public string DisplaySrcDurationo { get { return (SrcVideoDuration != TimeSpan.Zero) ? SrcVideoDuration.ToGUIString() : null; } }
        public string DisplayOutDuration { get { return (OutVideoDuration != TimeSpan.Zero) ? OutVideoDuration.ToGUIString() : null; } }
        public string DisplayVideoNotIncluded
        {
            get
            {
                if (SrcVideoDuration == TimeSpan.Zero) return null;
                var s = SrcVideoDuration.TotalMilliseconds;
                var o = OutVideoDuration.TotalMilliseconds;
                return ((double)(s - o) / (double)s * 100.0).ToString("F2");
            }
        }
        public string DisplaySrcFileSize { get { return ((double)SrcFileSize / (1024 * 1024)).ToString("F2"); } }
        public string DisplayIntFileSize { get { return ((double)IntVideoFileSize / (1024 * 1024)).ToString("F2"); } }
        public string DisplayOutFileSize { get { return ((double)OutFileSize / (1024 * 1024)).ToString("F2"); } }
        public string DisplayIntVideoRate { get { return ((double)IntVideoFileSize / (double)SrcFileSize * 100).ToString("F2"); } }
        public string DisplayCompressionRate { get { return ((double)OutFileSize / (double)SrcFileSize * 100).ToString("F2"); } }
        public string DisplaySrcAudioFrames { get { return (AudioDiff != null) ? AudioDiff.TotalSrcFrames.ToString() : null; } }
        public string DisplayOutAudioFrames { get { return (AudioDiff != null) ? AudioDiff.TotalOutFrames.ToString() : null; } }
        public string DisplayAudioNotIncluded { get { return (AudioDiff != null) ? AudioDiff.NotIncludedPer.ToString("F3") : null; } }
        public string DisplayAvgAudioDiff { get { return (AudioDiff != null) ? AudioDiff.AvgDiff.ToString("F2") : null; } }
        public string DisplayReason { get { return Reason; } }
        public string DisplayAudioMaxDiff { get { return (AudioDiff != null) ? AudioDiff.MaxDiff.ToString("F2") : null; } }
        public string DisplayAudioMaxDiffPos { get { return (AudioDiff != null) ? AudioDiff.MaxDiffPos.ToString("F2") : null; } }
        public string DisplayEncodeSpeed
        {
            get
            {
                return (SrcVideoDuration != TimeSpan.Zero)
? (SrcVideoDuration.TotalSeconds / EncodeDuration.TotalSeconds).ToString("F2")
: null;
            }
        }
        public string DisplaySrcBitrate
        {
            get
            {
                return (SrcVideoDuration != TimeSpan.Zero)
? ((double)SrcFileSize / (SrcVideoDuration.TotalSeconds * 128.0 * 1024)).ToString("F3")
: null;
            }
        }
        public string DisplayOutBitrate
        {
            get
            {
                return (SrcVideoDuration != TimeSpan.Zero)
? ((double)OutFileSize / (OutVideoDuration.TotalSeconds * 128.0 * 1024)).ToString("F3")
: null;
            }
        }
        public string DisplayLogo { get { return LogoFiles?.FirstOrDefault() ?? "なし"; } }
        public string DisplayChapter { get { return Chapter ? "○" : "☓"; } }
        public string DisplayNicoJK { get { return NicoJK ? "○" : "☓"; } }
        public string DisplayTrimAVS { get { return TrimAVS ? "○" : "☓"; } }
        public string DisplayOutputMask {
            get {
                var labels = new List<string>();
                if ((OutputMask & 1) != 0) labels.Add("通常");
                if ((OutputMask & 2) != 0) labels.Add("CMをカット");
                if ((OutputMask & 4) != 0) labels.Add("CMのみ出力");
                if ((OutputMask & 8) != 0) labels.Add("前後のCMのみカット");
                if (labels.Count == 0) {
                    return (OutputMask == 0) ? "なし" : "不明";
                }
                return string.Join("/", labels);
            }
        }
        public string DisplayService { get { return ServiceName + "(" + ServiceId + ")"; } }
        public string DisplayTsTime { get { return (TsTime == DateTime.MinValue) ? "不明" : TsTime.ToString("yyyy年M月d日", CultureInfo.InvariantCulture); } }
        public string DisplayTags { get { return (Tags != null) ? string.Join(" ", Tags) : ""; } }
    }

    [DataContract]
    public class LogData : IExtensibleDataObject
    {
        [DataMember]
        public List<LogItem> Items { get; set; }

        public ExtensionDataObject ExtensionData { get; set; }
    }

    [DataContract]
    public class LogFile
    {
        [DataMember]
        public long Id { get; set; }
        [DataMember]
        public string Content { get; set; }
    }

    [DataContract]
    public class ConsoleData
    {
        [DataMember]
        public int index { get; set; }
        [DataMember]
        public List<string> text { get; set; }
    }

    [DataContract]
    public class ConsoleUpdate
    {
        [DataMember]
        public int index { get; set; }
        [DataMember]
        public byte[] data { get; set; }
    }

    [DataContract]
    public class FinishSetting
    {
        [DataMember]
        public FinishAction Action { get; set; }
        [DataMember]
        public int Seconds { get; set; }
        [DataMember]
        public bool noActionExe { get; set; }
        [DataMember]
        public List<string> noActionExeList { get; set; }
    }

    public enum StateChangeEvent
    {
        WorkersStarted,
        WorkersFinished,
        EncodeStarted,
        EncodeSucceeded,
        EncodeFailed,
        EncodeCanceled,
    }

    [DataContract]
    public class UIData
    {
        [DataMember]
        public State State { get; set; }
        [DataMember]
        public QueueData QueueData { get; set; }
        [DataMember]
        public QueueUpdate QueueUpdate { get; set; }
        [DataMember]
        public LogData LogData { get; set; }
        [DataMember]
        public LogItem LogItem { get; set; }
        [DataMember]
        public CheckLogData CheckLogData { get; set; }
        [DataMember]
        public CheckLogItem CheckLogItem { get; set; }
        [DataMember]
        public ConsoleData ConsoleData { get; set; }
        [DataMember]
        public EncodeState EncodeState { get; set; }
        [DataMember]
        public FinishSetting SleepCancel { get; set; }
        [DataMember]
        public StateChangeEvent? StateChangeEvent { get; set; }
    }

    [DataContract]
    public class EncodeState
    {
        [DataMember]
        public int ConsoleId { get; set; }
        [DataMember]
        public ResourcePhase Phase { get; set; }
        [DataMember]
        public Resource Resource { get; set; }
    }

    [DataContract]
    public class LogFileRequest
    {
        [DataMember]
        public LogItem LogItem { get; set; }
        [DataMember]
        public CheckLogItem CheckLogItem { get; set; }
        [DataMember]
        public string RequestId { get; set; }
    }

        [DataContract]
    public class DiskItem
    {
        [DataMember]
        public string Path { get; set; }
        [DataMember]
        public long Capacity { get; set; }
        [DataMember]
        public long Free { get; set; }
    }

    [DataContract]
    public class LogoData
    {
        [DataMember]
        public int ServiceId { get; set; }
        [DataMember]
        public string FileName { get; set; }
        [DataMember]
        public int ImageWith { get; set; }
        [DataMember]
        public int ImageHeight { get; set; }

        // BitmapFrameをobjectに変更
        public object Image { get; set; }
    }

    public enum ServiceSettingUpdateType
    {
        Update,
        AddNoLogo,
        Remove,
        RemoveLogo,
        Clear,
    }

    [DataContract]
    public class ServiceSettingUpdate
    {
        [DataMember]
        public ServiceSettingUpdateType Type { get; set; }

        [DataMember]
        public int ServiceId { get; set; }

        [DataMember]
        public ServiceSettingElement Data { get; set; }

        [DataMember]
        public int RemoveLogoIndex { get; set; }
    }

    public enum DrcsUpdateType
    {
        Remove, Update
    }

    [DataContract]
    public class DrcsSourceItem
    {
        // 当該TSファイル名
        [DataMember]
        public string FileName { get; set; }

        // DRCS文字を発見した日時（DRCSチェックやエンコードを走らせた時刻）
        [DataMember]
        public DateTime FoundTime { get; set; }

        // 当該DRCS文字出現の映像開始からの時間
        [DataMember]
        public TimeSpan Elapsed { get; set; }

        public override string ToString()
        {
            return Elapsed.ToString(@"h\:mm\:ss") + "@" + FileName;
        }
    }

    [DataContract]
    public class DrcsImage
    {
        [DataMember]
        public string MD5 { get; set; }
        [DataMember]
        public string MapStr { get; set; }

        // 出現リスト
        [DataMember]
        public DrcsSourceItem[] SourceList { get; set; }

        // BitmapFrameをobjectに変更
        public object Image { get; set; }

        // マッピング更新要求では画像本体は不要。
        // WPFのBitmapSourceをRPCコピー対象にしないため、MD5/MapStrのみを持つDTOを作る。
        public static DrcsImage CreateMapUpdate(string md5, string mapStr)
        {
            return new DrcsImage()
            {
                MD5 = md5,
                MapStr = mapStr
            };
        }
    }

    [DataContract]
    public class DrcsImageUpdate
    {
        [DataMember]
        public DrcsUpdateType Type { get; set; }

        [DataMember]
        public DrcsImage Image;

        [DataMember]
        public List<DrcsImage> ImageList;
    }

    [DataContract]
    public class ServerInfo
    {
        [DataMember]
        public string HostName { get; set; }
        [DataMember]
        public byte[] MacAddress { get; set; }
        [DataMember]
        public string Version { get; set; }
        [DataMember]
        public string Platform { get; set; }
        [DataMember]
        public int CharSet { get; set; }
        [DataMember]
        public int LogicalProcessorCount { get; set; }
        [DataMember]
        public int RestApiPort { get; set; }
    }

    [DataContract]
    public class State
    {
        // キュー停止
        [DataMember]
        public bool Pause { get; set; }

        // エンコードサスペンド
        [DataMember]
        public bool Suspend { get; set; }

        // エンコードサスペンド
        [DataMember]
        public bool[] EncoderSuspended { get; set; }

        // エンコード実行中
        [DataMember]
        public bool Running { get; set; }

        // スケジューリングにより一時停止状態
        [DataMember]
        public bool ScheduledPause { get; set; }

        // 更新適用のためにキューを停止中
        [DataMember]
        public bool MaintenancePaused { get; set; }

        // スケジューリングにより一時停止状態
        [DataMember]
        public bool ScheduledSuspend { get; set; }

        // 進捗
        [DataMember]
        public double Progress { get; set; }
    }

    [DataContract]
    public class PauseRequest
    {
        // true: キュー停止, false: エンコーダ停止
        [DataMember]
        public bool IsQueue { get; set; }
        // 停止するエンコーダ -1の場合は全エンコーダ
        [DataMember]
        public int Index { get; set; }
        // 停止か稼働か
        [DataMember]
        public bool Pause { get; set; }
    }

    [DataContract]
    public class MakeScriptData
    {
        [DataMember]
        public string Profile { get; set; }
        [DataMember]
        public string OutDir { get; set; }
        [DataMember]
        public string NasDir { get; set; }
        [DataMember]
        public bool IsNasEnabled { get; set; }
        [DataMember]
        public bool IsWakeOnLan { get; set; }
        [DataMember]
        public bool MoveAfter { get; set; }
        [DataMember]
        public bool ClearEncoded { get; set; }
        [DataMember]
        public bool WithRelated { get; set; }
        [DataMember]
        public bool IsDirect { get; set; }
        [DataMember]
        public int Priority { get; set; }
        [DataMember]
        public string AddQueueBat { get; set; }
    }

    [DataContract]
    public class CommonData
    {
        [DataMember]
        public UIState UIState { get; set; }
        [DataMember]
        public Setting Setting { get; set; }
        [DataMember]
        public MakeScriptData MakeScriptData { get; set; }
        [DataMember]
        public List<string> JlsCommandFiles { get; set; }
        [DataMember]
        public List<string> MainScriptFiles { get; set; }
        [DataMember]
        public List<string> PostScriptFiles { get; set; }
        [DataMember]
        public List<DiskItem> Disks { get; set; }
        [DataMember]
        public List<int> CpuClusters { get; set; }
        [DataMember]
        public ServerInfo ServerInfo { get; set; }
        [DataMember]
        public List<string> AddQueueBatFiles { get; set; }
        [DataMember]
        public List<string> PreBatFiles { get; set; }
        [DataMember]
        public List<string> PreEncodeBatFiles { get; set; }
        [DataMember]
        public List<string> PostBatFiles { get; set; }
        [DataMember]
        public List<string> QueueFinishBatFiles { get; set; }
        [DataMember]
        public FinishSetting FinishSetting { get; set; }
    }

    [DataContract]
    public class OperationResult
    {
        [DataMember]
        public bool IsFailed { get; set; }
        [DataMember]
        public string Message { get; set; }
        [DataMember]
        public string StackTrace { get; set; }
    }

    public enum VideoSizeCondition
    {
        FullHD, HD1440, SD, OneSeg
    }

    [DataContract]
    public class AutoSelectCondition
    {
        [DataMember]
        public bool FileNameEnabled { get; set; }
        [DataMember]
        public string FileName { get; set; }
        [DataMember]
        public bool ContentConditionEnabled { get; set; }
        [DataMember]
        public List<GenreItem> ContentConditions { get; set; }
        [DataMember]
        public bool MatchAllGenres { get; set; }
        [DataMember]
        public bool ServiceIdEnabled { get; set; }
        [DataMember]
        public List<int> ServiceIds { get; set; }
        [DataMember]
        public bool VideoSizeEnabled { get; set; }
        [DataMember]
        public List<VideoSizeCondition> VideoSizes { get; set; }
        [DataMember]
        public bool TagEnabled { get; set; }
        [DataMember]
        public string Tag { get; set; }
        [DataMember]
        public string Profile { get; set; }
        [DataMember]
        public int Priority { get; set; }
        [DataMember]
        public string Description { get; set; }
    }

    [DataContract]
    public class AutoSelectProfile
    {
        [DataMember]
        public string Name { get; set; }
        [DataMember]
        public List<AutoSelectCondition> Conditions { get; set; }
    }

    [DataContract]
    public class AutoSelectUpdate
    {
        [DataMember]
        public UpdateType Type { get; set; }
        [DataMember]
        public AutoSelectProfile Profile { get; set; }
        [DataMember]
        public string NewName { get; set; }
    }
}
