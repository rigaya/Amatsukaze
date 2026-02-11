using System;
using System.Collections.Generic;
using System.Linq;
using System.IO;
using System.Text;
using System.Threading.Tasks;
using System.Security.Cryptography;
using Amatsukaze.Lib;
using Amatsukaze.Shared;

namespace Amatsukaze.Server.Rest
{
    internal class TaskConsoleState : ConsoleTextBase
    {
        private readonly List<string> lines = new List<string>();
        private readonly List<ConsoleChangeRecord> changes = new List<ConsoleChangeRecord>();
        private long version;

        public ConsoleTaskInfo TaskInfo { get; private set; }
        public int TaskId { get; }
        public bool IsFallback { get; private set; }

        public TaskConsoleState(ConsoleTaskInfo info)
        {
            TaskInfo = info;
            TaskId = info.TaskId;
        }

        public IReadOnlyList<string> Lines
        {
            get { return lines; }
        }

        public long Version
        {
            get { return version; }
        }

        public void UpdateTaskInfo(ConsoleTaskInfo info)
        {
            TaskInfo = info;
        }

        public override void OnAddLine(string text)
        {
            AddLine(text);
            AddChange(ConsoleChangeType.Add, text);
        }

        public override void OnReplaceLine(string text)
        {
            var hadLine = lines.Count > 0;
            ReplaceLine(text);
            AddChange(hadLine ? ConsoleChangeType.Replace : ConsoleChangeType.Add, text);
        }

        public void LoadFallbackLines(List<string> newLines)
        {
            SetTextLines(newLines);
            IsFallback = true;
        }

        public void ResetForLive()
        {
            if (!IsFallback)
            {
                return;
            }
            IsFallback = false;
            lines.Clear();
            AddChange(ConsoleChangeType.Clear, string.Empty);
        }

        public void SetTextLines(List<string> newLines)
        {
            Clear();
            lines.Clear();
            changes.Clear();
            version = 0;
            if (newLines != null)
            {
                lines.AddRange(newLines);
                TrimLines();
            }
        }

        public ConsoleTaskChangesView GetChanges(long sinceVersion)
        {
            if (sinceVersion > version)
            {
                return new ConsoleTaskChangesView()
                {
                    FromVersion = sinceVersion,
                    ToVersion = version,
                    FullSyncRequired = true
                };
            }

            if (changes.Count == 0)
            {
                return new ConsoleTaskChangesView()
                {
                    FromVersion = sinceVersion,
                    ToVersion = version,
                    FullSyncRequired = sinceVersion != version
                };
            }

            var minVersion = changes[0].Version;
            if (sinceVersion < minVersion)
            {
                return new ConsoleTaskChangesView()
                {
                    FromVersion = sinceVersion,
                    ToVersion = version,
                    FullSyncRequired = true
                };
            }

            var items = changes
                .Where(change => change.Version > sinceVersion)
                .Select(change => change.Change)
                .ToList();

            return new ConsoleTaskChangesView()
            {
                FromVersion = sinceVersion,
                ToVersion = version,
                FullSyncRequired = false,
                Changes = items
            };
        }

        private void AddLine(string text)
        {
            lines.Add(text);
            TrimLines();
        }

        private void ReplaceLine(string text)
        {
            if (lines.Count == 0)
            {
                lines.Add(text);
                return;
            }
            lines[lines.Count - 1] = text;
        }

        private void TrimLines()
        {
            if (lines.Count <= ConsoleConstants.MaxConsoleLines)
            {
                return;
            }
            var trim = Math.Max(ConsoleConstants.ConsoleTrimLines, lines.Count - ConsoleConstants.MaxConsoleLines);
            trim = Math.Min(trim, lines.Count);
            lines.RemoveRange(0, trim);
        }

        private void AddChange(ConsoleChangeType type, string text)
        {
            version++;
            changes.Add(new ConsoleChangeRecord()
            {
                Version = version,
                Change = new ConsoleTaskChange()
                {
                    Type = type,
                    Line = text
                }
            });
            if (changes.Count > ConsoleConstants.MaxConsoleChanges)
            {
                var trim = Math.Max(ConsoleConstants.ConsoleTrimChanges, changes.Count - ConsoleConstants.MaxConsoleChanges);
                trim = Math.Min(trim, changes.Count);
                changes.RemoveRange(0, trim);
            }
        }

        private class ConsoleChangeRecord
        {
            public long Version { get; set; }
            public ConsoleTaskChange Change { get; set; }
        }
    }

    public class RestStateStore : IUserClient
    {
        private readonly object sync = new object();
        private readonly EncodeServer encodeServer;

        private State state;
        private FinishSetting finishSetting;
        private ServerInfo serverInfo;
        private OperationResult lastOperationResult;

        private List<QueueItem> queueItems = new List<QueueItem>();
        private long queueVersion = 0;
        private const int MaxQueueChanges = 1000;
        private readonly List<QueueChangeRecord> queueChanges = new List<QueueChangeRecord>();
        private const int MaxMessageLog = 500;
        private long messageVersion = 0;
        private readonly List<MessageRecord> messageLog = new List<MessageRecord>();
        private const int MaxPendingConsoleBytes = 256 * 1024;
        private List<LogItem> logItems = new List<LogItem>();
        private List<CheckLogItem> checkLogItems = new List<CheckLogItem>();

        private readonly Dictionary<int, TaskConsoleState> taskConsoles = new Dictionary<int, TaskConsoleState>();
        private readonly Dictionary<int, int> consoleIdToTaskId = new Dictionary<int, int>();
        private readonly Dictionary<int, int> taskIdToConsoleId = new Dictionary<int, int>();
        private readonly Dictionary<int, List<string>> pendingConsoleSnapshots = new Dictionary<int, List<string>>();
        private readonly Dictionary<int, List<byte>> pendingConsoleBytes = new Dictionary<int, List<byte>>();

        private Setting setting;
        private UIState uiState;
        private MakeScriptData makeScriptData;
        private List<string> addQueueBatFiles = new List<string>();
        private List<string> queueFinishBatFiles = new List<string>();
        private List<string> jlsCommandFiles = new List<string>();
        private List<string> mainScriptFiles = new List<string>();
        private List<string> postScriptFiles = new List<string>();
        private List<string> preBatFiles = new List<string>();
        private List<string> preEncodeBatFiles = new List<string>();
        private List<string> postBatFiles = new List<string>();
        private List<DiskItem> disks = new List<DiskItem>();
        private List<int> cpuClusters = new List<int>();

        private readonly Dictionary<string, ProfileSetting> profiles = new Dictionary<string, ProfileSetting>(StringComparer.OrdinalIgnoreCase);
        private readonly Dictionary<string, AutoSelectProfile> autoSelects = new Dictionary<string, AutoSelectProfile>(StringComparer.OrdinalIgnoreCase);
        private readonly Dictionary<int, ServiceSettingElement> services = new Dictionary<int, ServiceSettingElement>();
        private readonly Dictionary<string, DrcsImage> drcsMap = new Dictionary<string, DrcsImage>();

