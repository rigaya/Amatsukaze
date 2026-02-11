using System;
using System.Collections.Generic;
using System.Text.Json;

namespace Amatsukaze.Shared
{
    public class StatusSummary
    {
        public string? RunningStateLabel { get; set; }
        public bool IsError { get; set; }
        public string? LastOperationMessage { get; set; }
    }

    public class ServerInfo
    {
        public string? HostName { get; set; }
        public string? Version { get; set; }
        public string? Platform { get; set; }
        public int CharSet { get; set; }
        public int LogicalProcessorCount { get; set; }
        public string? MacAddress { get; set; }
    }

    public class State
    {
        public bool Pause { get; set; }
        public bool Suspend { get; set; }
        public bool[]? EncoderSuspended { get; set; }
        public bool Running { get; set; }
        public bool ScheduledPause { get; set; }
        public bool ScheduledSuspend { get; set; }
        public double Progress { get; set; }
    }

    public class FinishSetting
    {
        public string? Action { get; set; }
        public int Seconds { get; set; }
        public bool NoActionExe { get; set; }
        public List<string>? NoActionExeList { get; set; }
    }

    public class FinishActionOptionView
    {
        public string? Value { get; set; }
        public string? Label { get; set; }
    }

    public class SystemSnapshot
    {
        public ServerInfo? ServerInfo { get; set; }
        public State? State { get; set; }
        public FinishSetting? FinishSetting { get; set; }
        public List<FinishActionOptionView>? FinishActionOptions { get; set; }
        public StatusSummary? StatusSummary { get; set; }
        public List<DiskUsageView>? Disks { get; set; }
    }

    public class ServerEnvironmentView
    {
        public bool IsServerLinux { get; set; }
    }

    public class InfoSummaryView
    {
        public string? HostName { get; set; }
        public string? Version { get; set; }
        public string? Platform { get; set; }
    }

    public class DiskUsageView
    {
        public string? Path { get; set; }
        public long CapacityBytes { get; set; }
        public long FreeBytes { get; set; }
        public long UsedBytes { get; set; }
        public double UsedRatio { get; set; }
    }

    public class UiStateView
    {
        public string? LastUsedProfile { get; set; }
        public string? LastOutputPath { get; set; }
        public string? LastAddQueueBat { get; set; }
        public List<string>? OutputPathHistory { get; set; }
    }

    public class OutputOptionItem
    {
        public string? Name { get; set; }
        public int Mask { get; set; }
    }

    public class FilterOptionItem
    {
        public int Id { get; set; }
        public string? Name { get; set; }
    }

    public class ProfileOptions
    {
        public List<string>? EncoderList { get; set; }
        public List<int>? EncoderParallelList { get; set; }
        public List<string>? SvtAv1BitDepthList { get; set; }
        public List<string>? DeinterlaceAlgorithmList { get; set; }
        public List<string>? DeblockStrengthList { get; set; }
        public List<string>? DeblockQualityList { get; set; }
        public List<int>? DeblockQualityValues { get; set; }
        public List<string>? D3dvpGpuList { get; set; }
        public List<string>? QtgmcPresetList { get; set; }
        public List<string>? FilterFpsList { get; set; }
        public List<string>? VfrFpsList { get; set; }
        public List<string>? JlsCommandFiles { get; set; }
        public List<string>? Mpeg2DecoderList { get; set; }
        public List<string>? H264DecoderList { get; set; }
        public List<string>? HevcDecoderList { get; set; }
        public List<string>? FormatList { get; set; }
        public List<OutputOptionItem>? OutputOptionList { get; set; }
        public List<int>? TsreplaceOutputMasks { get; set; }
        public List<string>? PreBatFiles { get; set; }
        public List<string>? PreEncodeBatFiles { get; set; }
        public List<string>? PostBatFiles { get; set; }
        public List<FilterOptionItem>? FilterOptions { get; set; }
        public List<string>? MainScriptFiles { get; set; }
        public List<string>? PostScriptFiles { get; set; }
        public List<string>? SubtitleModeList { get; set; }
        public List<string>? WhisperModelList { get; set; }
        public List<string>? AudioEncoderList { get; set; }
        public bool IsServerLinux { get; set; }
    }

