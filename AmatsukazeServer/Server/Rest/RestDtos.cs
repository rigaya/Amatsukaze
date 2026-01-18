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
        public StatusSummary StatusSummary { get; set; }
    }

    public class QueueItemView
    {
        public int Id { get; set; }
        public string FileName { get; set; }
        public string ServiceName { get; set; }
        public string ProfileName { get; set; }
        public string State { get; set; }
        public string StateLabel { get; set; }
        public int Priority { get; set; }
        public bool IsBatch { get; set; }
        public DateTime? EncodeStart { get; set; }
        public DateTime? EncodeFinish { get; set; }
        public string DisplayEncodeStart { get; set; }
        public string DisplayEncodeFinish { get; set; }
        public double Progress { get; set; }
        public int ConsoleId { get; set; }
        public int OutputMask { get; set; }
        public bool IsTooSmall { get; set; }
    }

    public class QueueCounters
    {
        public int Active { get; set; }
        public int Encoding { get; set; }
        public int Complete { get; set; }
        public int Pending { get; set; }
        public int Failed { get; set; }
        public int Canceled { get; set; }
    }

    public class QueueFilter
    {
        public List<string> States { get; set; } = new List<string>();
        public string Search { get; set; }
        public List<string> SearchTargets { get; set; } = new List<string>();
        public DateTime? DateFrom { get; set; }
        public DateTime? DateTo { get; set; }
        public bool HideOneSeg { get; set; }
    }

    public class QueueView
    {
        public List<QueueItemView> Items { get; set; } = new List<QueueItemView>();
        public QueueCounters Counters { get; set; } = new QueueCounters();
        public QueueFilter Filters { get; set; } = new QueueFilter();
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

    public class LogoAnalyzeRequest
    {
        public string FilePath { get; set; }
        public int ServiceId { get; set; }
        public string WorkPath { get; set; }
        public int X { get; set; }
        public int Y { get; set; }
        public int Width { get; set; }
        public int Height { get; set; }
        public int Threshold { get; set; }
        public int MaxFrames { get; set; }
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
        public string LogoFileName { get; set; }
        public string ImageUrl { get; set; }
    }

    public class Snapshot
    {
        public SystemSnapshot System { get; set; }
        public QueueView QueueView { get; set; }
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