        private string lastLogFile;

        private class QueueChangeRecord
        {
            public long Version { get; set; }
            public QueueChange Change { get; set; }
        }

        private class MessageRecord
        {
            public long Id { get; set; }
            public MessageEventView Event { get; set; }
        }


        public RestStateStore(EncodeServer server)
        {
            encodeServer = server;
        }

        public async Task RequestInitialSync(EncodeServer server)
        {
            await server.Request(ServerRequest.Setting |
                                 ServerRequest.Queue |
                                 ServerRequest.Log |
                                 ServerRequest.CheckLog |
                                 ServerRequest.Console |
                                 ServerRequest.State |
                                 ServerRequest.FreeSpace |
                                 ServerRequest.ServiceSetting);
            await server.RequestDrcsImages();
        }

        private QueueItem CopyQueueItem(QueueItem item)
        {
            return ServerSupport.DeepCopy(item);
        }

        private LogItem CopyLogItem(LogItem item)
        {
            return ServerSupport.DeepCopy(item);
        }

        private CheckLogItem CopyCheckLogItem(CheckLogItem item)
        {
            return ServerSupport.DeepCopy(item);
        }

        private ProfileSetting CopyProfile(ProfileSetting profile)
        {
            return ServerSupport.DeepCopy(profile);
        }

        private AutoSelectProfile CopyAutoSelect(AutoSelectProfile profile)
        {
            return ServerSupport.DeepCopy(profile);
        }

        private ServiceSettingElement CopyService(ServiceSettingElement service)
        {
            return ServerSupport.DeepCopy(service);
        }

        public SystemSnapshot GetSystemSnapshot()
        {
            lock (sync)
            {
                ServerInfo compactServerInfo = null;
                if (serverInfo != null)
                {
                    compactServerInfo = new ServerInfo
                    {
                        LogicalProcessorCount = serverInfo.LogicalProcessorCount,
                        CharSet = serverInfo.CharSet,
                        MacAddress = serverInfo.MacAddress
                    };
                }
                return new SystemSnapshot()
                {
                    ServerInfo = compactServerInfo,
                    State = state != null ? ServerSupport.DeepCopy(state) : null,
                    FinishSetting = finishSetting != null ? ServerSupport.DeepCopy(finishSetting) : null,
                    FinishActionOptions = BuildFinishActionOptions(setting, serverInfo),
                    StatusSummary = BuildStatusSummary(),
                    Disks = null
                };
            }
        }

        public InfoSummaryView GetInfoSummary()
        {
            lock (sync)
            {
                return new InfoSummaryView
                {
                    HostName = serverInfo?.HostName,
                    Version = serverInfo?.Version,
                    Platform = serverInfo?.Platform
                };
            }
        }

        public List<DiskUsageView> GetInfoDisks()
        {
            return DiskUtility.GetMainDisks().Select(item =>
            {
                var used = Math.Max(0, item.CapacityBytes - item.FreeBytes);
                var ratio = item.CapacityBytes > 0 ? (double)used / item.CapacityBytes : 0.0;
                return new DiskUsageView
                {
                    Path = item.Path,
                    CapacityBytes = item.CapacityBytes,
                    FreeBytes = item.FreeBytes,
                    UsedBytes = used,
                    UsedRatio = ratio
                };
            }).ToList();
        }

        private static List<FinishActionOptionView> BuildFinishActionOptions(Setting currentSetting, ServerInfo currentServerInfo)
        {
            var isLinux = Util.IsServerLinux();
            var enableShutdown = currentSetting?.EnableShutdownAction ?? false;

            var options = new List<FinishActionOptionView>
            {
                new FinishActionOptionView { Value = "None", Label = "何もしない" }
            };

            if (isLinux)
            {
                return options;
            }

            options.Add(new FinishActionOptionView { Value = "Suspend", Label = "スリープ" });
            options.Add(new FinishActionOptionView { Value = "Hibernate", Label = "休止状態" });
            if (enableShutdown)
            {
                options.Add(new FinishActionOptionView { Value = "Shutdown", Label = "シャットダウン" });
            }
            return options;
        }

        public UiStateView GetUiStateView()
        {
            lock (sync)
            {
                if (uiState == null)
                {
                    return new UiStateView();
                }
                return new UiStateView
                {
                    LastUsedProfile = uiState.LastUsedProfile,
                    LastOutputPath = uiState.LastOutputPath,
                    LastAddQueueBat = uiState.LastAddQueueBat,
                    OutputPathHistory = uiState.OutputPathHistory != null
                        ? uiState.OutputPathHistory.ToList()
                        : new List<string>()
                };
            }
        }

        public Amatsukaze.Shared.QueueView GetQueueView(Amatsukaze.Shared.QueueFilter filter)
        {
            filter = filter ?? new Amatsukaze.Shared.QueueFilter();
            List<QueueItem> items;
            Setting currentSetting;
            long currentVersion;
            lock (sync)
            {
                items = queueItems.Where(item => item != null && string.IsNullOrEmpty(item.SrcPath) == false).ToList();
                currentSetting = setting;
                currentVersion = queueVersion;
            }

            IEnumerable<QueueItem> filtered = items;
            if (filter.HideOneSeg || (currentSetting?.HideOneSeg ?? false))
            {
                filtered = filtered.Where(item => IsTooSmall(item) == false);
            }

            if (filter.States != null && filter.States.Count > 0)
            {
                var set = new HashSet<string>(filter.States, StringComparer.OrdinalIgnoreCase);
                filtered = filtered.Where(item => set.Contains(item.State.ToString()));
            }

            if (!string.IsNullOrWhiteSpace(filter.Search))
            {
                var search = filter.Search;
                var targets = filter.SearchTargets ?? new List<string>();
                bool targetFile = targets.Count == 0 || targets.Any(t => t.Equals("file", StringComparison.OrdinalIgnoreCase));
                bool targetService = targets.Count == 0 || targets.Any(t => t.Equals("service", StringComparison.OrdinalIgnoreCase));
                bool targetProfile = targets.Count == 0 || targets.Any(t => t.Equals("profile", StringComparison.OrdinalIgnoreCase));

                filtered = filtered.Where(item =>
                    (targetFile && item.FileName?.IndexOf(search, StringComparison.OrdinalIgnoreCase) >= 0) ||
                    (targetService && item.ServiceName?.IndexOf(search, StringComparison.OrdinalIgnoreCase) >= 0) ||
                    (targetProfile && (item.Profile?.Name ?? item.ProfileName)?.IndexOf(search, StringComparison.OrdinalIgnoreCase) >= 0)
                );
            }

            if (filter.DateFrom.HasValue)
            {
                filtered = filtered.Where(item => item.EncodeStart >= filter.DateFrom.Value);
            }
            if (filter.DateTo.HasValue)
            {
                filtered = filtered.Where(item => item.EncodeFinish <= filter.DateTo.Value);
            }

            var list = filtered.Select(ToQueueItemView).ToList();
            var counters = BuildQueueCounters(items);
            var digest = BuildQueueDigest(counters, filtered);
            return new Amatsukaze.Shared.QueueView()
            {
                Version = currentVersion,
                Digest = digest,
                Items = list,
                Counters = counters,
                Filters = filter
            };
        }