    public class AutoSelectOptionsView
    {
        public List<string>? ProfileNames { get; set; }
        public List<int>? PriorityList { get; set; }
        public List<GenreNodeView>? Genres { get; set; }
        public List<ServiceOptionView>? Services { get; set; }
        public List<VideoSizeOptionView>? VideoSizes { get; set; }
    }

    public class GenreNodeView
    {
        public string? Id { get; set; }
        public int Space { get; set; }
        public int Level1 { get; set; }
        public int Level2 { get; set; }
        public string? Name { get; set; }
        public List<GenreNodeView>? Children { get; set; }
    }

    public class ServiceOptionView
    {
        public int ServiceId { get; set; }
        public string? Name { get; set; }
    }

    public class VideoSizeOptionView
    {
        public string? Name { get; set; }
        public string? Value { get; set; }
    }

    public class QueueItemView
    {
        public int Id { get; set; }
        public ProcMode Mode { get; set; }
        public string? SrcPath { get; set; }
        public string? DirName { get; set; }
        public string? FileName { get; set; }
        public string? ServiceName { get; set; }
        public string? ProfileName { get; set; }
        public string? State { get; set; }
        public string? StateLabel { get; set; }
        public int Priority { get; set; }
        public bool IsBatch { get; set; }
        public DateTime? EncodeStart { get; set; }
        public DateTime? EncodeFinish { get; set; }
        public DateTime? TsTime { get; set; }
        public DateTime? EitStartTime { get; set; }
        public string? DisplayBroadcastTime { get; set; }
        public string? DisplayEncodeStart { get; set; }
        public string? DisplayEncodeFinish { get; set; }
        public double Progress { get; set; }
        public int ConsoleId { get; set; }
        public int OutputMask { get; set; }
        public bool IsTooSmall { get; set; }
        public List<string>? Tags { get; set; }
        public string? OutDir { get; set; }
        public int ImageWidth { get; set; }
        public int ImageHeight { get; set; }
        public List<GenreNodeView>? Genres { get; set; }
        public List<string>? GenreNames { get; set; }
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
        public string? Search { get; set; }
        public List<string> SearchTargets { get; set; } = new List<string>();
        public DateTime? DateFrom { get; set; }
        public DateTime? DateTo { get; set; }
        public bool HideOneSeg { get; set; }
    }

    public class QueueView
    {
        public long Version { get; set; }
        public string? Digest { get; set; }
        public List<QueueItemView> Items { get; set; } = new List<QueueItemView>();
        public QueueCounters Counters { get; set; } = new QueueCounters();
        public QueueFilter Filters { get; set; } = new QueueFilter();
    }

    public enum QueueChangeType
    {
        Add,
        Update,
        Remove,
        Move
    }

    public class QueueChange
    {
        public QueueChangeType Type { get; set; }
        public QueueItemView? Item { get; set; }
        public int? Id { get; set; }
        public int? Position { get; set; }
    }

    public class QueueChangesView
    {
        public long FromVersion { get; set; }
        public long ToVersion { get; set; }
        public bool FullSyncRequired { get; set; }
        public string? QueueViewDigest { get; set; }
        public List<QueueChange> Changes { get; set; } = new List<QueueChange>();
        public QueueCounters Counters { get; set; } = new QueueCounters();
    }

    public class MessageEventView
    {
        public long Id { get; set; }
        public DateTime Time { get; set; }
        public string Level { get; set; } = "info";
        public string Message { get; set; } = string.Empty;
        public string? Source { get; set; }
        public string? Page { get; set; }
        public string? Action { get; set; }
        public string? RequestId { get; set; }
        public int? QueueItemId { get; set; }
    }

