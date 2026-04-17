using System;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;

namespace Amatsukaze.Lib
{
    public static class AmatsukazeNatives
    {
#if WINDOWS
        public const string AmatsukazeLibName = "Amatsukaze.dll";
        public const CharSet AmatsukazeLibCharSet = CharSet.Unicode;
#else
        public const string AmatsukazeLibName = "libAmatsukaze.so";
        public const CharSet AmatsukazeLibCharSet = CharSet.Ansi;
#endif
    }

    public enum LogoRectDetectFail
    {
        None = 0,
        InsufficientScorePixels = 1,
        NoSeed = 2,
        BinaryFallbackUsed = 3,
        NoBestComponent = 4,
        RectSizeAbnormal = 5,
        RectPositionAbnormal = 6,
    }

    public enum LogoAnalyzeFail
    {
        None = 0,
        Pass2RoiTooSmall = 1,
        GetLogoNull = 2,
        CorrSequenceInvalid = 3,
        FrameMaskEmpty = 4,
        TooFewAcceptedFrames = 5,
        Pass2RectDiverged = 6,
    }

    public sealed class AutoDetectLogoRectResult
    {
        public int X { get; init; }
        public int Y { get; init; }
        public int W { get; init; }
        public int H { get; init; }
        public LogoRectDetectFail RectDetectFail { get; init; }
        public LogoAnalyzeFail LogoAnalyzeFail { get; init; }
        public double Pass1ScoreMax { get; init; }
        public double Pass2ScoreMax { get; init; }
        public double FinalScoreBeforeRescueMax { get; init; }
        public bool Pass2Entered { get; init; }
        public bool Pass2PrepareSucceeded { get; init; }
        public bool Pass2CollectSucceeded { get; init; }
        public bool Pass2RescueFallbackApplied { get; init; }
        public LogoAnalyzeFail Pass2FailBeforeClear { get; init; }
        public int Pass2FrameMaskNonZero { get; init; }
        public int Pass2AcceptedFrames { get; init; }
        public int Pass2SkippedFrames { get; init; }
        public int FrameGateRetryAttemptCount { get; init; }
        public int FrameGateRetrySuccessAttempt { get; init; }
    }

    public sealed class AutoDetectLogoRectException : IOException
    {
        public LogoRectDetectFail RectDetectFail { get; }
        public LogoAnalyzeFail LogoAnalyzeFail { get; }
        public double Pass1ScoreMax { get; }
        public double Pass2ScoreMax { get; }
        public double FinalScoreBeforeRescueMax { get; }
        public bool Pass2Entered { get; }
        public bool Pass2PrepareSucceeded { get; }
        public bool Pass2CollectSucceeded { get; }
        public bool Pass2RescueFallbackApplied { get; }
        public LogoAnalyzeFail Pass2FailBeforeClear { get; }
        public int Pass2FrameMaskNonZero { get; }
        public int Pass2AcceptedFrames { get; }
        public int Pass2SkippedFrames { get; }
        public int FrameGateRetryAttemptCount { get; }
        public int FrameGateRetrySuccessAttempt { get; }

        public AutoDetectLogoRectException(
            string message,
            LogoRectDetectFail rectDetectFail,
            LogoAnalyzeFail logoAnalyzeFail,
            double pass1ScoreMax,
            double pass2ScoreMax,
            double finalScoreBeforeRescueMax,
            bool pass2Entered,
            bool pass2PrepareSucceeded,
            bool pass2CollectSucceeded,
            bool pass2RescueFallbackApplied,
            LogoAnalyzeFail pass2FailBeforeClear,
            int pass2FrameMaskNonZero,
            int pass2AcceptedFrames,
            int pass2SkippedFrames,
            int frameGateRetryAttemptCount,
            int frameGateRetrySuccessAttempt)
            : base(message)
        {
            RectDetectFail = rectDetectFail;
            LogoAnalyzeFail = logoAnalyzeFail;
            Pass1ScoreMax = pass1ScoreMax;
            Pass2ScoreMax = pass2ScoreMax;
            FinalScoreBeforeRescueMax = finalScoreBeforeRescueMax;
            Pass2Entered = pass2Entered;
            Pass2PrepareSucceeded = pass2PrepareSucceeded;
            Pass2CollectSucceeded = pass2CollectSucceeded;
            Pass2RescueFallbackApplied = pass2RescueFallbackApplied;
            Pass2FailBeforeClear = pass2FailBeforeClear;
            Pass2FrameMaskNonZero = pass2FrameMaskNonZero;
            Pass2AcceptedFrames = pass2AcceptedFrames;
            Pass2SkippedFrames = pass2SkippedFrames;
            FrameGateRetryAttemptCount = frameGateRetryAttemptCount;
            FrameGateRetrySuccessAttempt = frameGateRetrySuccessAttempt;
        }
    }