        public Amatsukaze.Shared.QueueChangesView GetQueueChanges(long sinceVersion)
        {
            lock (sync)
            {
                var viewItems = queueItems.Where(item => item != null && string.IsNullOrEmpty(item.SrcPath) == false).ToList();
                var counters = BuildQueueCounters(viewItems);
                var digest = BuildQueueDigest(counters, viewItems);
                if (sinceVersion > queueVersion)
                {
                    return new Amatsukaze.Shared.QueueChangesView()
                    {
                        FromVersion = sinceVersion,
                        ToVersion = queueVersion,
                        FullSyncRequired = true,
                        QueueViewDigest = digest,
                        Counters = BuildQueueCounters(viewItems)
                    };
                }

                if (queueChanges.Count == 0)
                {
                    if (sinceVersion == queueVersion)
                    {
                        return new Amatsukaze.Shared.QueueChangesView()
                        {
                            FromVersion = sinceVersion,
                            ToVersion = queueVersion,
                            FullSyncRequired = false,
                            QueueViewDigest = digest,
                            Counters = BuildQueueCounters(viewItems)
                        };
                    }
                    return new Amatsukaze.Shared.QueueChangesView()
                    {
                        FromVersion = sinceVersion,
                        ToVersion = queueVersion,
                        FullSyncRequired = true,
                        QueueViewDigest = digest,
                        Counters = BuildQueueCounters(viewItems)
                    };
                }

                var minVersion = queueChanges[0].Version;
                if (sinceVersion < minVersion)
                {
                    return new Amatsukaze.Shared.QueueChangesView()
                    {
                        FromVersion = sinceVersion,
                        ToVersion = queueVersion,
                        FullSyncRequired = true,
                        QueueViewDigest = digest,
                        Counters = BuildQueueCounters(viewItems)
                    };
                }

                var changes = queueChanges
                    .Where(c => c.Version > sinceVersion)
                    .Select(c => c.Change)
                    .ToList();

                return new Amatsukaze.Shared.QueueChangesView()
                {
                    FromVersion = sinceVersion,
                    ToVersion = queueVersion,
                    FullSyncRequired = false,
                    QueueViewDigest = digest,
                    Changes = changes,
                    Counters = BuildQueueCounters(viewItems)
                };
            }
        }

        public Amatsukaze.Shared.MessageChangesView GetMessageChanges(long sinceId, string page, string requestId, HashSet<string> levels, int max)
        {
            lock (sync)
            {
                if (messageLog.Count == 0)
                {
                    return new Amatsukaze.Shared.MessageChangesView()
                    {
                        FromId = sinceId,
                        ToId = messageVersion
                    };
                }

                var minId = messageLog[0].Id;
                var fullSyncRequired = sinceId < minId;
                var items = messageLog
                    .Where(r => r.Id > sinceId)
                    .Select(r => r.Event)
                    .Where(e => FilterMessageEvent(e, page, requestId, levels))
                    .ToList();

                bool truncated = false;
                if (max > 0 && items.Count > max)
                {
                    truncated = true;
                    items = items.Skip(items.Count - max).ToList();
                }

                return new Amatsukaze.Shared.MessageChangesView()
                {
                    FromId = sinceId,
                    ToId = messageVersion,
                    FullSyncRequired = fullSyncRequired,
                    Truncated = truncated,
                    Items = items
                };
            }
        }

        public bool TryGetMessageForRequestId(string requestId, out Amatsukaze.Shared.MessageEventView message)
        {
            lock (sync)
            {
                if (string.IsNullOrEmpty(requestId) || messageLog.Count == 0)
                {
                    message = null;
                    return false;
                }

                for (int i = messageLog.Count - 1; i >= 0; i--)
                {
                    var evt = messageLog[i].Event;
                    if (evt != null && string.Equals(evt.RequestId, requestId, StringComparison.Ordinal))
                    {
                        message = evt;
                        return true;
                    }
                }
                message = null;
                return false;
            }
        }

        public List<LogItem> GetEncodeLogs()
        {
            lock (sync)
            {
                return logItems.Select(CopyLogItem).ToList();
            }
        }

        public List<CheckLogItem> GetCheckLogs()
        {
            lock (sync)
            {
                return checkLogItems.Select(CopyCheckLogItem).ToList();
            }
        }

        public bool TryGetConsoleTaskView(int taskId, out ConsoleTaskView view)
        {
            lock (sync)
            {
                if (!taskConsoles.TryGetValue(taskId, out var console))
                {
                    var item = queueItems.FirstOrDefault(q => q != null && q.Id == taskId);
                    if (item == null)
                    {
                        view = null;
                        return false;
                    }
                    if (!TryCreateFallbackConsole(item, out console))
                    {
                        view = null;
                        return false;
                    }
                    taskConsoles[taskId] = console;
                }
                else
                {
                    var item = queueItems.FirstOrDefault(q => q != null && q.Id == taskId);
                    if (item != null &&
                        item.State != QueueState.Encoding &&
                        !console.IsFallback &&
                        console.Lines.Count == 0)
                    {
                        if (TryCreateFallbackConsole(item, out var fallback))
                        {
                            taskConsoles[taskId] = fallback;
                            console = fallback;
                        }
                    }
                }
                view = new ConsoleTaskView()
                {
                    Task = CopyConsoleTaskInfo(console.TaskInfo),
                    Lines = console.Lines.ToList(),
                    Version = console.Version
                };
                return true;
            }
        }

        public bool TryGetConsoleTaskChanges(int taskId, long sinceVersion, out ConsoleTaskChangesView view)
        {
            lock (sync)
            {
                if (!taskConsoles.TryGetValue(taskId, out var console))
                {
                    view = null;
                    return false;
                }
                view = console.GetChanges(sinceVersion);
                return true;
            }
        }

        public List<ProfileSetting> GetProfiles()
        {
            lock (sync)
            {
                return profiles.Values.Select(CopyProfile).ToList();
            }
        }

        public List<string> GetJlsCommandFiles()
        {
            lock (sync)
            {
                return jlsCommandFiles.ToList();
            }
        }

        public List<string> GetMainScriptFiles()
        {
            lock (sync)
            {
                return mainScriptFiles.ToList();
            }
        }

        public List<string> GetPostScriptFiles()
        {
            lock (sync)
            {
                return postScriptFiles.ToList();
            }
        }

        public List<string> GetPreBatFiles()
        {
            lock (sync)
            {
                return preBatFiles.ToList();
            }
        }

        public List<string> GetPreEncodeBatFiles()
        {
            lock (sync)
            {
                return preEncodeBatFiles.ToList();
            }
        }

        public List<string> GetPostBatFiles()
        {
            lock (sync)
            {
                return postBatFiles.ToList();
            }
        }