    public class MessageChangesView
    {
        public long FromId { get; set; }
        public long ToId { get; set; }
        public bool FullSyncRequired { get; set; }
        public bool Truncated { get; set; }
        public List<MessageEventView> Items { get; set; } = new List<MessageEventView>();
    }

    public class PagedResult<T>
    {
        public int Total { get; set; }
        public List<T> Items { get; set; } = new List<T>();
    }

    public class LogItemView
    {
        public string? SrcPath { get; set; }
        public bool Success { get; set; }
        public List<string>? OutPath { get; set; }
        public DateTime EncodeStartDate { get; set; }
        public DateTime EncodeFinishDate { get; set; }
        public string? Reason { get; set; }
        public int Incident { get; set; }
        public string? ServiceName { get; set; }
        public int ServiceId { get; set; }
        public DateTime TsTime { get; set; }
        public string? Profile { get; set; }
        public List<string>? Tags { get; set; }
        public string? DisplayEncodeStart { get; set; }
        public string? DisplayEncodeFinish { get; set; }
        public string? DisplayResult { get; set; }
        public string? DisplayService { get; set; }
        public string? DisplayTsTime { get; set; }
        public string? DisplayLogo { get; set; }
    }

    public class CheckLogItemView
    {
        public string? Type { get; set; }
        public string? SrcPath { get; set; }
        public bool Success { get; set; }
        public DateTime CheckStartDate { get; set; }
        public DateTime CheckFinishDate { get; set; }
        public string? Profile { get; set; }
        public string? ServiceName { get; set; }
        public int ServiceId { get; set; }
        public DateTime TsTime { get; set; }
        public string? Reason { get; set; }
        public string? DisplayType { get; set; }
        public string? DisplayResult { get; set; }
        public string? DisplayEncodeStart { get; set; }
        public string? DisplayEncodeFinish { get; set; }
    }

    public class LogFileMeta
    {
        public long Size { get; set; }
        public bool TooLarge { get; set; }
        public string? Encoding { get; set; }
    }

    public class LogFileContent
    {
        public string? Content { get; set; }
        public LogFileMeta? Meta { get; set; }
    }

    public class ConsoleState
    {
        public int Id { get; set; }
        public List<string> Lines { get; set; } = new List<string>();
        public string? Phase { get; set; }
        public JsonElement? Resource { get; set; }
    }

    public class ConsoleView
    {
        public List<ConsoleState> Consoles { get; set; } = new List<ConsoleState>();
        public List<string> AddQueueConsole { get; set; } = new List<string>();
    }

    public class ConsoleTaskInfo
    {
        public int TaskId { get; set; }
        public string? FileName { get; set; }
        public string? ServiceName { get; set; }
        public string? ProfileName { get; set; }
        public string? OutDir { get; set; }
    }

    public class ConsoleTaskView
    {
        public ConsoleTaskInfo? Task { get; set; }
        public List<string> Lines { get; set; } = new List<string>();
        public long Version { get; set; }
    }

    public enum ConsoleChangeType
    {
        Add,
        Replace,
        Clear
    }

    public class ConsoleTaskChange
    {
        public ConsoleChangeType Type { get; set; }
        public string? Line { get; set; }
    }

    public class ConsoleTaskChangesView
    {
        public long FromVersion { get; set; }
        public long ToVersion { get; set; }
        public bool FullSyncRequired { get; set; }
        public List<ConsoleTaskChange> Changes { get; set; } = new List<ConsoleTaskChange>();
    }

    public class LogoView
    {
        public int LogoId { get; set; }
        public string? FileName { get; set; }
        public string? ImageUrl { get; set; }
        public bool Enabled { get; set; }
        public string? LogoName { get; set; }
        public DateTime? From { get; set; }
        public DateTime? To { get; set; }
        public bool Exists { get; set; }
        public int? ImageWidth { get; set; }
        public int? ImageHeight { get; set; }
    }

    public class ServiceView
    {
        public int ServiceId { get; set; }
        public string? Name { get; set; }
        public List<LogoView> LogoList { get; set; } = new List<LogoView>();
    }