    public class AMTContext : IDisposable
    {
        public IntPtr Ptr { private set; get; }

        #region Natives
        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern void InitAmatsukazeDLL();

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern IntPtr AMTContext_Create();

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern void ATMContext_Delete(IntPtr ptr);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern IntPtr AMTContext_GetError(IntPtr ctx);
        #endregion

        static AMTContext()
        {
            InitAmatsukazeDLL();
        }

        public AMTContext()
        {
            Ptr = AMTContext_Create();
        }

        #region IDisposable Support
        private bool disposedValue = false; // 重複する呼び出しを検出するには

        protected virtual void Dispose(bool disposing)
        {
            if (!disposedValue)
            {
                ATMContext_Delete(Ptr);
                Ptr = IntPtr.Zero;
                disposedValue = true;
            }
        }

        ~AMTContext()
        {
            // このコードを変更しないでください。クリーンアップ コードを上の Dispose(bool disposing) に記述します。
            Dispose(false);
        }

        // このコードは、破棄可能なパターンを正しく実装できるように追加されました。
        public void Dispose()
        {
            // このコードを変更しないでください。クリーンアップ コードを上の Dispose(bool disposing) に記述します。
            Dispose(true);
            GC.SuppressFinalize(this);
        }
        #endregion

        public string GetError()
        {
            return Marshal.PtrToStringAnsi(AMTContext_GetError(Ptr));
        }
    }

    public class ContentNibbles
    {
        public int Level1;
        public int Level2;
        public int User1;
        public int User2;
    }

    public class Program
    {
        public int ServiceId;
        public bool HasVideo;
        public int VideoPid;
        public int Stream;
        public int Width;
        public int Height;
        public int SarW;
        public int SarH;
        public string EventName;
        public string EventText;
        public ContentNibbles[] Content;
    }

    public class Service
    {
        public int ServiceId;
        public string ProviderName;
        public string ServiceName;
    }

    public class TsInfo : IDisposable
    {
        public AMTContext Ctx { private set; get; }
        public IntPtr Ptr { private set; get; }