        public List<AutoSelectProfile> GetAutoSelects()
        {
            lock (sync)
            {
                return autoSelects.Values.Select(CopyAutoSelect).ToList();
            }
        }

        public List<ServiceView> GetServiceViews()
        {
            lock (sync)
            {
                return services.Values.Select(service =>
                {
                    var view = new ServiceView()
                    {
                        ServiceId = service.ServiceId,
                        Name = service.ServiceName
                    };
                    if (service.LogoSettings != null)
                    {
                        for (int i = 0; i < service.LogoSettings.Count; i++)
                        {
                            var logo = service.LogoSettings[i];
                            var logoView = new LogoView()
                            {
                                LogoId = i,
                                FileName = logo.FileName,
                                ImageUrl = (logo.FileName == LogoSetting.NO_LOGO) ? null : $"/api/assets/logo/{service.ServiceId}/{i}",
                                Enabled = logo.Enabled,
                                LogoName = logo.LogoName,
                                From = logo.From,
                                To = logo.To,
                                Exists = logo.Exists
                            };
                            if (encodeServer != null && encodeServer.TryGetLogoImageSize(logo.FileName, out var w, out var h))
                            {
                                logoView.ImageWidth = w;
                                logoView.ImageHeight = h;
                            }
                            view.LogoList.Add(logoView);
                        }
                    }
                    return view;
                }).ToList();
            }
        }

        public List<ServiceSettingView> GetServiceSettingViews()
        {
            lock (sync)
            {
                return services.Values.Select(service =>
                {
                    var view = new ServiceSettingView()
                    {
                        ServiceId = service.ServiceId,
                        ServiceName = service.ServiceName,
                        DisableCMCheck = service.DisableCMCheck,
                        JlsCommand = service.JLSCommand,
                        JlsOption = service.JLSOption
                    };
                    if (service.LogoSettings != null)
                    {
                        for (int i = 0; i < service.LogoSettings.Count; i++)
                        {
                            var logo = service.LogoSettings[i];
                            var logoView = new LogoSettingView()
                            {
                                FileName = logo.FileName,
                                LogoName = logo.LogoName,
                                Enabled = logo.Enabled,
                                From = logo.From,
                                To = logo.To,
                                Exists = logo.Exists,
                                ImageUrl = (logo.FileName == LogoSetting.NO_LOGO) ? null : $"/api/assets/logo/{service.ServiceId}/{i}"
                            };
                            if (encodeServer != null && logo.FileName != LogoSetting.NO_LOGO &&
                                encodeServer.TryGetLogoImageSize(logo.FileName, out var w, out var h))
                            {
                                logoView.ImageWidth = w;
                                logoView.ImageHeight = h;
                            }
                            view.Logos.Add(logoView);
                        }
                    }
                    return view;
                }).ToList();
            }
        }

        public bool TryGetServiceSetting(int serviceId, out ServiceSettingElement service)
        {
            lock (sync)
            {
                if (services.TryGetValue(serviceId, out var existing))
                {
                    service = CopyService(existing);
                    return true;
                }
            }
            service = null;
            return false;
        }

        public List<DrcsView> GetDrcsViews()
        {
            lock (sync)
            {
                return drcsMap.Values.Select(item => new DrcsView()
                {
                    Md5 = item.MD5,
                    MapStr = item.MapStr,
                    ImageUrl = $"/api/assets/drcs/{item.MD5}"
                }).ToList();
            }
        }

        public bool TryGetDrcsAppearance(string md5, out List<string> items)
        {
            lock (sync)
            {
                if (string.IsNullOrWhiteSpace(md5))
                {
                    items = new List<string>();
                    return false;
                }
                if (drcsMap.TryGetValue(md5, out var image) && image?.SourceList != null)
                {
                    items = image.SourceList
                        .Where(s => s != null)
                        .Select(s => s.ToString())
                        .Where(s => string.IsNullOrWhiteSpace(s) == false)
                        .Distinct()
                        .ToList();
                    return true;
                }
                items = new List<string>();
                return false;
            }
        }

        public Setting GetSetting()
        {
            lock (sync)
            {
                return setting != null ? ServerSupport.DeepCopy(setting) : null;
            }
        }

        public MakeScriptData GetMakeScriptData()
        {
            lock (sync)
            {
                return makeScriptData != null ? ServerSupport.DeepCopy(makeScriptData) : null;
            }
        }

        public List<string> GetAddQueueBatFiles()
        {
            lock (sync)
            {
                return addQueueBatFiles.ToList();
            }
        }

        public List<string> GetQueueFinishBatFiles()
        {
            lock (sync)
            {
                return queueFinishBatFiles.ToList();
            }
        }

        public Snapshot GetSnapshot()
        {
            return new Snapshot()
            {
                System = GetSystemSnapshot(),
                QueueView = GetQueueView(new Amatsukaze.Shared.QueueFilter()),
                EncodeLogs = GetEncodeLogs(),
                CheckLogs = GetCheckLogs(),
                ConsoleView = null,
                Profiles = GetProfiles(),
                AutoSelects = GetAutoSelects(),
                Services = GetServiceViews(),
                Setting = GetSetting(),
                MakeScriptData = GetMakeScriptData()
            };
        }

        public string GetLastLogFile()
        {
            lock (sync)
            {
                return lastLogFile;
            }
        }

        public OperationResult GetLastOperationResult()
        {
            lock (sync)
            {
                return lastOperationResult != null ? ServerSupport.DeepCopy(lastOperationResult) : null;
            }
        }

        public bool TryGetQueueItem(int id, out QueueItem item)
        {
            lock (sync)
            {
                var found = queueItems.FirstOrDefault(q => q != null && q.Id == id);
                if (found == null)
                {
                    item = null;
                    return false;
                }
                item = CopyQueueItem(found);
                return true;
            }
        }

        private StatusSummary BuildStatusSummary()
        {
            string runningState = "停止";
            if (state != null && state.Running)
            {
                if (state.Suspend || state.ScheduledSuspend)
                {
                    runningState = "一時停止中";
                }
                else
                {
                    runningState = "エンコード中";
                }
            }
            return new StatusSummary()
            {
                RunningStateLabel = runningState,
                IsError = lastOperationResult?.IsFailed ?? false,
                LastOperationMessage = lastOperationResult?.Message
            };
        }

