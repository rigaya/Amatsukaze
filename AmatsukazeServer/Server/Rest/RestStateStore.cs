using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using Amatsukaze.Lib;
using Amatsukaze.Shared;

namespace Amatsukaze.Server.Rest
{
    internal class RestConsoleBuffer : ConsoleTextBase
    {
        private readonly List<string> lines = new List<string>();

        public IReadOnlyList<string> Lines {
            get { return lines; }
        }

        public override void OnAddLine(string text)
        {
            if (lines.Count > 800)
            {
                lines.RemoveAt(0);
            }
            lines.Add(text);
        }

        public override void OnReplaceLine(string text)
        {
            if (lines.Count == 0)
            {
                lines.Add(text);
            }
            else
            {
                lines[lines.Count - 1] = text;
            }
        }

        public void SetTextLines(List<string> newLines)
        {
            Clear();
            lines.Clear();
            if (newLines != null)
            {
                lines.AddRange(newLines);
            }
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
        private List<LogItem> logItems = new List<LogItem>();
        private List<CheckLogItem> checkLogItems = new List<CheckLogItem>();

        private readonly Dictionary<int, RestConsoleBuffer> consoleBuffers = new Dictionary<int, RestConsoleBuffer>();
        private readonly RestConsoleBuffer addQueueConsole = new RestConsoleBuffer();
        private readonly Dictionary<int, EncodeState> encodeStates = new Dictionary<int, EncodeState>();

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
                return new SystemSnapshot()
                {
                    ServerInfo = serverInfo != null ? ServerSupport.DeepCopy(serverInfo) : null,
                    State = state != null ? ServerSupport.DeepCopy(state) : null,
                    FinishSetting = finishSetting != null ? ServerSupport.DeepCopy(finishSetting) : null,
                    StatusSummary = BuildStatusSummary()
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
                items = queueItems.ToList();
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
            return new Amatsukaze.Shared.QueueView()
            {
                Version = currentVersion,
                Items = list,
                Counters = counters,
                Filters = filter
            };
        }

        public Amatsukaze.Shared.QueueChangesView GetQueueChanges(long sinceVersion)
        {
            lock (sync)
            {
                if (sinceVersion > queueVersion)
                {
                    return new Amatsukaze.Shared.QueueChangesView()
                    {
                        FromVersion = sinceVersion,
                        ToVersion = queueVersion,
                        FullSyncRequired = true,
                        Counters = BuildQueueCounters(queueItems)
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
                            Counters = BuildQueueCounters(queueItems)
                        };
                    }
                    return new Amatsukaze.Shared.QueueChangesView()
                    {
                        FromVersion = sinceVersion,
                        ToVersion = queueVersion,
                        FullSyncRequired = true,
                        Counters = BuildQueueCounters(queueItems)
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
                        Counters = BuildQueueCounters(queueItems)
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
                    Changes = changes,
                    Counters = BuildQueueCounters(queueItems)
                };
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

        public ConsoleView GetConsoleView()
        {
            lock (sync)
            {
                var consoles = consoleBuffers
                    .OrderBy(pair => pair.Key)
                    .Select(pair =>
                    {
                        var id = pair.Key;
                        var buffer = pair.Value;
                        encodeStates.TryGetValue(id, out var stateInfo);
                        return new ConsoleState()
                        {
                            Id = id,
                            Lines = buffer.Lines.ToList(),
                            Phase = stateInfo?.Phase ?? ResourcePhase.TSAnalyze,
                            Resource = stateInfo?.Resource
                        };
                    }).ToList();
                return new ConsoleView()
                {
                    Consoles = consoles,
                    AddQueueConsole = addQueueConsole.Lines.ToList()
                };
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
                ConsoleView = GetConsoleView(),
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
                FileName = item.FileName,
                ServiceName = item.ServiceName,
                ProfileName = item.Profile?.Name ?? item.ProfileName,
                State = item.State.ToString(),
                StateLabel = GetStateLabel(item),
                Priority = item.Priority,
                IsBatch = item.IsBatch,
                EncodeStart = item.EncodeStart == DateTime.MinValue ? null : item.EncodeStart,
                EncodeFinish = item.EncodeFinish == DateTime.MinValue ? null : item.EncodeFinish,
                DisplayEncodeStart = item.EncodeStart.ToGUIString(),
                DisplayEncodeFinish = item.EncodeFinish.ToGUIString(),
                Progress = 0,
                ConsoleId = item.ConsoleId,
                OutputMask = 0,
                IsTooSmall = IsTooSmall(item)
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
                    queueItems = data.QueueData.Items?.Select(CopyQueueItem).ToList() ?? new List<QueueItem>();
                    ResetQueueChanges();
                }
                if (data.QueueUpdate != null)
                {
                    var update = data.QueueUpdate;
                    if (update.Type == UpdateType.Clear)
                    {
                        queueItems.Clear();
                        ResetQueueChanges();
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
                        if (update.Item != null)
                        {
                            if (idx >= 0)
                            {
                                queueItems[idx] = CopyQueueItem(update.Item);
                            }
                            else
                            {
                                queueItems.Add(CopyQueueItem(update.Item));
                            }
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
                    if (data.ConsoleData.index == -1)
                    {
                        addQueueConsole.SetTextLines(data.ConsoleData.text);
                    }
                    else
                    {
                        EnsureConsole(data.ConsoleData.index);
                        consoleBuffers[data.ConsoleData.index].SetTextLines(data.ConsoleData.text);
                    }
                }
                if (data.EncodeState != null)
                {
                    encodeStates[data.EncodeState.ConsoleId] = data.EncodeState;
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
                    addQueueConsole.AddBytes(converted, 0, converted.Length);
                }
                else
                {
                    EnsureConsole(update.index);
                    consoleBuffers[update.index].AddBytes(converted, 0, converted.Length);
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
                encodeStates[stateInfo.ConsoleId] = ServerSupport.DeepCopy(stateInfo);
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
            }
            return Task.FromResult(0);
        }

        public void Finish()
        {
        }

        private void EnsureConsole(int index)
        {
            if (!consoleBuffers.ContainsKey(index))
            {
                consoleBuffers[index] = new RestConsoleBuffer();
            }
        }
    }
}