        #region Natives
        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern IntPtr TsInfo_Create(IntPtr ctx);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern void TsInfo_Delete(IntPtr ptr);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName, CharSet = AmatsukazeNatives.AmatsukazeLibCharSet)]
        private static extern int TsInfo_ReadFile(IntPtr ptr, string filepath);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern int TsInfo_HasServiceInfo(IntPtr ptr);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern void TsInfo_GetDay(IntPtr ptr, out int y, out int m, out int d);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern void TsInfo_GetTime(IntPtr ptr, out int h, out int m, out int s);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern void TsInfo_GetStartDay(IntPtr ptr, out int y, out int m, out int d);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern void TsInfo_GetStartTime(IntPtr ptr, out int h, out int m, out int s);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern int TsInfo_GetNumProgram(IntPtr ptr);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern void TsInfo_GetProgramInfo(IntPtr ptr, int i, out int progId, out bool hasVideo, out int videoPid, out int numContent);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern void TsInfo_GetContentNibbles(IntPtr ptr, int i, int ci, out int level1, out int level2, out int user1, out int user2);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern void TsInfo_GetVideoFormat(IntPtr ptr, int i, out int stream, out int width, out int height, out int sarW, out int  sarH);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern int TsInfo_GetNumService(IntPtr ptr);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern int TsInfo_GetServiceId(IntPtr ptr, int i);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern IntPtr TsInfo_GetProviderName(IntPtr ptr, int i);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern IntPtr TsInfo_GetServiceName(IntPtr ptr, int i);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern IntPtr TsInfo_GetEventName(IntPtr ptr, int i);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern IntPtr TsInfo_GetEventText(IntPtr ptr, int i);
        #endregion

        public TsInfo(AMTContext ctx)
        {
            Ctx = ctx;
            Ptr = TsInfo_Create(Ctx.Ptr);
            if(Ptr == IntPtr.Zero)
            {
                throw new IOException(Ctx.GetError());
            }
        }

        #region IDisposable Support
        private bool disposedValue = false; // 重複する呼び出しを検出するには

        protected virtual void Dispose(bool disposing)
        {
            if (!disposedValue)
            {
                TsInfo_Delete(Ptr);
                Ptr = IntPtr.Zero;
                disposedValue = true;
            }
        }

        ~TsInfo()
        {
            // このコードを変更しないでください。クリーンアップ コードを上の Dispose(bool disposing) に記述します。
            Dispose(false);
        }

        // このコードは、破棄可能なパターンを正しく実装できるように追加されました。
        public void Dispose()
        {
            // このコードを変更しないでください。クリーンアップ コードを上の Dispose(bool disposing) に記述します。
            Dispose(true);
            GC.SuppressFinalize(this);
        }
        #endregion

        public bool ReadFile(string filepath)
        {
            return TsInfo_ReadFile(Ptr, filepath) != 0;
        }

        public bool HasServiceInfo
        {
            get
            {
                return TsInfo_HasServiceInfo(Ptr) != 0;
            }
        }

        public Program[] GetProgramList()
        {
            return Enumerable.Range(0, TsInfo_GetNumProgram(Ptr))
                .Select(i => {
                    Program prog = new Program();
                    int numContent;
                    TsInfo_GetProgramInfo(Ptr, i,
                        out prog.ServiceId, out prog.HasVideo, out prog.VideoPid, out numContent);
                    TsInfo_GetVideoFormat(Ptr, i,
                        out prog.Stream, out prog.Width, out prog.Height, out prog.SarW, out prog.SarH);
                    prog.EventName = Marshal.PtrToStringUTF8(TsInfo_GetEventName(Ptr, i));
                    prog.EventText = Marshal.PtrToStringUTF8(TsInfo_GetEventText(Ptr, i));
                    prog.Content = Enumerable.Range(0, numContent)
                        .Select(ci => {
                            ContentNibbles data = new ContentNibbles();
                            TsInfo_GetContentNibbles(Ptr, i, ci,
                                out data.Level1, out data.Level2, out data.User1, out data.User2);
                            return data;
                        }).ToArray();
                    return prog;
                }).ToArray();
        }

        // ServiceInfoがある場合のみ
        public DateTime GetTime()
        {
            int year, month, day, hour, minute, second;
            TsInfo_GetDay(Ptr, out year, out month, out day);
            TsInfo_GetTime(Ptr, out hour, out minute, out second);
            return new DateTime(year, month, day, hour, minute, second);
        }

        // EIT(start_time)。不正値の場合は MinValue を返す
        public DateTime GetEITStartTime()
        {
            int year, month, day, hour, minute, second;
            TsInfo_GetStartDay(Ptr, out year, out month, out day);
            TsInfo_GetStartTime(Ptr, out hour, out minute, out second);
            try
            {
                if (year <= 1 || month <= 0 || day <= 0)
                {
                    return DateTime.MinValue;
                }
                return new DateTime(year, month, day, hour, minute, second);
            }
            catch
            {
                return DateTime.MinValue;
            }
        }

        // ServiceInfoがある場合のみ
        public Service[] GetServiceList()
        {
            return Enumerable.Range(0, TsInfo_GetNumService(Ptr))
                .Select(i => new Service() {
                    ServiceId = TsInfo_GetServiceId(Ptr, i),
                    ProviderName = Marshal.PtrToStringUTF8(TsInfo_GetProviderName(Ptr, i)),
                    ServiceName = Marshal.PtrToStringUTF8(TsInfo_GetServiceName(Ptr, i))
                }).ToArray();
        }
    }

    public class MediaFile : IDisposable
    {
        public AMTContext Ctx { private set; get; }
        public IntPtr Ptr { private set; get; }

        #region Natives
        [DllImport(AmatsukazeNatives.AmatsukazeLibName, CharSet = AmatsukazeNatives.AmatsukazeLibCharSet)]
        private static extern IntPtr MediaFile_Create(IntPtr ctx, string filepath, int serviceid);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern void MediaFile_Delete(IntPtr ptr);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern int MediaFile_DecodeFrame(IntPtr ptr, float pos, ref int width, ref int height);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static unsafe extern void MediaFile_GetFrame(IntPtr ptr, byte* rgb, int width, int height);
        #endregion

        public MediaFile(AMTContext ctx, string filepath, int serviceid)
        {
            Ctx = ctx;
            Ptr = MediaFile_Create(Ctx.Ptr, filepath, serviceid);
            if (Ptr == IntPtr.Zero)
            {
                throw new IOException(Ctx.GetError());
            }
        }

        #region IDisposable Support
        private bool disposedValue = false; // 重複する呼び出しを検出するには

        protected virtual void Dispose(bool disposing)
        {
            if (!disposedValue)
            {
                MediaFile_Delete(Ptr);
                Ptr = IntPtr.Zero;
                disposedValue = true;
            }
        }

        ~MediaFile()
        {
            // このコードを変更しないでください。クリーンアップ コードを上の Dispose(bool disposing) に記述します。
            Dispose(false);
        }

        // このコードは、破棄可能なパターンを正しく実装できるように追加されました。
        public void Dispose()
        {
            // このコードを変更しないでください。クリーンアップ コードを上の Dispose(bool disposing) に記述します。
            Dispose(true);
            GC.SuppressFinalize(this);
        }
        #endregion

        // 失敗したらnullが返るので注意
        public object GetFrame(float pos)
        {
            int width = 0, height = 0;
            if(MediaFile_DecodeFrame(Ptr, pos, ref width, ref height) != 0)
            {
                if(width != 0 && height != 0)
                {
                    int stride = width * 3;
                    byte[] buffer = new byte[stride * height];
                    unsafe
                    {
                        fixed (byte* pbuffer = buffer)
                        {
                            MediaFile_GetFrame(Ptr, pbuffer, width, height);
                        }
                    }
                    return BitmapManager.CreateBitmapFromRgb(buffer, width, height, stride);
                }
            }
            return null;
        }
    }

    public class TrimAdjust : IDisposable
    {
        public AMTContext Ctx { private set; get; }
        public IntPtr Ptr { private set; get; }

        #region Natives
        [DllImport(AmatsukazeNatives.AmatsukazeLibName, CharSet = AmatsukazeNatives.AmatsukazeLibCharSet)]
        private static extern IntPtr TrimAdjust_Create(IntPtr ctx, string datFilePath, int scaleMode);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern void TrimAdjust_Delete(IntPtr ptr);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern int TrimAdjust_GetNumFrames(IntPtr ptr);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern int TrimAdjust_GetWidth(IntPtr ptr);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern int TrimAdjust_GetHeight(IntPtr ptr);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern int TrimAdjust_DecodeFrame(IntPtr ptr, int frameNumber, ref int width, ref int height);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static unsafe extern int TrimAdjust_GetFrameJpeg(IntPtr ptr, int frameNumber,
            out IntPtr jpegData, out int jpegSize);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern int TrimAdjust_GetFrameInfo(IntPtr ptr, int frameNumber,
            ref long pts, ref long duration, ref int keyFrame, ref int cmType);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static unsafe extern int TrimAdjust_GetWaveformJpeg(IntPtr ptr, int frameNumber,
            out IntPtr jpegData, out int jpegSize);
        #endregion

        public TrimAdjust(AMTContext ctx, string datFilePath, int scaleMode)
        {
            Ctx = ctx;
            try
            {
                Ptr = TrimAdjust_Create(Ctx.Ptr, datFilePath, scaleMode);
            }
            catch (SEHException ex)
            {
                var nativeError = Ctx?.GetError();
                throw new IOException(
                    $"TrimAdjust_Createでネイティブ例外が発生しました (HRESULT=0x{ex.HResult:X8}, dat={datFilePath}, scaleMode={scaleMode}, nativeError={nativeError ?? "<none>"})",
                    ex);
            }
            if (Ptr == IntPtr.Zero)
            {
                throw new IOException(
                    $"TrimAdjust_Createが失敗しました (dat={datFilePath}, scaleMode={scaleMode}): {Ctx.GetError()}");
            }
        }

        #region IDisposable Support
        private bool disposedValue = false;

        protected virtual void Dispose(bool disposing)
        {
            if (!disposedValue)
            {
                TrimAdjust_Delete(Ptr);
                Ptr = IntPtr.Zero;
                disposedValue = true;
            }
        }

        ~TrimAdjust()
        {
            Dispose(false);
        }

        public void Dispose()
        {
            Dispose(true);
            GC.SuppressFinalize(this);
        }
        #endregion

        public int NumFrames => TrimAdjust_GetNumFrames(Ptr);
        public int Width => TrimAdjust_GetWidth(Ptr);
        public int Height => TrimAdjust_GetHeight(Ptr);

        // フレームをJPEGバイト列として取得。失敗時はnull
        public byte[] GetFrameJpeg(int frameNumber)
        {
            int width = 0, height = 0;
            if (TrimAdjust_DecodeFrame(Ptr, frameNumber, ref width, ref height) != 0)
            {
                if (width != 0 && height != 0)
                {
                    if (TrimAdjust_GetFrameJpeg(Ptr, frameNumber, out var jpegData, out var jpegSize) != 0
                        && jpegSize > 0)
                    {
                        var buffer = new byte[jpegSize];
                        System.Runtime.InteropServices.Marshal.Copy(jpegData, buffer, 0, jpegSize);
                        return buffer;
                    }
                }
            }
            return null;
        }

        // フレームのメタ情報を取得
        public bool GetFrameInfo(int frameNumber, out long pts, out long duration, out int keyFrame, out int cmType)
        {
            pts = 0;
            duration = 0;
            keyFrame = 0;
            cmType = 0;
            return TrimAdjust_GetFrameInfo(Ptr, frameNumber, ref pts, ref duration, ref keyFrame, ref cmType) != 0;
        }

        // 波形画像をJPEGバイト列として取得。音声データなしの場合はnull
        public byte[] GetWaveformJpeg(int frameNumber)
        {
            if (TrimAdjust_GetWaveformJpeg(Ptr, frameNumber, out var jpegData, out var jpegSize) != 0
                && jpegSize > 0)
            {
                var buffer = new byte[jpegSize];
                System.Runtime.InteropServices.Marshal.Copy(jpegData, buffer, 0, jpegSize);
                return buffer;
            }
            return null;
        }
    }

    public delegate bool LogoAnalyzeCallback(float progress, int nread, int total, int ngather);
    public delegate bool LogoAutoDetectCallback(int stage, float stageProgress, float progress, int nread, int total);

    public class LogoFile : IDisposable
    {
        public AMTContext Ctx { private set; get; }
        public IntPtr Ptr { private set; get; }

        #region Natives
        [DllImport(AmatsukazeNatives.AmatsukazeLibName, CharSet = AmatsukazeNatives.AmatsukazeLibCharSet)]
        private static extern IntPtr LogoFile_Create(IntPtr ctx, string filepath);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern void LogoFile_Delete(IntPtr ptr);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern int LogoFile_GetWidth(IntPtr ptr);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern int LogoFile_GetHeight(IntPtr ptr);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern int LogoFile_GetX(IntPtr ptr);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern int LogoFile_GetY(IntPtr ptr);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern int LogoFile_GetImgWidth(IntPtr ptr);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern int LogoFile_GetImgHeight(IntPtr ptr);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern int LogoFile_GetServiceId(IntPtr ptr);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern void LogoFile_SetServiceId(IntPtr ptr, int serviceId);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern IntPtr LogoFile_GetName(IntPtr ptr);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern void LogoFile_SetName(IntPtr ptr, IntPtr name);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static unsafe extern void LogoFile_GetImage(IntPtr ptr, byte* buf, int stride, byte bg);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName, CharSet = AmatsukazeNatives.AmatsukazeLibCharSet)]
        private static extern int LogoFile_Save(IntPtr ptr, string filename);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName, CharSet = AmatsukazeNatives.AmatsukazeLibCharSet)]
        private static extern int LogoFile_ConvertAviUtlToExtended(IntPtr ctx, string srcpath, string dstpath, int serviceId, int imgw, int imgh);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName, CharSet = AmatsukazeNatives.AmatsukazeLibCharSet)]
        private static extern int ScanLogo(IntPtr ctx, string srcpath, int serviceid, string workfile, string dstpath,
            int imgx, int imgy, int w, int h, int thy, int numMaxFrames, LogoAnalyzeCallback cb);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName, CharSet = AmatsukazeNatives.AmatsukazeLibCharSet)]
        private static extern int AutoDetectLogoRect(IntPtr ctx, string srcpath, int serviceid,
            int divx, int divy, int searchFrames, int blockSize, int threshold,
            int marginX, int marginY, int threadN,
            ref int x, ref int y, ref int w, ref int h, ref int rectDetectFail, ref int logoAnalyzeFail,
            ref double pass1ScoreMax, ref double pass2ScoreMax, ref double finalScoreBeforeRescueMax,
            ref int pass2Entered, ref int pass2PrepareSucceeded, ref int pass2CollectSucceeded, ref int pass2RescueFallbackApplied,
            ref int pass2FailBeforeClear, ref int pass2FrameMaskNonZero, ref int pass2AcceptedFrames, ref int pass2SkippedFrames,
            ref int frameGateRetryAttemptCount, ref int frameGateRetrySuccessAttempt,
            string scorePath, string binaryPath, string cclPath, string countPath, string aPath, string bPath,
            string alphaPath, string logoYPath, string consistencyPath, string fgVarPath, string bgVarPath,
            string transitionPath, string keepRatePath,
            string acceptedPath,
            int detailedDebug,
            LogoAutoDetectCallback cb);
        #endregion

        public LogoFile(AMTContext ctx, string filepath)
        {
            Ctx = ctx;
            Ptr = LogoFile_Create(Ctx.Ptr, filepath);
            if (Ptr == IntPtr.Zero)
            {
                throw new IOException(Ctx.GetError());
            }
        }

        #region IDisposable Support
        private bool disposedValue = false; // 重複する呼び出しを検出するには

        protected virtual void Dispose(bool disposing)
        {
            if (!disposedValue)
            {
                LogoFile_Delete(Ptr);
                Ptr = IntPtr.Zero;
                disposedValue = true;
            }
        }

        ~LogoFile()
        {
            // このコードを変更しないでください。クリーンアップ コードを上の Dispose(bool disposing) に記述します。
            Dispose(false);
        }

        // このコードは、破棄可能なパターンを正しく実装できるように追加されました。
        public void Dispose()
        {
            // このコードを変更しないでください。クリーンアップ コードを上の Dispose(bool disposing) に記述します。
            Dispose(true);
            GC.SuppressFinalize(this);
        }
        #endregion

        public int Width { get { return LogoFile_GetWidth(Ptr); } }

        public int Height { get { return LogoFile_GetHeight(Ptr); } }

        public int ImageX { get { return LogoFile_GetX(Ptr); } }

        public int ImageY { get { return LogoFile_GetY(Ptr); } }

        public int ImageWidth { get { return LogoFile_GetImgWidth(Ptr); } }

        public int ImageHeight { get { return LogoFile_GetImgHeight(Ptr); } }

        public int ServiceId {
            get {
                return LogoFile_GetServiceId(Ptr);
            }
            set {
                LogoFile_SetServiceId(Ptr, value);
            }
        }

        private static string ConvertFromCP932(IntPtr ptr) {
            if (ptr == IntPtr.Zero) return string.Empty;
            
            // 文字列の長さを取得
            int length = 0;
            while (Marshal.ReadByte(ptr, length) != 0) length++;
            
            // バイト配列にコピー
            byte[] bytes = new byte[length];
            Marshal.Copy(ptr, bytes, 0, length);
            
            // ロゴフォーマットはCP932固定
            return global::Amatsukaze.Server.Util.LogoEncoding.GetString(bytes);
        }

        private static IntPtr ConvertToCP932(string str) {
            if (string.IsNullOrEmpty(str)) return IntPtr.Zero;
            
            // ロゴフォーマットはCP932固定
            byte[] bytes = global::Amatsukaze.Server.Util.LogoEncoding.GetBytes(str);
            
            // 終端のnullを含むメモリを確保
            IntPtr ptr = Marshal.AllocHGlobal(bytes.Length + 1);
            Marshal.Copy(bytes, 0, ptr, bytes.Length);
            Marshal.WriteByte(ptr, bytes.Length, 0); // null終端
            
            return ptr;
        }

        public string Name {
            get {
                return ConvertFromCP932(LogoFile_GetName(Ptr));
            }
            set {
                IntPtr ptr = ConvertToCP932(value);
                try {
                    LogoFile_SetName(Ptr, ptr);
                } finally {
                    if (ptr != IntPtr.Zero) {
                        Marshal.FreeHGlobal(ptr);
                    }
                }
            }
        }

        public object GetImage(byte bg)
        {
            int stride = Width * 3;
            byte[] buffer = new byte[stride * Height];
            unsafe
            {
                fixed (byte* pbuffer = buffer)
                {
                    LogoFile_GetImage(Ptr, pbuffer, stride, bg);
                }
            }
            return BitmapManager.CreateBitmapFromRgb(buffer, Width, Height, stride);
        }

        public void Save(string filepath)
        {
            if(LogoFile_Save(Ptr, filepath) == 0)
            {
                throw new IOException(Ctx.GetError());
            }
        }

        public static void ConvertAviUtlToExtended(AMTContext ctx, string srcpath, string dstpath, int serviceId, int imgw, int imgh)
        {
            if (LogoFile_ConvertAviUtlToExtended(ctx.Ptr, srcpath, dstpath, serviceId, imgw, imgh) == 0)
            {
                throw new IOException(ctx.GetError());
            }
        }

        public static void ScanLogo(AMTContext ctx, string srcpath, int serviceid, string workfile, string dstpath,
            int imgx, int imgy, int w, int h, int thy, int numMaxFrames, LogoAnalyzeCallback cb)
        {
            if(ScanLogo(ctx.Ptr, srcpath, serviceid, workfile, dstpath, imgx, imgy, w, h, thy, numMaxFrames, cb) == 0)
            {
                throw new IOException(ctx.GetError());
            }
        }

        public static AutoDetectLogoRectResult AutoDetectLogoRect(AMTContext ctx, string srcpath, int serviceid,
            int divx, int divy, int searchFrames, int blockSize, int threshold,
            int marginX, int marginY, int threadN,
            string scorePath, string binaryPath, string cclPath, string countPath, string aPath, string bPath,
            string alphaPath, string logoYPath, string consistencyPath, string fgVarPath, string bgVarPath,
            string transitionPath, string keepRatePath,
            string acceptedPath,
            bool detailedDebug,
            LogoAutoDetectCallback cb)
        {
            int x = 0;
            int y = 0;
            int w = 0;
            int h = 0;
            int rectDetectFail = 0;
            int logoAnalyzeFail = 0;
            double pass1ScoreMax = 0.0;
            double pass2ScoreMax = 0.0;
            double finalScoreBeforeRescueMax = 0.0;
            int pass2Entered = 0;
            int pass2PrepareSucceeded = 0;
            int pass2CollectSucceeded = 0;
            int pass2RescueFallbackApplied = 0;
            int pass2FailBeforeClear = 0;
            int pass2FrameMaskNonZero = 0;
            int pass2AcceptedFrames = 0;
            int pass2SkippedFrames = 0;
            int frameGateRetryAttemptCount = 0;
            int frameGateRetrySuccessAttempt = 0;
            if (AutoDetectLogoRect(ctx.Ptr, srcpath, serviceid,
                divx, divy, searchFrames, blockSize, threshold, marginX, marginY, threadN,
                ref x, ref y, ref w, ref h, ref rectDetectFail, ref logoAnalyzeFail,
                ref pass1ScoreMax, ref pass2ScoreMax, ref finalScoreBeforeRescueMax,
                ref pass2Entered, ref pass2PrepareSucceeded, ref pass2CollectSucceeded, ref pass2RescueFallbackApplied,
                ref pass2FailBeforeClear, ref pass2FrameMaskNonZero, ref pass2AcceptedFrames, ref pass2SkippedFrames,
                ref frameGateRetryAttemptCount, ref frameGateRetrySuccessAttempt,
                scorePath, binaryPath, cclPath, countPath, aPath, bPath, alphaPath, logoYPath, consistencyPath, fgVarPath, bgVarPath, transitionPath, keepRatePath, acceptedPath, detailedDebug ? 1 : 0, cb) == 0)
            {
                throw new AutoDetectLogoRectException(
                    ctx.GetError(),
                    (LogoRectDetectFail)rectDetectFail,
                    (LogoAnalyzeFail)logoAnalyzeFail,
                    pass1ScoreMax,
                    pass2ScoreMax,
                    finalScoreBeforeRescueMax,
                    pass2Entered != 0,
                    pass2PrepareSucceeded != 0,
                    pass2CollectSucceeded != 0,
                    pass2RescueFallbackApplied != 0,
                    (LogoAnalyzeFail)pass2FailBeforeClear,
                    pass2FrameMaskNonZero,
                    pass2AcceptedFrames,
                    pass2SkippedFrames,
                    frameGateRetryAttemptCount,
                    frameGateRetrySuccessAttempt);
            }
            return new AutoDetectLogoRectResult()
            {
                X = x,
                Y = y,
                W = w,
                H = h,
                RectDetectFail = (LogoRectDetectFail)rectDetectFail,
                LogoAnalyzeFail = (LogoAnalyzeFail)logoAnalyzeFail,
                Pass1ScoreMax = pass1ScoreMax,
                Pass2ScoreMax = pass2ScoreMax,
                FinalScoreBeforeRescueMax = finalScoreBeforeRescueMax,
                Pass2Entered = pass2Entered != 0,
                Pass2PrepareSucceeded = pass2PrepareSucceeded != 0,
                Pass2CollectSucceeded = pass2CollectSucceeded != 0,
                Pass2RescueFallbackApplied = pass2RescueFallbackApplied != 0,
                Pass2FailBeforeClear = (LogoAnalyzeFail)pass2FailBeforeClear,
                Pass2FrameMaskNonZero = pass2FrameMaskNonZero,
                Pass2AcceptedFrames = pass2AcceptedFrames,
                Pass2SkippedFrames = pass2SkippedFrames,
                FrameGateRetryAttemptCount = frameGateRetryAttemptCount,
                FrameGateRetrySuccessAttempt = frameGateRetrySuccessAttempt,
            };
        }
    }

    public delegate bool TsSlimCallback();

    public class TsSlimFilter : IDisposable
    {
        public AMTContext Ctx { private set; get; }
        public IntPtr Ptr { private set; get; }

        #region Natives
        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern IntPtr TsSlimFilter_Create(IntPtr ctx, int videoPid);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern void TsSlimFilter_Delete(IntPtr ptr);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName, CharSet = AmatsukazeNatives.AmatsukazeLibCharSet)]
        private static extern bool TsSlimFilter_Exec(IntPtr ptr, string srcpath, string dstpath, TsSlimCallback cb);
        #endregion

        public TsSlimFilter(AMTContext ctx, int videoPid)
        {
            Ctx = ctx;
            Ptr = TsSlimFilter_Create(Ctx.Ptr, videoPid);
            if (Ptr == IntPtr.Zero)
            {
                throw new IOException(Ctx.GetError());
            }
        }

        #region IDisposable Support
        private bool disposedValue = false; // 重複する呼び出しを検出するには

        protected virtual void Dispose(bool disposing)
        {
            if (!disposedValue)
            {
                TsSlimFilter_Delete(Ptr);
                Ptr = IntPtr.Zero;
                disposedValue = true;
            }
        }

        ~TsSlimFilter()
        {
            // このコードを変更しないでください。クリーンアップ コードを上の Dispose(bool disposing) に記述します。
            Dispose(false);
        }

        // このコードは、破棄可能なパターンを正しく実装できるように追加されました。
        public void Dispose()
        {
            // このコードを変更しないでください。クリーンアップ コードを上の Dispose(bool disposing) に記述します。
            Dispose(true);
            GC.SuppressFinalize(this);
        }
        #endregion

        public void Exec(string srcpath, string dstpath, TsSlimCallback cb)
        {
            if(!TsSlimFilter_Exec(Ptr, srcpath, dstpath, cb))
            {
                throw new IOException(Ctx.GetError());
            }
        }
    }

    public struct ProcessGroup
    {
        public int Group;
        public ulong Mask;
    }

    public enum ProcessGroupKind
    {
        None, Core, L2, L3, NUMA, Group, Count
    }

    public class CPUInfo : IDisposable
    {
        public AMTContext Ctx { private set; get; }
        public IntPtr Ptr { private set; get; }

        [StructLayout(LayoutKind.Sequential)]
        private struct GROUP_AFFINITY
        {
            public UIntPtr Mask;
            public ushort Group;
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 3)]
            public ushort[] Reserved;
        }

        #region Natives
        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern IntPtr CPUInfo_Create();

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern void CPUInfo_Delete(IntPtr ptr);

        [DllImport(AmatsukazeNatives.AmatsukazeLibName)]
        private static extern IntPtr CPUInfo_GetData(IntPtr ptr, int tag, out int count);
        #endregion

        public CPUInfo(AMTContext ctx)
        {
            Ctx = ctx;
            Ptr = CPUInfo_Create();
            if (Ptr == IntPtr.Zero)
            {
                throw new IOException(Ctx.GetError());
            }
        }

        #region IDisposable Support
        private bool disposedValue = false; // 重複する呼び出しを検出するには

        protected virtual void Dispose(bool disposing)
        {
            if (!disposedValue)
            {
                CPUInfo_Delete(Ptr);
                Ptr = IntPtr.Zero;
                disposedValue = true;
            }
        }

        ~CPUInfo()
        {
            // このコードを変更しないでください。クリーンアップ コードを上の Dispose(bool disposing) に記述します。
            Dispose(false);
        }

        // このコードは、破棄可能なパターンを正しく実装できるように追加されました。
        public void Dispose()
        {
            // このコードを変更しないでください。クリーンアップ コードを上の Dispose(bool disposing) に記述します。
            Dispose(true);
            GC.SuppressFinalize(this);
        }
        #endregion

        public ProcessGroup[] Get(ProcessGroupKind kind)
        {
            int count;
            var ptr = CPUInfo_GetData(Ptr, (int)kind, out count);
            var ret = new ProcessGroup[count];
            int size = Marshal.SizeOf(typeof(GROUP_AFFINITY));
            for(int i = 0; i < count; i++)
            {
                var item = (GROUP_AFFINITY)Marshal.PtrToStructure(ptr, typeof(GROUP_AFFINITY));
                ret[i].Group = item.Group;
                ret[i].Mask = item.Mask.ToUInt64();
                ptr += size;
            }
            return ret;
        }
    }
}