        private Amatsukaze.Shared.QueueItemView ToQueueItemView(QueueItem item)
        {
            return new Amatsukaze.Shared.QueueItemView()
            {
                Id = item.Id,
                Mode = (Amatsukaze.Shared.ProcMode)item.Mode,
                SrcPath = item.SrcPath,
                DirName = item.DirName,
                FileName = item.FileName,
                ServiceName = item.ServiceName,
                ProfileName = item.Profile?.Name ?? item.ProfileName,
                State = item.State.ToString(),
                StateLabel = GetStateLabel(item),
                Priority = item.Priority,
                IsBatch = item.IsBatch,
                EncodeStart = item.EncodeStart == DateTime.MinValue ? null : item.EncodeStart,
                EncodeFinish = item.EncodeFinish == DateTime.MinValue ? null : item.EncodeFinish,
                TsTime = item.TsTime == DateTime.MinValue ? null : item.TsTime,
                EitStartTime = item.EITStartTime == DateTime.MinValue ? null : item.EITStartTime,
                DisplayBroadcastTime = item.EITStartTime != DateTime.MinValue
                    ? item.EITStartTime.ToString("yyyy/MM/dd")
                    : (item.TsTime == DateTime.MinValue ? null : item.TsTime.ToString("yyyy/MM/dd")),
                DisplayEncodeStart = item.EncodeStart.ToGUIString(),
                DisplayEncodeFinish = item.EncodeFinish.ToGUIString(),
                Progress = 0,
                ConsoleId = item.ConsoleId,
                OutputMask = 0,
                IsTooSmall = IsTooSmall(item),
                Tags = item.Tags,
                OutDir = item.DstPath,
                ImageWidth = item.ImageWidth,
                ImageHeight = item.ImageHeight,
                Genres = item.Genre?.Select(g => new Amatsukaze.Shared.GenreNodeView
                {
                    Space = g.Space,
                    Level1 = g.Level1,
                    Level2 = g.Level2,
                    Name = null,
                    Children = null
                }).ToList(),
                GenreNames = item.Genre?.Select(g => SubGenre.GetDisplayGenre(g)?.FullName ?? SubGenre.GetUnknownFullName(g)).ToList()
            };
        }

        private static string GetStateLabel(QueueItem item)
        {
            switch (item.State)
            {
                case QueueState.Queue:
                    return "待ち";
                case QueueState.Encoding:
                    switch (item.Mode)
                    {
                        case ProcMode.CMCheck:
                            return "CM解析中→" + (item.ConsoleId + 1);
                        case ProcMode.DrcsCheck:
                            return "DRCSチェック中→" + (item.ConsoleId + 1);
                    }
                    return "エンコード中→" + (item.ConsoleId + 1);
                case QueueState.Failed:
                case QueueState.PreFailed:
                    return "失敗";
                case QueueState.LogoPending:
                    return "ペンディング";
                case QueueState.Canceled:
                    return "キャンセル";
                case QueueState.Complete:
                    return "完了";
            }
            return "不明";
        }

        private static bool IsTooSmall(QueueItem item)
        {
            return item.State == QueueState.PreFailed &&
                   !string.IsNullOrEmpty(item.FailReason) &&
                   item.FailReason.Contains("映像が小さすぎます");
        }

        private static ConsoleTaskInfo BuildConsoleTaskInfo(QueueItem item)
        {
            return new ConsoleTaskInfo()
            {
                TaskId = item.Id,
                FileName = item.FileName,
                ServiceName = item.ServiceName,
                ProfileName = item.Profile?.Name ?? item.ProfileName,
                OutDir = item.DstPath
            };
        }

        private static ConsoleTaskInfo CopyConsoleTaskInfo(ConsoleTaskInfo info)
        {
            return new ConsoleTaskInfo()
            {
                TaskId = info.TaskId,
                FileName = info.FileName,
                ServiceName = info.ServiceName,
                ProfileName = info.ProfileName,
                OutDir = info.OutDir
            };
        }

        private void UpdateTaskConsoleMappings(IEnumerable<QueueItem> items)
        {
            if (items == null)
            {
                return;
            }
            foreach (var item in items)
            {
                EnsureTaskConsoleMapping(item);
            }
        }

        private void ClearTaskConsoles()
        {
            taskConsoles.Clear();
            consoleIdToTaskId.Clear();
            taskIdToConsoleId.Clear();
            pendingConsoleSnapshots.Clear();
            pendingConsoleBytes.Clear();
        }

        private void RemoveTaskConsole(int taskId)
        {
            taskConsoles.Remove(taskId);
            if (taskIdToConsoleId.TryGetValue(taskId, out var consoleId))
            {
                taskIdToConsoleId.Remove(taskId);
                if (consoleIdToTaskId.TryGetValue(consoleId, out var mappedTaskId) && mappedTaskId == taskId)
                {
                    consoleIdToTaskId.Remove(consoleId);
                }
                pendingConsoleSnapshots.Remove(consoleId);
                pendingConsoleBytes.Remove(consoleId);
            }
        }

        private void EnsureTaskConsoleMapping(QueueItem item)
        {
            if (item == null)
            {
                return;
            }
            if (item.State != QueueState.Encoding)
            {
                return;
            }
            if (item.EncodeStart == DateTime.MinValue)
            {
                return;
            }
            var consoleId = item.ConsoleId;
            if (consoleId < 0)
            {
                return;
            }
            if (consoleIdToTaskId.TryGetValue(consoleId, out var existingTaskId) == false ||
                existingTaskId != item.Id)
            {
                consoleIdToTaskId[consoleId] = item.Id;
            }
            taskIdToConsoleId[item.Id] = consoleId;
            EnsureTaskConsoleState(item);
            ApplyPendingConsoleSnapshot(consoleId, item.Id);
            ApplyPendingConsoleBytes(consoleId, item.Id);
        }

        private void EnsureTaskConsoleMapping(int consoleId)
        {
            if (consoleId < 0 || consoleIdToTaskId.ContainsKey(consoleId))
            {
                return;
            }
            var item = queueItems.FirstOrDefault(q =>
                q != null &&
                q.State == QueueState.Encoding &&
                q.ConsoleId == consoleId &&
                q.EncodeStart != DateTime.MinValue);
            if (item != null)
            {
                EnsureTaskConsoleMapping(item);
            }
        }

        private bool TryResolveTaskIdFromConsoleId(int consoleId, out int taskId)
        {
            if (consoleIdToTaskId.TryGetValue(consoleId, out taskId))
            {
                return true;
            }
            EnsureTaskConsoleMapping(consoleId);
            return consoleIdToTaskId.TryGetValue(consoleId, out taskId);
        }

        private void EnsureTaskConsoleState(QueueItem item)
        {
            if (item == null)
            {
                return;
            }
            if (!taskConsoles.TryGetValue(item.Id, out var console))
            {
                taskConsoles[item.Id] = new TaskConsoleState(BuildConsoleTaskInfo(item));
                return;
            }
            console.UpdateTaskInfo(BuildConsoleTaskInfo(item));
            console.ResetForLive();
        }

        private void ApplyConsoleSnapshot(int consoleId, List<string> text)
        {
            if (consoleId < 0)
            {
                return;
            }
            if (TryResolveTaskIdFromConsoleId(consoleId, out var taskId) &&
                taskConsoles.TryGetValue(taskId, out var console))
            {
                console.SetTextLines(text);
                return;
            }
            pendingConsoleSnapshots[consoleId] = text != null ? new List<string>(text) : new List<string>();
        }

