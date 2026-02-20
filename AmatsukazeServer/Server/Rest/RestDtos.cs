using System;
using System.Collections.Generic;

namespace Amatsukaze.Server.Rest
{
    public class StatusSummary
    {
        public string RunningStateLabel { get; set; }
        public bool IsError { get; set; }
        public string LastOperationMessage { get; set; }
    }

    public class SystemSnapshot
    {
        public ServerInfo ServerInfo { get; set; }
        public State State { get; set; }
        public FinishSetting FinishSetting { get; set; }
        public List<FinishActionOptionView> FinishActionOptions { get; set; }
        public StatusSummary StatusSummary { get; set; }
        public List<DiskUsageView> Disks { get; set; }
    }

    public class DiskUsageView
    {
        public string Path { get; set; }
        public long CapacityBytes { get; set; }
        public long FreeBytes { get; set; }
        public long UsedBytes { get; set; }
        public double UsedRatio { get; set; }
    }

    public class FinishActionOptionView
    {
        public string Value { get; set; }
        public string Label { get; set; }
    }

    public class LogFileMeta
    {
        public long Size { get; set; }
        public bool TooLarge { get; set; }
        public string Encoding { get; set; }
    }

    public class LogFileContent
    {
        public string Content { get; set; }
        public LogFileMeta Meta { get; set; }
    }

    public class ConsoleState
    {
        public int Id { get; set; }
        public List<string> Lines { get; set; } = new List<string>();
        public ResourcePhase Phase { get; set; }
        public Resource Resource { get; set; }
    }

    public class ConsoleView
    {
        public List<ConsoleState> Consoles { get; set; } = new List<ConsoleState>();
        public List<string> AddQueueConsole { get; set; } = new List<string>();
    }

    public class LogoView
    {
        public int LogoId { get; set; }
        public string FileName { get; set; }
        public string ImageUrl { get; set; }
        public bool Enabled { get; set; }
        public string LogoName { get; set; }
        public DateTime From { get; set; }
        public DateTime To { get; set; }
        public bool Exists { get; set; }
        public int ImageWidth { get; set; }
        public int ImageHeight { get; set; }
    }

    public class ServiceView
    {
        public int ServiceId { get; set; }
        public string Name { get; set; }
        public List<LogoView> LogoList { get; set; } = new List<LogoView>();
    }

    public class DrcsView
    {
        public string Md5 { get; set; }
        public string MapStr { get; set; }
        public string ImageUrl { get; set; }
    }

    public class MakeScriptPreview
    {
        public string CommandLine { get; set; }
    }

    public class LogoAnalyzeStartRequest
    {
        public int QueueItemId { get; set; }
        public int X { get; set; }
        public int Y { get; set; }
        public int Width { get; set; }
        public int Height { get; set; }
        public int Threshold { get; set; }
        public int MaxFrames { get; set; }
    }

    public class LogoPreviewSessionRequest
    {
        public int QueueItemId { get; set; }
        public int ServiceId { get; set; }
    }

    public class LogoPreviewSessionResponse
    {
        public string SessionId { get; set; }
    }

    public class LogoAnalyzeStatus
    {
        public string JobId { get; set; }
        public bool Completed { get; set; }
        public string Error { get; set; }
        public float Progress { get; set; }
        public int NumRead { get; set; }
        public int NumTotal { get; set; }
        public int NumValid { get; set; }
        public int Pass { get; set; }
        public string LogoFileName { get; set; }
        public string ImageUrl { get; set; }
    }

    public class LogoAutoDetectStartRequest
    {
        public int QueueItemId { get; set; }
        public int DivX { get; set; } = 4;
        public int DivY { get; set; } = 4;
        public int SearchFrames { get; set; } = 20000;
        public int BlockSize { get; set; } = 32;
        public int Threshold { get; set; } = 12;
        public int MarginX { get; set; } = 4;
        public int MarginY { get; set; } = 4;
        public int ThreadN { get; set; } = 1;
        public bool DetailedDebug { get; set; } = false;
    }

    public class LogoRect
    {
        public int X { get; set; }
        public int Y { get; set; }
        public int Width { get; set; }
        public int Height { get; set; }
    }

    public class LogoAutoDetectDebugImages
    {
        public string ScoreUrl { get; set; }
        public string BinaryUrl { get; set; }
        public string CclUrl { get; set; }
        public string CountUrl { get; set; }
        public string AUrl { get; set; }
        public string BUrl { get; set; }
        public string AlphaUrl { get; set; }
        public string LogoYUrl { get; set; }
        public string ConsistencyUrl { get; set; }
        public string FgVarUrl { get; set; }
        public string BgVarUrl { get; set; }
        public string TransitionUrl { get; set; }
        public string KeepRateUrl { get; set; }
        public string AcceptedUrl { get; set; }
    }

    public class LogoAutoDetectStatus
    {
        public string JobId { get; set; }
        public bool Completed { get; set; }
        public string Error { get; set; }
        public float Progress { get; set; }
        public int Stage { get; set; }
        public string StageName { get; set; }
        public float StageProgress { get; set; }
        public int NumRead { get; set; }
        public int NumTotal { get; set; }
        public LogoRect DetectedRect { get; set; }
        public LogoAutoDetectDebugImages DebugImages { get; set; }
    }

    public class Snapshot
    {
        public SystemSnapshot System { get; set; }
        public Amatsukaze.Shared.QueueView QueueView { get; set; }
        public List<LogItem> EncodeLogs { get; set; }
        public List<CheckLogItem> CheckLogs { get; set; }
        public ConsoleView ConsoleView { get; set; }
        public List<ProfileSetting> Profiles { get; set; }
        public List<AutoSelectProfile> AutoSelects { get; set; }
        public List<ServiceView> Services { get; set; }
        public Setting Setting { get; set; }
        public MakeScriptData MakeScriptData { get; set; }
    }
}