    public class ServiceSettingView
    {
        public int ServiceId { get; set; }
        public string? ServiceName { get; set; }
        public bool DisableCMCheck { get; set; }
        public string? JlsCommand { get; set; }
        public string? JlsOption { get; set; }
        public List<LogoSettingView> Logos { get; set; } = new List<LogoSettingView>();
    }

    public class LogoSettingView
    {
        public string? FileName { get; set; }
        public string? LogoName { get; set; }
        public bool Enabled { get; set; }
        public DateTime From { get; set; }
        public DateTime To { get; set; }
        public bool Exists { get; set; }
        public string? ImageUrl { get; set; }
        public int? ImageWidth { get; set; }
        public int? ImageHeight { get; set; }
    }

    public class ServiceOptions
    {
        public List<string> JlsCommandFiles { get; set; } = new List<string>();
    }

    public class LogoPeriodUpdateRequest
    {
        public string? FileName { get; set; }
        public DateTime? From { get; set; }
        public DateTime? To { get; set; }
    }

    public class LogoEnabledUpdateRequest
    {
        public string? FileName { get; set; }
        public bool Enabled { get; set; }
    }

    public class LogoProbeResponse
    {
        public bool IsExtended { get; set; }
        public int? ServiceId { get; set; }
        public string? ServiceName { get; set; }
        public int? ImageWidth { get; set; }
        public int? ImageHeight { get; set; }
    }

    public class LogoFileNameRequest
    {
        public string? FileName { get; set; }
    }

    public class DrcsView
    {
        public string? Md5 { get; set; }
        public string? MapStr { get; set; }
        public string? ImageUrl { get; set; }
    }

    public class DrcsMapUpdateRequest
    {
        public string? Md5 { get; set; }
        public string? MapStr { get; set; }
    }

    public class DrcsAppearanceResponse
    {
        public string? Md5 { get; set; }
        public List<string> Items { get; set; } = new List<string>();
    }

    public class MakeScriptData
    {
        public string? Profile { get; set; }
        public string? OutDir { get; set; }
        public string? NasDir { get; set; }
        public bool IsNasEnabled { get; set; }
        public bool IsWakeOnLan { get; set; }
        public bool MoveAfter { get; set; }
        public bool ClearEncoded { get; set; }
        public bool WithRelated { get; set; }
        public bool IsDirect { get; set; }
        public int Priority { get; set; }
        public string? AddQueueBat { get; set; }
    }

    public class MakeScriptPreview
    {
        public string? CommandLine { get; set; }
    }

    public class MakeScriptGenerateRequest
    {
        public MakeScriptData? MakeScriptData { get; set; }
        public string? TargetHost { get; set; } // local / remote
        public string? ScriptType { get; set; } // bat / sh
        public string? RemoteHost { get; set; }
        public string? Subnet { get; set; }
        public string? Mac { get; set; }
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
        public string? SessionId { get; set; }
    }

    public class LogoAnalyzeStatus
    {
        public string? JobId { get; set; }
        public bool Completed { get; set; }
        public string? Error { get; set; }
        public float Progress { get; set; }
        public int NumRead { get; set; }
        public int NumTotal { get; set; }
        public int NumValid { get; set; }
        public int Pass { get; set; }
        public string? LogoFileName { get; set; }
        public string? ImageUrl { get; set; }
    }

    public class Snapshot
    {
        public SystemSnapshot? System { get; set; }
        public QueueView? QueueView { get; set; }
        public List<LogItemView>? EncodeLogs { get; set; }
        public List<CheckLogItemView>? CheckLogs { get; set; }
        public ConsoleView? ConsoleView { get; set; }
        public List<JsonElement>? Profiles { get; set; }
        public List<JsonElement>? AutoSelects { get; set; }
        public List<ServiceView>? Services { get; set; }
        public JsonElement? Setting { get; set; }
        public MakeScriptData? MakeScriptData { get; set; }
    }
}