        private void ApplyPendingConsoleSnapshot(int consoleId, int taskId)
        {
            if (!pendingConsoleSnapshots.TryGetValue(consoleId, out var text) ||
                !taskConsoles.TryGetValue(taskId, out var console))
            {
                return;
            }
            console.SetTextLines(text);
            pendingConsoleSnapshots.Remove(consoleId);
        }

        private void AddPendingConsoleBytes(int consoleId, byte[] bytes)
        {
            if (bytes == null || bytes.Length == 0)
            {
                return;
            }
            if (!pendingConsoleBytes.TryGetValue(consoleId, out var list))
            {
                list = new List<byte>();
                pendingConsoleBytes[consoleId] = list;
            }
            list.AddRange(bytes);
            if (list.Count > MaxPendingConsoleBytes)
            {
                var trim = list.Count - MaxPendingConsoleBytes;
                list.RemoveRange(0, trim);
            }
        }

        private void ApplyPendingConsoleBytes(int consoleId, int taskId)
        {
            if (!pendingConsoleBytes.TryGetValue(consoleId, out var list) ||
                !taskConsoles.TryGetValue(taskId, out var console))
            {
                return;
            }
            var data = list.ToArray();
            pendingConsoleBytes.Remove(consoleId);
            console.AddBytes(data, 0, data.Length);
        }

        private bool TryCreateFallbackConsole(QueueItem item, out TaskConsoleState console)
        {
            console = null;
            var logPath = ResolveTaskLogPath(item);
            if (string.IsNullOrEmpty(logPath))
            {
                return false;
            }
            var lines = ReadLogFileLines(logPath);
            console = new TaskConsoleState(BuildConsoleTaskInfo(item));
            console.LoadFallbackLines(lines);
            return true;
        }

        private string ResolveTaskLogPath(QueueItem item)
        {
            if (item == null)
            {
                return null;
            }
            if (item.IsCheck)
            {
                var check = FindCheckLog(item);
                if (check != null)
                {
                    return encodeServer.GetCheckLogFileBase(check.CheckStartDate) + ".txt";
                }
            }
            else
            {
                var log = FindEncodeLog(item);
                if (log != null)
                {
                    return encodeServer.GetLogFileBase(log.EncodeStartDate) + ".txt";
                }
            }
            if (item.EncodeStart != DateTime.MinValue)
            {
                var path = item.IsCheck
                    ? encodeServer.GetCheckLogFileBase(item.EncodeStart) + ".txt"
                    : encodeServer.GetLogFileBase(item.EncodeStart) + ".txt";
                if (File.Exists(path))
                {
                    return path;
                }
            }
            return null;
        }

        private LogItem FindEncodeLog(QueueItem item)
        {
            if (item == null)
            {
                return null;
            }
            var logs = logItems.Where(x => x != null && x.SrcPath == item.SrcPath).ToList();
            if (logs.Count == 0)
            {
                return null;
            }
            if (item.EncodeStart != DateTime.MinValue)
            {
                var exact = logs.FirstOrDefault(x => x.EncodeStartDate == item.EncodeStart);
                if (exact != null)
                {
                    return exact;
                }
            }
            return logs.OrderByDescending(x => x.EncodeStartDate).FirstOrDefault();
        }

        private CheckLogItem FindCheckLog(QueueItem item)
        {
            if (item == null)
            {
                return null;
            }
            var targetType = item.Mode == ProcMode.DrcsCheck ? CheckType.DRCS : CheckType.CM;
            var logs = checkLogItems
                .Where(x => x != null && x.SrcPath == item.SrcPath && x.Type == targetType)
                .ToList();
            if (logs.Count == 0)
            {
                return null;
            }
            if (item.EncodeStart != DateTime.MinValue)
            {
                var exact = logs.FirstOrDefault(x => x.CheckStartDate == item.EncodeStart);
                if (exact != null)
                {
                    return exact;
                }
            }
            return logs.OrderByDescending(x => x.CheckStartDate).FirstOrDefault();
        }

        private static List<string> ReadLogFileLines(string path)
        {
            if (string.IsNullOrEmpty(path) || !File.Exists(path))
            {
                return new List<string> { "ログファイルが見つかりません。パス: " + path };
            }
            var bytes = File.ReadAllBytes(path);
            var content = Util.AmatsukazeDefaultEncoding.GetString(bytes);
            var lines = SplitLines(content);
            TrimLogLines(lines);
            return lines;
        }

        private static List<string> SplitLines(string content)
        {
            if (string.IsNullOrEmpty(content))
            {
                return new List<string>();
            }
            var normalized = content.Replace("\r\n", "\n").Replace('\r', '\n');
            return normalized.Split('\n').ToList();
        }

        private static void TrimLogLines(List<string> lines)
        {
            if (lines.Count <= ConsoleConstants.MaxConsoleLines)
            {
                return;
            }
            var trim = Math.Max(ConsoleConstants.ConsoleTrimLines, lines.Count - ConsoleConstants.MaxConsoleLines);
            trim = Math.Min(trim, lines.Count);
            lines.RemoveRange(0, trim);
        }

        public Task OnUIData(UIData data)
        {
            if (data == null)
            {
                return Task.FromResult(0);
            }
            lock (sync)
            {
                if (data.State != null)
                {
                    state = ServerSupport.DeepCopy(data.State);
                }
                if (data.QueueData != null)
                {
                    queueItems = data.QueueData.Items?
                        .Where(item => item != null && string.IsNullOrEmpty(item.SrcPath) == false)
                        .Select(CopyQueueItem)
                        .ToList() ?? new List<QueueItem>();
                    ResetQueueChanges();
                    UpdateTaskConsoleMappings(queueItems);
                }
                if (data.QueueUpdate != null)
                {
                    var update = data.QueueUpdate;
                    if (update.Type == UpdateType.Clear)
                    {
                        queueItems.Clear();
                        ResetQueueChanges();
                        ClearTaskConsoles();
                        return Task.FromResult(0);
                    }
                    var idx = queueItems.FindIndex(item => item.Id == update.Item?.Id);
                    if (update.Type == UpdateType.Remove)
                    {
                        if (idx >= 0)
                        {
                            queueItems.RemoveAt(idx);
                        }
                        if (update.Item != null)
                        {
                            RemoveTaskConsole(update.Item.Id);
                            AddQueueChange(new Amatsukaze.Shared.QueueChange()
                            {
                                Type = Amatsukaze.Shared.QueueChangeType.Remove,
                                Id = update.Item.Id
                            });
                        }
                    }
                    else if (update.Type == UpdateType.Move)
                    {
                        if (idx >= 0 && update.Position >= 0 && update.Position <= queueItems.Count)
                        {
                            var item = queueItems[idx];
                            queueItems.RemoveAt(idx);
                            if (update.Position >= queueItems.Count)
                            {
                                queueItems.Add(item);
                            }
                            else
                            {
                                queueItems.Insert(update.Position, item);
                            }
                        }
                        if (update.Item != null)
                        {
                            AddQueueChange(new Amatsukaze.Shared.QueueChange()
                            {
                                Type = Amatsukaze.Shared.QueueChangeType.Move,
                                Id = update.Item.Id,
                                Position = update.Position
                            });
                        }
                    }
                    else if (update.Type == UpdateType.Add || update.Type == UpdateType.Update)
                    {
                        if (update.Item != null && string.IsNullOrEmpty(update.Item.SrcPath) == false)
                        {
                            if (idx >= 0)
                            {
                                queueItems[idx] = CopyQueueItem(update.Item);
                            }
                            else
                            {
                                queueItems.Add(CopyQueueItem(update.Item));
                            }
                            EnsureTaskConsoleMapping(update.Item);
                            AddQueueChange(new Amatsukaze.Shared.QueueChange()
                            {
                                Type = update.Type == UpdateType.Add ? Amatsukaze.Shared.QueueChangeType.Add : Amatsukaze.Shared.QueueChangeType.Update,
                                Item = ToQueueItemView(update.Item)
                            });
                        }
                    }
                }
                if (data.LogData != null)
                {
                    logItems = data.LogData.Items?.Select(CopyLogItem).Reverse().ToList() ?? new List<LogItem>();
                }
                if (data.LogItem != null)
                {
                    logItems.Insert(0, CopyLogItem(data.LogItem));
                }
                if (data.CheckLogData != null)
                {
                    checkLogItems = data.CheckLogData.Items?.Select(CopyCheckLogItem).Reverse().ToList() ?? new List<CheckLogItem>();
                }
                if (data.CheckLogItem != null)
                {
                    checkLogItems.Insert(0, CopyCheckLogItem(data.CheckLogItem));
                }
                if (data.ConsoleData != null)
                {
                    ApplyConsoleSnapshot(data.ConsoleData.index, data.ConsoleData.text);
                }
                if (data.EncodeState != null)
                {
                    EnsureTaskConsoleMapping(data.EncodeState.ConsoleId);
                }
                if (data.SleepCancel != null)
                {
                    finishSetting = ServerSupport.DeepCopy(data.SleepCancel);
                }
            }
            return Task.FromResult(0);
        }

        private Amatsukaze.Shared.QueueCounters BuildQueueCounters(IEnumerable<QueueItem> items)
        {
            var list = items as IList<QueueItem> ?? items.ToList();
            return new Amatsukaze.Shared.QueueCounters()
            {
                Active = list.Count(s => s.IsActive),
                Encoding = list.Count(s => s.State == QueueState.Encoding),
                Complete = list.Count(s => s.State == QueueState.Complete),
                Pending = list.Count(s => s.State == QueueState.LogoPending),
                Failed = list.Count(s => s.State == QueueState.Failed || s.State == QueueState.PreFailed),
                Canceled = list.Count(s => s.State == QueueState.Canceled)
            };
        }

        private static string BuildQueueDigest(Amatsukaze.Shared.QueueCounters counters, IEnumerable<QueueItem> viewItems)
        {
            var sb = new StringBuilder();
            sb.Append("A=").Append(counters.Active)
              .Append("|E=").Append(counters.Encoding)
              .Append("|C=").Append(counters.Complete)
              .Append("|P=").Append(counters.Pending)
              .Append("|F=").Append(counters.Failed)
              .Append("|X=").Append(counters.Canceled)
              .Append(";");

            foreach (var item in viewItems)
            {
                sb.Append(item.Id).Append('|')
                  .Append(item.State).Append('|')
                  .Append(GetStateLabel(item)).Append('|')
                  .Append(item.FileName ?? "").Append('|')
                  .Append(item.ServiceName ?? "").Append('|')
                  .Append(item.Profile?.Name ?? item.ProfileName ?? "").Append('|')
                  .Append(item.Priority).Append('|')
                  .Append(item.IsBatch ? "1" : "0").Append('|')
                  .Append(item.EncodeStart == DateTime.MinValue ? "" : item.EncodeStart.ToUniversalTime().Ticks.ToString()).Append('|')
                  .Append(item.EncodeFinish == DateTime.MinValue ? "" : item.EncodeFinish.ToUniversalTime().Ticks.ToString()).Append('|')
                  .Append(item.EncodeStart.ToGUIString()).Append('|')
                  .Append(item.EncodeFinish.ToGUIString()).Append('|')
                  .Append(item.ConsoleId).Append('|')
                  .Append(IsTooSmall(item) ? "1" : "0").Append(';');
            }

            using var sha = SHA256.Create();
            var bytes = Encoding.UTF8.GetBytes(sb.ToString());
            var hash = sha.ComputeHash(bytes);
            return Convert.ToHexString(hash);
        }

        private void ResetQueueChanges()
        {
            queueVersion++;
            queueChanges.Clear();
        }

        private void AddQueueChange(Amatsukaze.Shared.QueueChange change)
        {
            queueVersion++;
            queueChanges.Add(new QueueChangeRecord()
            {
                Version = queueVersion,
                Change = change
            });
            if (queueChanges.Count > MaxQueueChanges)
            {
                queueChanges.RemoveAt(0);
            }
        }

        private static bool FilterMessageEvent(Amatsukaze.Shared.MessageEventView evt, string page, string requestId, HashSet<string> levels)
        {
            if (!string.IsNullOrEmpty(page) && !string.Equals(evt.Page, page, StringComparison.OrdinalIgnoreCase))
            {
                return false;
            }
            if (!string.IsNullOrEmpty(requestId) && !string.Equals(evt.RequestId, requestId, StringComparison.Ordinal))
            {
                return false;
            }
            if (levels != null && levels.Count > 0 && !levels.Contains(evt.Level))
            {
                return false;
            }
            return true;
        }

        private void AddMessageEvent(OperationResult result)
        {
            if (result == null || string.IsNullOrEmpty(result.Message))
            {
                return;
            }

            var ctx = OperationContextScope.Current;
            var evt = new Amatsukaze.Shared.MessageEventView
            {
                Id = ++messageVersion,
                Time = DateTime.UtcNow,
                Level = result.IsFailed ? "error" : "info",
                Message = result.Message,
                Source = ctx?.Source ?? "server",
                Page = ctx?.Page,
                Action = ctx?.Action,
                RequestId = ctx?.RequestId
            };

            messageLog.Add(new MessageRecord
            {
                Id = evt.Id,
                Event = evt
            });

            if (messageLog.Count > MaxMessageLog)
            {
                messageLog.RemoveAt(0);
            }
        }

        public Task OnCommonData(CommonData data)
        {
            if (data == null)
            {
                return Task.FromResult(0);
            }
            lock (sync)
            {
                if (data.Setting != null)
                {
                    setting = ServerSupport.DeepCopy(data.Setting);
                }
                if (data.UIState != null)
                {
                    uiState = ServerSupport.DeepCopy(data.UIState);
                }
                if (data.MakeScriptData != null)
                {
                    makeScriptData = ServerSupport.DeepCopy(data.MakeScriptData);
                }
                if (data.AddQueueBatFiles != null)
                {
                    addQueueBatFiles = data.AddQueueBatFiles.ToList();
                }
                if (data.QueueFinishBatFiles != null)
                {
                    queueFinishBatFiles = data.QueueFinishBatFiles.ToList();
                }
                if (data.JlsCommandFiles != null)
                {
                    jlsCommandFiles = data.JlsCommandFiles.ToList();
                }
                if (data.MainScriptFiles != null)
                {
                    mainScriptFiles = data.MainScriptFiles.ToList();
                }
                if (data.PostScriptFiles != null)
                {
                    postScriptFiles = data.PostScriptFiles.ToList();
                }
                if (data.PreBatFiles != null)
                {
                    preBatFiles = data.PreBatFiles.ToList();
                }
                if (data.PreEncodeBatFiles != null)
                {
                    preEncodeBatFiles = data.PreEncodeBatFiles.ToList();
                }
                if (data.PostBatFiles != null)
                {
                    postBatFiles = data.PostBatFiles.ToList();
                }
                if (data.Disks != null)
                {
                    disks = data.Disks.Select(item => ServerSupport.DeepCopy(item)).ToList();
                }
                if (data.CpuClusters != null)
                {
                    cpuClusters = data.CpuClusters.ToList();
                }
                if (data.ServerInfo != null)
                {
                    serverInfo = ServerSupport.DeepCopy(data.ServerInfo);
                }
                if (data.FinishSetting != null)
                {
                    finishSetting = ServerSupport.DeepCopy(data.FinishSetting);
                }
            }
            return Task.FromResult(0);
        }

        public Task OnConsoleUpdate(ConsoleUpdate update)
        {
            if (update == null)
            {
                return Task.FromResult(0);
            }
            byte[] converted;
            lock (sync)
            {
                var charset = serverInfo?.CharSet ?? Util.AmatsukazeDefaultEncoding.CodePage;
                Encoding src;
                try
                {
                    src = Encoding.GetEncoding(charset);
                }
                catch
                {
                    src = Util.AmatsukazeDefaultEncoding;
                }
                converted = Util.ConvertEncoding(update.data, src, Util.AmatsukazeDefaultEncoding);
                if (update.index == -1)
                {
                    return Task.FromResult(0);
                }
                if (TryResolveTaskIdFromConsoleId(update.index, out var taskId) &&
                    taskConsoles.TryGetValue(taskId, out var console))
                {
                    console.AddBytes(converted, 0, converted.Length);
                }
                else
                {
                    AddPendingConsoleBytes(update.index, converted);
                }
            }
            return Task.FromResult(0);
        }

        public Task OnEncodeState(EncodeState stateInfo)
        {
            if (stateInfo == null)
            {
                return Task.FromResult(0);
            }
            lock (sync)
            {
                EnsureTaskConsoleMapping(stateInfo.ConsoleId);
            }
            return Task.FromResult(0);
        }

        public Task OnLogFile(string str)
        {
            lock (sync)
            {
                lastLogFile = str;
            }
            return Task.FromResult(0);
        }

        public Task OnProfile(ProfileUpdate data)
        {
            if (data == null)
            {
                return Task.FromResult(0);
            }
            lock (sync)
            {
                if (data.Type == UpdateType.Clear)
                {
                    profiles.Clear();
                    return Task.FromResult(0);
                }
                if (data.Type == UpdateType.Remove && data.Profile != null)
                {
                    profiles.Remove(data.Profile.Name);
                    return Task.FromResult(0);
                }
                if (data.Profile != null)
                {
                    var copy = CopyProfile(data.Profile);
                    if (!string.IsNullOrEmpty(data.NewName))
                    {
                        profiles.Remove(copy.Name);
                        copy.Name = data.NewName;
                    }
                    profiles[copy.Name] = copy;
                }
            }
            return Task.FromResult(0);
        }

        public Task OnAutoSelect(AutoSelectUpdate data)
        {
            if (data == null)
            {
                return Task.FromResult(0);
            }
            lock (sync)
            {
                if (data.Type == UpdateType.Clear)
                {
                    autoSelects.Clear();
                    return Task.FromResult(0);
                }
                if (data.Type == UpdateType.Remove && data.Profile != null)
                {
                    autoSelects.Remove(data.Profile.Name);
                    return Task.FromResult(0);
                }
                if (data.Profile != null)
                {
                    var copy = CopyAutoSelect(data.Profile);
                    if (!string.IsNullOrEmpty(data.NewName))
                    {
                        autoSelects.Remove(copy.Name);
                        copy.Name = data.NewName;
                    }
                    autoSelects[copy.Name] = copy;
                }
            }
            return Task.FromResult(0);
        }

        public Task OnServiceSetting(ServiceSettingUpdate update)
        {
            if (update == null)
            {
                return Task.FromResult(0);
            }
            lock (sync)
            {
                if (update.Type == ServiceSettingUpdateType.Clear)
                {
                    services.Clear();
                    return Task.FromResult(0);
                }
                if (update.Type == ServiceSettingUpdateType.Remove)
                {
                    services.Remove(update.ServiceId);
                    return Task.FromResult(0);
                }
                if (update.Type == ServiceSettingUpdateType.RemoveLogo)
                {
                    if (services.TryGetValue(update.ServiceId, out var service) &&
                        service?.LogoSettings != null &&
                        update.RemoveLogoIndex >= 0 &&
                        update.RemoveLogoIndex < service.LogoSettings.Count)
                    {
                        service.LogoSettings.RemoveAt(update.RemoveLogoIndex);
                    }
                    return Task.FromResult(0);
                }
                if (update.Type == ServiceSettingUpdateType.Update && update.Data != null)
                {
                    services[update.ServiceId] = CopyService(update.Data);
                }
            }
            return Task.FromResult(0);
        }

        public Task OnLogoData(LogoData logoData)
        {
            return Task.FromResult(0);
        }

        public Task OnDrcsData(DrcsImageUpdate update)
        {
            if (update == null)
            {
                return Task.FromResult(0);
            }
            lock (sync)
            {
                Action<DrcsImage> updateItem = image =>
                {
                    if (image == null)
                        return;
                    if (update.Type == DrcsUpdateType.Remove)
                    {
                        drcsMap.Remove(image.MD5);
                    }
                    else
                    {
                        drcsMap[image.MD5] = ServerSupport.DeepCopy(image);
                    }
                };
                if (update.Image != null)
                {
                    updateItem(update.Image);
                }
                if (update.ImageList != null)
                {
                    foreach (var item in update.ImageList)
                    {
                        updateItem(item);
                    }
                }
            }
            return Task.FromResult(0);
        }

        public Task OnAddResult(string requestId)
        {
            return Task.FromResult(0);
        }

        public Task OnOperationResult(OperationResult result)
        {
            lock (sync)
            {
                lastOperationResult = result != null ? ServerSupport.DeepCopy(result) : null;
                AddMessageEvent(result);
            }
            return Task.FromResult(0);
        }

        public void Finish()
        {
        }

    }
}
