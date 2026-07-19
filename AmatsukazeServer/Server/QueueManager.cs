using Amatsukaze.Lib;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.Serialization;
using System.Text;
using System.Globalization;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using Amatsukaze.Shared;

namespace Amatsukaze.Server
{
    class QueueManager
    {
        private static readonly log4net.ILog LOG = log4net.LogManager.GetLogger("QueueManager");
        private static readonly Regex TaskTempDirRegex = new Regex(@"一時フォルダ\s*[:：]\s*(.+)", RegexOptions.Compiled);
        private static readonly Regex AmtTaskDirNameRegex = new Regex(@"^amt[0-9]+$", RegexOptions.Compiled | RegexOptions.CultureInvariant);
        private EncodeServer server;
        private readonly object queueSync = new object();

        public List<QueueItem> Queue { get; private set; } = new List<QueueItem>();

        class DirHash
        {
            public string DirPath;
            public Dictionary<string, byte[]> HashDict = new Dictionary<string, byte[]>();
        }

        class ConsoleText : ConsoleTextBase
        {
            private static log4net.ILog LOG = log4net.LogManager.GetLogger("UserScript.Add");

            private RollingTextLines Lines = new RollingTextLines(500);

            public List<string> TextLines
            {
                get
                {
                    return Lines.TextLines;
                }
            }

            public override void OnAddLine(string text)
            {
                Lines.AddLine(text);
                LOG.Info(text);
            }

            public override void OnReplaceLine(string text)
            {
                Lines.ReplaceLine(text);
                LOG.Info(text);
            }
        }

        private Dictionary<string, DirHash> hashCache = new Dictionary<string, DirHash>();
        private ConsoleText consoleText = new ConsoleText();

        public List<string> TextLines { get { return consoleText.TextLines; } }

        private int nextItemId = 1;
        private bool queueUpdated = false;

        // キャンセルサポート
        private IProcessExecuter process;
        private bool addQueueCanceled;

        public QueueManager(EncodeServer server)
        {
            this.server = server;
        }

        public void LoadAppData()
        {
            string path = server.GetQueueFilePath();
            if (File.Exists(path) == false)
            {
                return;
            }
            using (FileStream fs = new FileStream(path, FileMode.Open))
            {
                var s = new DataContractSerializer(typeof(List<QueueItem>));
                try
                {
                    var loaded = (List<QueueItem>)s.ReadObject(fs);
                    if (loaded == null)
                    {
                        return;
                    }
                    var sanitized = loaded
                        .Where(item => item != null && string.IsNullOrEmpty(item.SrcPath) == false)
                        .ToList();
                    if (sanitized.Count != loaded.Count)
                    {
                        LOG.Warn($"Queue restore: dropped {loaded.Count - sanitized.Count} invalid items (null or SrcPath missing).");
                    }
                    lock (queueSync)
                    {
                        Queue = sanitized;
                        nextItemId = 1;
                        foreach (var item in Queue)
                        {
                            // エンコードするアイテムはリセットしておく
                            if (item.State == QueueState.Encoding || item.State == QueueState.Queue)
                            {
                                item.Reset();
                            }
                            if (item.Profile == null || item.Profile.LastUpdate == DateTime.MinValue)
                            {
                                item.Profile = server.PendingProfile;
                            }
                            item.ClearAutoLogoTransientState();
                            // IDを振り直す
                            item.Order = item.Id = nextItemId++;
                        }
                    }
                    return;
                }
                catch
                {
                    // 古いバージョンのファイルだとエラーになる
                    // キューの復旧は必須ではないのでエラーは無視する
                    lock (queueSync)
                    {
                        Queue = new List<QueueItem>();
                    }
                    return;
                }
            }
        }

        public void SaveQueueData(bool force)
        {
            List<QueueItem> snapshot = null;
            bool shouldSave = false;
            lock (queueSync)
            {
                if (queueUpdated || force)
                {
                    queueUpdated = false;
                    if (Queue.Any(item => item == null || string.IsNullOrEmpty(item.SrcPath)))
                    {
                        var before = Queue.Count;
                        Queue = Queue.Where(item => item != null && string.IsNullOrEmpty(item.SrcPath) == false).ToList();
                        LOG.Warn($"Queue save: dropped {before - Queue.Count} invalid items (null or SrcPath missing).");
                    }

                    var dupGroups = Queue.GroupBy(item => item.Id).Where(g => g.Count() > 1).ToList();
                    if (dupGroups.Count > 0)
                    {
                        int maxId = Queue.Max(item => item.Id);
                        if (nextItemId <= maxId)
                        {
                            nextItemId = maxId + 1;
                        }
                        foreach (var group in dupGroups)
                        {
                            bool first = true;
                            foreach (var item in group)
                            {
                                if (first)
                                {
                                    first = false;
                                    continue;
                                }
                                var oldId = item.Id;
                                item.Id = Interlocked.Increment(ref nextItemId);
                                LOG.Warn($"Queue save: duplicate id {oldId} reassigned to {item.Id}.");
                            }
                        }
                    }

                    for (int i = 0; i < Queue.Count; i++)
                    {
                        Queue[i].Order = i;
                    }

                    snapshot = Queue.ToList();
                    shouldSave = true;
                }
            }

            if (shouldSave)
            {
                string path = server.GetQueueFilePath();
                string tmp = path + ".tmp";
                Directory.CreateDirectory(Path.GetDirectoryName(path));
                using (FileStream fs = new FileStream(tmp, FileMode.Create))
                {
                    var s = new DataContractSerializer(typeof(List<QueueItem>));
                    s.WriteObject(fs, snapshot);
                }
                // ファイル置き換え
                StorageUtility.MoveFileWithOverwrite(tmp, path);
            }
        }

        private Task WriteTextBytes(byte[] buffer, int offset, int length)
        {
            consoleText.AddBytes(buffer, offset, length);

            byte[] newbuf = new byte[length];
            Array.Copy(buffer, newbuf, length);
            return server.Client.OnConsoleUpdate(new ConsoleUpdate() { index = -1, data = newbuf });
        }

        private Task WriteTextBytes(byte[] buffer)
        {
            return WriteTextBytes(buffer, 0, buffer.Length);
        }

        private Task WriteLine(string line)
        {
            var formatted = DateTime.Now.ToString("yyyy/MM/dd HH:mm:ss", CultureInfo.InvariantCulture) + " " + line + "\n";
            return WriteTextBytes(Util.AmatsukazeDefaultEncoding.GetBytes(formatted));
        }

        private Task ClientQueueUpdate(QueueUpdate update)
        {
            lock (queueSync)
            {
                queueUpdated = true;
            }
            return server.ClientQueueUpdate(update);
        }

        private void UpdateProgress()
        {
            // 進捗を更新
            double enabledCount;
            double remainCount;
            lock (queueSync)
            {
                enabledCount = Queue.Count(s =>
                    s.State != QueueState.LogoPending && s.State != QueueState.PreFailed);
                remainCount = Queue.Count(s =>
                    s.State == QueueState.Queue || s.State == QueueState.Encoding);
            }
            // 完全にゼロだと分からないので・・・
            server.Progress = ((enabledCount - remainCount) + 0.1) / (enabledCount + 0.1);
        }

        public List<Task> UpdateQueueItems(List<Task> waits)
        {
            QueueItem[] items;
            lock (queueSync)
            {
                items = Queue.ToArray();
            }
            foreach (var item in items)
            {
                if (item.State != QueueState.LogoPending && item.State != QueueState.Queue)
                {
                    continue;
                }
                if (UpdateQueueItem(item, waits))
                {
                    waits?.Add(NotifyQueueItemUpdate(item));
                }
            }
            return waits;
        }

        private bool CheckProfile(QueueItem item, List<Task> waits)
        {
            if (item.Profile != server.PendingProfile)
            {
                // すでにプロファイルが決定済み
                return true;
            }
            if(item.State == QueueState.PreFailed)
            {
                // TSファイルの情報取得に失敗している
                return false;
            }

            // ペンディングならプロファイルの決定を試みる
            int itemPriority = 0;
            var profile = server.SelectProfile(item, out itemPriority);
            if(profile == null)
            {
                return false;
            }

            item.Profile = ServerSupport.DeepCopy(profile);
            if (itemPriority > 0)
            {
                item.Priority = itemPriority;
            }

            waits?.Add(ClientQueueUpdate(new QueueUpdate()
            {
                Type = UpdateType.Remove,
                Item = item
            }));

            return true;
        }

        // ペンディング <=> キュー 状態を切り替える
        // ペンディングからキューになったらスケジューリングに追加する
        // notifyItem: trueの場合は、ディレクトリ・アイテム両方の更新通知、falseの場合は、ディレクトリの更新通知のみ
        // 戻り値: 状態が変わった
        public bool UpdateQueueItem(QueueItem item, List<Task> waits)
        {
            if (item.State == QueueState.LogoPending || item.State == QueueState.Queue)
            {
                var prevState = item.State;
                if(item.Mode == ProcMode.DrcsCheck)
                {
                    // DRCSチェックはプロファイルを必要としないので即開始
                    if(item.State == QueueState.LogoPending)
                    {
                        item.FailReason = "";
                        item.ClearAutoLogoTransientState();
                        item.State = QueueState.Queue;
                        server.ScheduleQueueItem(item);
                    }
                }
                else if (CheckProfile(item, waits))
                {
                    var map = server.ServiceMap;
                    if (item.ServiceId == -1)
                    {
                        item.FailReason = "TS情報取得中";
                        item.Reset();
                    }
                    else if (map.ContainsKey(item.ServiceId) == false)
                    {
                        item.FailReason = "このTSのチャンネル設定がありません（追加し直してください）";
                        item.Reset();
                    }
                    else if (!server.AppData_.setting.LogoPendAsError // ロゴ設定をエラー扱いする場合はここでチェックせず、実際に処理してエラーを発生させる
                        && ((!item.Profile.DisableChapter || !item.Profile.NoDelogo) &&
                        map[item.ServiceId].LogoSettings.Any(s => s.CanUse(item.TsTime)) == false))
                    {
                        item.FailReason = "ロゴ設定がありません";
                        item.Reset();
                        // ロゴ設定不足で保留になった場合は、設定に応じて自動補完を非同期実行する
                        server.TryKickAutoLogoPending(item);
                    }
                    else if(item.IsSeparateHashRequired && item.Hash == null)
                    {
                        item.Reset();
                    }
                    else
                    {
                        // OK
                        if (item.State == QueueState.LogoPending)
                        {
                            item.FailReason = "";
                            item.ClearAutoLogoTransientState();
                            item.State = QueueState.Queue;

                            server.ScheduleQueueItem(item);
                        }
                    }
                }
                return prevState != item.State;
            }
            return false;
        }

        private AMTContext amtcontext = new AMTContext();
        public async Task AddQueue(AddQueueRequest req)
        {
            List<Task> waits = new List<Task>();

            addQueueCanceled = false;

            // 受信内容のログ出力（デバッグ用）
            try
            {
                var firstOutput = (req.Outputs != null && req.Outputs.Count > 0) ? req.Outputs[0] : null;
                var firstTarget = (req.Targets != null && req.Targets.Count > 0) ? req.Targets[0] : null;
                var msg = string.Format(
                    "[AddQueue/Receive] DirPath='{0}', Targets={1}, FirstTarget='{2}', OutDir='{3}', Profile='{4}', Priority={5}, Mode={6}, RequestId='{7}', AddQueueBat='{8}'",
                    req.DirPath ?? "<null>",
                    req.Targets?.Count ?? 0,
                    firstTarget?.Path ?? "<null>",
                    firstOutput?.DstPath ?? "<null>",
                    firstOutput?.Profile ?? "<null>",
                    firstOutput?.Priority ?? 0,
                    req.Mode,
                    req.RequestId ?? "<null>",
                    req.AddQueueBat ?? "<null>");
                Debug.Print(msg);
            }
            catch { }

            // ユーザ操作でない場合はログを記録する
            bool enableLog = (req.Mode == ProcMode.AutoBatch);

            if (req.Outputs.Count == 0)
            {
                await server.NotifyError("出力が1つもありません", enableLog);
                return;
            }

            if (req.Outputs.Any(o => string.IsNullOrWhiteSpace(o.DstPath)))
            {
                await server.NotifyError("出力先ディレクトリが指定されていません", enableLog);
                return;
            }

            // 既に追加されているファイルは除外する
            // バッチのときは全ファイルが対象だが、バッチじゃなければバッチのみが対象
            List<QueueItem> ignoreSnapshot;
            lock (queueSync)
            {
                ignoreSnapshot = Queue.ToList();
            }
            var ignores = req.IsBatch ? ignoreSnapshot : ignoreSnapshot.Where(t => t.IsBatch);

            var ignoreSet = new HashSet<string>(
                ignores.Where(item => item.IsActive)
                .Select(item => item.SrcPath));

            var items = ((req.Targets != null)
                ? req.Targets
                : Directory.GetFiles(req.DirPath)
                    .Where(s =>
                    {
                        string lower = s.ToLower();
                        return lower.EndsWith(".ts") || lower.EndsWith(".m2t");
                    })
                    .Select(f => new AddQueueItem() { Path = f }))
                    .Where(f => !ignoreSet.Contains(f.Path)).ToList();

            waits.Add(WriteLine("" + items.Count + "件を追加処理します"));

            var map = server.ServiceMap;
            var numItems = 0;
            var progress = 0;

            // TSファイル情報を読む
            foreach (var additem in items)
            {
                waits.Add(WriteLine("(" + (++progress) + "/" + items.Count + ") " + Path.GetFileName(additem.Path) + " を処理中"));

                using (var info = new TsInfo(amtcontext))
                {
                    var failReason = "";
                    var addItems = new List<QueueItem>();
                    if (await Task.Run(() => info.ReadFile(additem.Path)) == false)
                    {
                        failReason = "TS情報取得に失敗: " + amtcontext.GetError();
                    }
                    else
                    {
                        failReason = "TSファイルに映像が見つかりませんでした";
                        var list = info.GetProgramList();
                        var videopids = new List<int>();
                        int numFiles = 0;
                        for (int i = 0; i < list.Length; i++)
                        {
                            var prog = list[i];
                            if (prog.HasVideo &&
                                videopids.Contains(prog.VideoPid) == false)
                            {
                                videopids.Add(prog.VideoPid);

                                var serviceName = "不明";
                                var tsTime = DateTime.MinValue;
                                var eitStartTime = info.GetEITStartTime();
                                if (info.HasServiceInfo)
                                {
                                    var service = info.GetServiceList().Where(s => s.ServiceId == prog.ServiceId).FirstOrDefault();
                                    if (service.ServiceId != 0)
                                    {
                                        serviceName = service.ServiceName;
                                    }
                                    tsTime = info.GetTime();
                                }

                                var outname = Path.GetFileNameWithoutExtension(additem.Path);
                                if (numFiles > 0)
                                {
                                    outname += "-マルチ" + numFiles;
                                }

                                Debug.Print("解析完了: " + additem.Path);

                                foreach (var outitem in req.Outputs)
                                {
                                    var genre = prog.Content.Select(s => ServerSupport.GetGenre(s)).ToList();

                                    var item = new QueueItem()
                                    {
                                        Mode = req.Mode,
                                        SrcPath = additem.Path,
                                        Hash = additem.Hash,
                                        DstPath = Path.Combine(outitem.DstPath, outname),
                                        StreamFormat = (VideoStreamFormat)prog.Stream,
                                        ServiceId = prog.ServiceId,
                                        ImageWidth = prog.Width,
                                        ImageHeight = prog.Height,
                                        TsTime = tsTime,
                                        EITStartTime = eitStartTime,
                                        ServiceName = serviceName,
                                        EventName = prog.EventName,
                                        State = QueueState.LogoPending,
                                        Priority = outitem.Priority,
                                        AddTime = DateTime.Now,
                                        ProfileName = outitem.Profile,
                                        Genre = genre,
                                        Tags = (req.Tags != null && req.Tags.Count > 0)
                                            ? new List<string>(req.Tags)
                                            : new List<string>()
                                    };
                                    item.ResetAutoLogoAttempt();

                                    if (item.IsOneSeg)
                                    {
                                        item.State = QueueState.PreFailed;
                                        item.FailReason = "映像が小さすぎます(" + prog.Width + "," + prog.Height + ")";
                                    }
                                    else
                                    {
                                        // ロゴファイルを探す
                                        if (req.Mode != ProcMode.DrcsCheck && map.ContainsKey(item.ServiceId) == false)
                                        {
                                            // 新しいサービスを登録
                                            waits.Add(server.AddService(new ServiceSettingElement()
                                            {
                                                ServiceId = item.ServiceId,
                                                ServiceName = item.ServiceName,
                                                LogoSettings = new List<LogoSetting>()
                                            }));
                                        }

                                        // 追加時バッチ
                                        if(string.IsNullOrEmpty(req.AddQueueBat) == false)
                                        {
                                            waits.Add(WriteLine("追加時バッチ起動"));
                                            using (var scriptExecuter = new UserScriptExecuter()
                                            {
                                                Server = server,
                                                Phase = ScriptPhase.OnAdd,
                                                ScriptPath = Path.Combine(server.GetBatDirectoryPath(), req.AddQueueBat),
                                                Item = item,
                                                Prog = prog,
                                                OnOutput = WriteTextBytes
                                            })
                                            {
                                                process = scriptExecuter;
                                                await scriptExecuter.Execute();
                                                process = null;
                                            }

                                            if (addQueueCanceled)
                                            {
                                                break;
                                            }
                                        }

                                        numFiles++;
                                    }

                                    addItems.Add(item);
                                }
                            }
                        }
                    }

                    if (addQueueCanceled)
                    {
                        break;
                    }

                    if (addItems.Count == 0)
                    {
                        // アイテムが１つもないときはエラー項目として追加
                        foreach (var outitem in req.Outputs)
                        {
                            bool isAuto = false;
                            var profileName = ServerSupport.ParseProfileName(outitem.Profile, out isAuto);
                            var profile = isAuto ? null : ServerSupport.DeepCopy(server.GetProfile(profileName));

                            var item = new QueueItem()
                            {
                                Mode = req.Mode,
                                Profile = profile,
                                SrcPath = additem.Path,
                                Hash = additem.Hash,
                                DstPath = "",
                                StreamFormat = VideoStreamFormat.Unknown,
                                ServiceId = -1,
                                ImageWidth = -1,
                                ImageHeight = -1,
                                TsTime = DateTime.MinValue,
                                ServiceName = "不明",
                                State = QueueState.PreFailed,
                                FailReason = failReason,
                                AddTime = DateTime.Now,
                                ProfileName = outitem.Profile,
                                Tags = (req.Tags != null && req.Tags.Count > 0)
                                    ? new List<string>(req.Tags)
                                    : new List<string>()
                            };
                            item.ResetAutoLogoAttempt();

                            addItems.Add(item);
                        }
                    }

                    // 1ソースファイルに対するaddはatomicに実行したいので、
                    // このループではawaitしないこと
                    foreach (var item in addItems)
                    {
                        if(item.State != QueueState.PreFailed)
                        {
                            // プロファイルを設定
                            UpdateProfileItem(item, null);
                        }
                        // 追加
                        lock (queueSync)
                        {
                            item.Id = Interlocked.Increment(ref nextItemId);
                            item.Order = Queue.Count;
                            Queue.Add(item);
                        }
                        // まずは内部だけで状態を更新
                        UpdateQueueItem(item, null);
                        // 状態が決まったらクライアント側に追加通知
                        waits.Add(ClientQueueUpdate(new QueueUpdate()
                        {
                            Type = UpdateType.Add,
                            Item = item
                        }));
                    }

                    numItems += addItems.Count;

                    UpdateProgress();
                    waits.Add(server.RequestState());
                }

                if(addQueueCanceled)
                {
                    break;
                }
            }

            if(addQueueCanceled)
            {
                waits.Add(WriteLine("キャンセルされました"));
            }

            waits.Add(WriteLine("" + numItems + "件追加しました"));

            if (addQueueCanceled == false && numItems == 0)
            {
                waits.Add(server.NotifyError(
                    "エンコード対象ファイルがありませんでした。パス:" + req.DirPath, enableLog));

                await Task.WhenAll(waits);

                return;
            }
            else
            {
                waits.Add(server.NotifyMessage("" + numItems + "件追加しました", false));
            }

            if (req.Mode != ProcMode.AutoBatch)
            {
                // 最後に使った設定を記憶しておく
                server.LastUsedProfile = req.Outputs[0].Profile;
                server.AddOutPathHistory(req.Outputs[0].DstPath);
                server.LastAddQueueBat = req.AddQueueBat;
                waits.Add(server.RequestUIState());
            }

            waits.Add(server.RequestFreeSpace());

            await Task.WhenAll(waits);
        }

        public void CancelAddQueue()
        {
            addQueueCanceled = true;
            process?.Canel();
        }

        private void ResetStateItem(QueueItem item, List<Task> waits)
        {
            item.ResetAutoLogoAttempt();
            item.Reset();
            UpdateQueueItem(item, waits);
            waits.Add(NotifyQueueItemUpdate(item));
        }

        // アイテムのProfileNameからプロファイルを決定して、
        // オプションでwaits!=nullのときはクライアントに通知
        // 戻り値: プロファイルが変更された場合（結果、エラーになった場合も含む）
        private bool UpdateProfileItem(QueueItem item, List<Task> waits)
        {
            var getResult = server.GetProfile(item, item.ProfileName);
            var profile = (getResult != null) ? ServerSupport.DeepCopy(getResult.Profile) : server.PendingProfile;
            var priority = (getResult != null && getResult.Priority > 0) ? getResult.Priority : item.Priority;

            if(item.Profile == null ||
                item.Profile.Name != profile.Name ||
                item.Profile.LastUpdate != profile.LastUpdate ||
                item.Priority != priority)
            {
                // 変更
                item.Profile = profile;
                item.Priority = priority;

                // ハッシュリスト取得
                if (profile != server.PendingProfile && // ペンディングの場合は決定したときに実行される
                    item.IsSeparateHashRequired)
                {
                    var hashpath = Path.GetDirectoryName(item.SrcPath) + ".hash";
                    if(hashCache.ContainsKey(hashpath) == false)
                    {
                        if (File.Exists(hashpath) == false)
                        {
                            item.State = QueueState.LogoPending;
                            item.FailReason = "ハッシュファイルがありません: " + hashpath;
                            return true;
                        }
                        else
                        {
                            try
                            {
                                hashCache.Add(hashpath, new DirHash()
                                {
                                    DirPath = hashpath,
                                    HashDict = HashUtil.ReadHashFile(hashpath)
                                });
                            }
                            catch (IOException e)
                            {
                                item.State = QueueState.LogoPending;
                                item.FailReason = "ハッシュファイルの読み込みに失敗: " + e.Message;
                                return true;
                            }
                        }
                    }

                    var cacheItem = hashCache[hashpath];
                    var filename = item.FileName;

                    if(cacheItem.HashDict.ContainsKey(filename) == false)
                    {
                        item.State = QueueState.LogoPending;
                        item.FailReason = "ハッシュファイルにこのファイルのハッシュがありません";
                        return true;
                    }

                    item.Hash = cacheItem.HashDict[filename];
                }

                server.ReScheduleQueue();
                UpdateQueueItem(item, waits);

                waits?.Add(ClientQueueUpdate(new QueueUpdate()
                {
                    Type = UpdateType.Add,
                    Item = item
                }));

                return true;
            }

            return false;
        }

        private void DuplicateItem(QueueItem item, List<Task> waits)
        {
            var newItem = ServerSupport.DeepCopy(item);
            lock (queueSync)
            {
                newItem.Id = Interlocked.Increment(ref nextItemId);
                newItem.Order = Queue.Count;
                Queue.Add(newItem);
            }

            // 状態はリセットしておく
            newItem.ResetAutoLogoAttempt();
            newItem.Reset();
            UpdateQueueItem(newItem, null);

            waits.Add(ClientQueueUpdate(new QueueUpdate()
            {
                Type = UpdateType.Add,
                Item = newItem
            }));
        }

        // バッチ処理後に succeeded / failed へ移動された入力ファイルを元の場所へ戻す
        private void RestoreMovedInputFiles(QueueItem item, bool preserveExistingTrim)
        {
            if (!item.IsBatch || (item.State != QueueState.Failed && item.State != QueueState.Complete))
            {
                return;
            }

            bool hasActive;
            lock (queueSync)
            {
                hasActive = Queue.Where(queueItem => queueItem.SrcPath == item.SrcPath).Any(queueItem => queueItem.IsActive);
            }
            if (hasActive)
            {
                return;
            }

            var dirPath = Path.GetDirectoryName(item.SrcPath);
            var movedDir = item.State == QueueState.Failed ? ServerSupport.FAIL_DIR : ServerSupport.SUCCESS_DIR;
            var movedPath = Path.Combine(dirPath, movedDir, Path.GetFileName(item.SrcPath));
            if (!File.Exists(movedPath))
            {
                return;
            }

            var trimPath = item.SrcPath + ".trim.avs";
            foreach (var moveItem in ServerSupport.GetMoveList(movedPath, dirPath, true))
            {
                // カット調整で保存したTrim指定ファイルは、移動先にある古い内容で上書きしない
                if (preserveExistingTrim && moveItem.DstPath == trimPath && File.Exists(trimPath))
                {
                    continue;
                }
                ServerSupport.MoveFile(moveItem.SrcPath, moveItem.DstPath);
            }
        }

        public async Task<QueueItem> RequeueTrimItem(
            int sourceItemId,
            string profileName,
            int priority,
            List<string> tags,
            string resumeDir,
            bool removeSourceItem)
        {
            QueueItem sourceItem;
            QueueItem newItem;
            lock (queueSync)
            {
                sourceItem = Queue.FirstOrDefault(item => item.Id == sourceItemId);
                if (sourceItem == null)
                {
                    throw new InvalidOperationException("元のキューアイテムが見つかりません");
                }
                if (sourceItem.State != QueueState.Complete)
                {
                    throw new InvalidOperationException("完了していないキューアイテムは再投入できません");
                }
                if (sourceItem.Mode != ProcMode.CMCheck && !sourceItem.IsBatch)
                {
                    throw new InvalidOperationException("CM解析またはエンコード済みのキューアイテムだけ再投入できます");
                }

                if (string.IsNullOrEmpty(sourceItem.SrcPath))
                {
                    throw new InvalidOperationException("入力ファイルが見つかりません");
                }
                if (!File.Exists(sourceItem.SrcPath))
                {
                    try
                    {
                        RestoreMovedInputFiles(sourceItem, true);
                    }
                    catch (Exception e)
                    {
                        throw new InvalidOperationException("入力ファイルの移動に失敗しました", e);
                    }
                }
                if (!File.Exists(sourceItem.SrcPath))
                {
                    throw new InvalidOperationException("入力ファイルが見つかりません");
                }

                newItem = ServerSupport.DeepCopy(sourceItem);
                newItem.Id = Interlocked.Increment(ref nextItemId);
                newItem.Order = Queue.Count;
                newItem.Mode = ProcMode.Batch;
                newItem.Profile = null;
                newItem.ProfileName = profileName;
                newItem.Priority = priority;
                newItem.Tags = tags != null ? new List<string>(tags) : new List<string>();
                newItem.ResumeDir = resumeDir;
                newItem.ActualDstPath = null;
                newItem.ConsoleId = 0;
                newItem.JlsCommand = null;
                newItem.FailReason = "";
                newItem.AddTime = DateTime.Now;
                newItem.ResetAutoLogoAttempt();
                newItem.Reset();
                Queue.Add(newItem);

                if (removeSourceItem)
                {
                    Queue.Remove(sourceItem);
                }
            }

            var waits = new List<Task>();
            UpdateProfileItem(newItem, null);
            UpdateQueueItem(newItem, null);
            UpdateProgress();
            waits.Add(ClientQueueUpdate(new QueueUpdate()
            {
                Type = UpdateType.Add,
                Item = newItem
            }));
            if (removeSourceItem)
            {
                // 再利用Batchが一時フォルダを所有するため、ここではフォルダを削除しない。
                waits.Add(ClientQueueUpdate(new QueueUpdate()
                {
                    Type = UpdateType.Remove,
                    Item = sourceItem
                }));
            }
            UpdateQueueOrder();
            waits.Add(server.NotifyMessage("カット調整後のタスクを再投入しました", false));
            await Task.WhenAll(waits);
            return newItem;
        }

        internal Task NotifyQueueItemUpdate(QueueItem item)
        {
            UpdateProgress();
            bool exists;
            lock (queueSync)
            {
                exists = Queue.Contains(item);
            }
            if (exists)
            {
                // ないアイテムをUpdateすると追加されてしまうので
                return ClientQueueUpdate(new QueueUpdate()
                    {
                        Type = UpdateType.Update,
                        Item = item
                    });
            }
            return Task.FromResult(0);
        }

        private void UpdateQueueOrder()
        {
            lock (queueSync)
            {
                for (int i = 0; i < Queue.Count; i++)
                {
                    Queue[i].Order = i;
                }
            }
            server.ReScheduleQueue();
        }

        public List<QueueItem> GetQueueSnapshot()
        {
            lock (queueSync)
            {
                return Queue.Where(item => item != null && string.IsNullOrEmpty(item.SrcPath) == false).ToList();
            }
        }

        private void TryDeleteTaskWorkDirForQueueRemove(QueueItem item)
        {
            if (server.AppData_?.setting?.DeleteTaskWorkDirOnQueueRemove != true)
            {
                return;
            }
            if (item == null)
            {
                return;
            }

            try
            {
                if (item.State == QueueState.Encoding || item.State == QueueState.LogoPending)
                {
                    LogTaskWorkDirDeleteSkipped(item, "タスクが処理中です");
                    return;
                }

                var logPath = server.ResolveTaskLogPath(item);
                if (string.IsNullOrEmpty(logPath) || !File.Exists(logPath))
                {
                    LogTaskWorkDirDeleteSkipped(item, "ログファイルが見つかりません");
                    return;
                }

                if (!TryExtractTaskWorkDirFromLog(logPath, out var workDir))
                {
                    LogTaskWorkDirDeleteSkipped(item, "ログファイルから一時フォルダを取得できません");
                    return;
                }

                if (!TryNormalizeDeletableTaskWorkDir(workDir, out var fullPath, out var reason))
                {
                    LogTaskWorkDirDeleteSkipped(item, reason);
                    return;
                }

                Directory.Delete(fullPath, true);
                Util.AddLog($"[Queue] タスク削除に伴い一時フォルダを削除しました。ItemId={item.Id}, Path={fullPath}", null);
            }
            catch (Exception ex)
            {
                Util.AddLog($"[Queue] タスク削除に伴う一時フォルダ削除に失敗しました。ItemId={item.Id}", ex);
            }
        }

        private static bool TryExtractTaskWorkDirFromLog(string logPath, out string workDir)
        {
            workDir = null;
            try
            {
                var bytes = File.ReadAllBytes(logPath);
                var content = Util.AmatsukazeDefaultEncoding.GetString(bytes);
                var lines = content.Replace("\r\n", "\n").Replace('\r', '\n').Split('\n');
                foreach (var line in lines)
                {
                    var match = TaskTempDirRegex.Match(line);
                    if (!match.Success)
                    {
                        continue;
                    }
                    var dir = match.Groups[1].Value.Trim().Trim('"');
                    if (string.IsNullOrEmpty(dir))
                    {
                        continue;
                    }
                    workDir = dir;
                    return true;
                }
            }
            catch
            {
                // ログ読み込み失敗は、キュー削除の付随処理ではスキップ扱いにする。
            }
            return false;
        }

        private static bool TryNormalizeDeletableTaskWorkDir(string workDir, out string fullPath, out string reason)
        {
            fullPath = null;
            reason = null;

            if (string.IsNullOrWhiteSpace(workDir))
            {
                reason = "一時フォルダパスが空です";
                return false;
            }
            if (!Path.IsPathRooted(workDir))
            {
                reason = "一時フォルダパスが絶対パスではありません";
                return false;
            }

            try
            {
                fullPath = Path.GetFullPath(workDir);
            }
            catch (Exception ex)
            {
                reason = "一時フォルダパスを正規化できません: " + ex.Message;
                return false;
            }

            var normalized = fullPath.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            var dirName = Path.GetFileName(normalized);
            if (!AmtTaskDirNameRegex.IsMatch(dirName ?? ""))
            {
                reason = "一時フォルダ名がamt<数字>形式ではありません: " + dirName;
                return false;
            }

            var dirInfo = new DirectoryInfo(normalized);
            if (!dirInfo.Exists)
            {
                reason = "一時フォルダが存在しません: " + normalized;
                return false;
            }
            if ((dirInfo.Attributes & FileAttributes.ReparsePoint) != 0)
            {
                reason = "一時フォルダがシンボリックリンクまたは再解析ポイントです: " + normalized;
                return false;
            }

            fullPath = normalized;
            return true;
        }

        private static void LogTaskWorkDirDeleteSkipped(QueueItem item, string reason)
        {
            Util.AddLog($"[Queue] タスク削除に伴う一時フォルダ削除をスキップしました。ItemId={item.Id}, Reason={reason}", null);
        }

        private void RemoveCompleted(List<Task> waits)
        {
            QueueItem[] removeItems;
            lock (queueSync)
            {
                removeItems = Queue.Where(s => s.State == QueueState.Complete || s.State == QueueState.PreFailed).ToArray();
                if (removeItems.Length > 0)
                {
                    foreach (var item in removeItems)
                    {
                        Queue.Remove(item);
                    }
                }
            }
            if(removeItems.Length > 0)
            {
                foreach (var item in removeItems)
                {
                    TryDeleteTaskWorkDirForQueueRemove(item);
                    waits.Add(ClientQueueUpdate(new QueueUpdate()
                    {
                        Type = UpdateType.Remove,
                        Item = item
                    }));
                }
                UpdateQueueOrder();
            } 
            waits.Add(server.NotifyMessage("" + removeItems.Length + "件削除しました", false));
        }

        public Task ChangeItem(ChangeItemData data)
        {
            if (data.ChangeType == ChangeItemType.RemoveCompleted)
            {
                var waits = new List<Task>();
                RemoveCompleted(waits);
                return Task.WhenAll(waits);
            }

            // アイテム操作
            QueueItem target;
            lock (queueSync)
            {
                target = Queue.FirstOrDefault(s => s.Id == data.ItemId);
            }
            if (target == null)
            {
                return server.NotifyError(
                    "指定されたアイテムが見つかりません", false);
            }

            if (data.ChangeType == ChangeItemType.ResetState ||
                data.ChangeType == ChangeItemType.UpdateProfile ||
                data.ChangeType == ChangeItemType.Duplicate)
            {
                if(target.State == QueueState.PreFailed)
                {
                    return server.NotifyError("このアイテムは追加処理に失敗しているため操作できません", false);
                }
                if (data.ChangeType == ChangeItemType.ResetState)
                {
                    // エンコード中は変更できない
                    if (target.State == QueueState.Encoding)
                    {
                        return server.NotifyError("エンコード中のアイテムはリトライできません", false);
                    }
                }
                else if (data.ChangeType == ChangeItemType.UpdateProfile)
                {
                    // エンコード中は変更できない
                    if (target.State == QueueState.Encoding)
                    {
                        return server.NotifyError("エンコード中のアイテムはプロファイル更新できません", false);
                    }
                }
                else if (data.ChangeType == ChangeItemType.Duplicate)
                {
                    // バッチモードでアクティブなやつは重複になるのでダメ
                    if (target.IsBatch && target.IsActive)
                    {
                        return server.NotifyError("通常モードで追加されたアイテムは複製できません", false);
                    }
                }

                var waits = new List<Task>();

                try
                {
                    RestoreMovedInputFiles(target, false);
                }
                catch (Exception e)
                {
                    return server.FatalError("ファイルの移動に失敗しました", e);
                }

                if (data.ChangeType == ChangeItemType.ResetState)
                {
                    // モード変更が指定されていれば適用
                    if (data.Mode.HasValue)
                    {
                        target.Mode = data.Mode.Value;
                    }
                    // プロファイル変更が指定されていれば適用
                    if (!string.IsNullOrEmpty(data.Profile))
                    {
                        target.ProfileName = data.Profile;
                    }
                    // リトライはプロファイル再適用も行う
                    UpdateProfileItem(target, waits);
                    ResetStateItem(target, waits);
                    waits.Add(server.NotifyMessage("リトライします", false));
                }
                else if (data.ChangeType == ChangeItemType.UpdateProfile)
                {
                    if(UpdateProfileItem(target, waits))
                    {
                        waits.Add(server.NotifyMessage("新しいプロファイルが適用されました", false));
                    }
                    else
                    {
                        waits.Add(server.NotifyMessage("すでに最新のプロファイルが適用されています", false));
                    }
                }
                else
                {
                    DuplicateItem(target, waits);
                    waits.Add(server.NotifyMessage("複製しました", false));
                }

                return Task.WhenAll(waits);
            }
            else if (data.ChangeType == ChangeItemType.Cancel)
            {
                if (server.CancelItem(target) || target.IsActive)
                {
                    target.State = QueueState.Canceled;
                    return Task.WhenAll(
                        ClientQueueUpdate(new QueueUpdate()
                        {
                            Type = UpdateType.Update,
                            Item = target
                        }),
                        server.NotifyMessage("キャンセルしました", false));
                }
                else
                {
                    return server.NotifyError(
                        "このアイテムはアクティブ状態でないため、キャンセルできません", false);
                }
            }
            else if (data.ChangeType == ChangeItemType.Priority)
            {
                target.Priority = data.Priority;
                server.ReScheduleQueue();
                return Task.WhenAll(
                    ClientQueueUpdate(new QueueUpdate()
                    {
                        Type = UpdateType.Update,
                        Item = target
                    }),
                    server.NotifyMessage("優先度を変更しました", false));
            }
            else if (data.ChangeType == ChangeItemType.Profile)
            {
                if (target.State == QueueState.Encoding)
                {
                    return server.NotifyError("エンコード中はプロファイル変更できません", false);
                }
                if (target.State == QueueState.PreFailed)
                {
                    return server.NotifyError("このアイテムはプロファイル変更できません", false);
                }

                var waits = new List<Task>();
                target.ProfileName = data.Profile;
                if (UpdateProfileItem(target, waits))
                {
                    waits.Add(server.NotifyMessage("プロファイルを「" + data.Profile + "」に変更しました", false));
                }
                else
                {
                    waits.Add(server.NotifyMessage("既に同じプロファイルが適用されています", false));
                }

                return Task.WhenAll(waits);
            }
            else if (data.ChangeType == ChangeItemType.RemoveItem)
            {
                server.CancelItem(target);
                TryDeleteTaskWorkDirForQueueRemove(target);
                target.State = QueueState.Canceled;
                lock (queueSync)
                {
                    Queue.Remove(target);
                }
                UpdateQueueOrder();
                return Task.WhenAll(
                    ClientQueueUpdate(new QueueUpdate()
                    {
                        Type = UpdateType.Remove,
                        Item = target
                    }),
                    server.NotifyMessage("アイテムを削除しました", false));
            }
            else if(data.ChangeType == ChangeItemType.ForceStart)
            {
                if(target.State != QueueState.Queue)
                {
                    return server.NotifyError("待ち状態にないアイテムは開始できません", false);
                }
                else
                {
                    server.ForceStartItem(target);
                }
            }
            else if(data.ChangeType == ChangeItemType.RemoveSourceFile)
            {
                if(target.IsBatch == false)
                {
                    return server.NotifyError("通常or自動追加以外はTSファイル削除ができません", false);
                }
                if(target.State != QueueState.Complete)
                {
                    return server.NotifyError("完了していないアイテムはTSファイル削除ができません", false);
                }
                bool hasActive;
                lock (queueSync)
                {
                    hasActive = Queue.Where(s => s.SrcPath == target.SrcPath).Any(s => s.IsActive);
                }
                if (hasActive)
                {
                    return server.NotifyError("まだ完了していない項目があるため、このTSは削除ができません", false);
                }

                // ！！！削除！！！
                var dirPath = Path.GetDirectoryName(target.SrcPath);
                var movedPath = Path.Combine(dirPath, ServerSupport.SUCCESS_DIR, Path.GetFileName(target.SrcPath));
                if (File.Exists(movedPath))
                {
                    // EDCB関連ファイルも移動したかどうかは分からないが、あれば消す
                    try
                    {
                        ServerSupport.DeleteTSFile(movedPath, true);
                    }
                    catch (Exception e)
                    {
                        return server.FatalError(
                            "ファイルの削除に失敗しました", e);
                    }
                }

                // アイテム削除
                TryDeleteTaskWorkDirForQueueRemove(target);
                lock (queueSync)
                {
                    Queue.Remove(target);
                }
                UpdateQueueOrder();
                return Task.WhenAll(
                    ClientQueueUpdate(new QueueUpdate()
                    {
                        Type = UpdateType.Remove,
                        Item = target
                    }),
                    server.NotifyMessage("TSファイルを削除しました", false));
            }
            else if(data.ChangeType == ChangeItemType.Move)
            {
                int queueCount;
                lock (queueSync)
                {
                    queueCount = Queue.Count;
                }
                if (data.Position >= queueCount)
                {
                    return server.NotifyError("位置が範囲外です", false);
                }

                lock (queueSync)
                {
                    Queue.Remove(target);
                    Queue.Insert(data.Position, target);
                }
                UpdateQueueOrder();
                return Task.WhenAll(
                    ClientQueueUpdate(new QueueUpdate()
                    {
                        Type = UpdateType.Move,
                        Item = target,
                        Position = data.Position
                    }));
            }
            return Task.FromResult(0);
        }

        public Task MoveItems(QueueMoveManyRequest data)
        {
            if (data == null || data.ItemIds == null || data.ItemIds.Count == 0)
            {
                return Task.FromResult(0);
            }

            List<QueueItem> finalOrder;
            lock (queueSync)
            {
                var ordered = Queue.ToList();
                var idSet = new HashSet<int>(data.ItemIds);
                var orderMap = new Dictionary<int, int>();
                for (int i = 0; i < data.ItemIds.Count; i++)
                {
                    if (orderMap.ContainsKey(data.ItemIds[i]) == false)
                    {
                        orderMap[data.ItemIds[i]] = i;
                    }
                }
                var moving = ordered
                    .Where(item => idSet.Contains(item.Id))
                    .OrderBy(item => orderMap.TryGetValue(item.Id, out var idx) ? idx : int.MaxValue)
                    .ToList();
                if (moving.Count == 0)
                {
                    return Task.FromResult(0);
                }

                var remaining = ordered.Where(item => idSet.Contains(item.Id) == false).ToList();

                var dropIndex = Math.Clamp(data.DropIndex, 0, ordered.Count);
                var beforeCount = 0;
                for (int i = 0; i < ordered.Count && i < dropIndex; i++)
                {
                    if (idSet.Contains(ordered[i].Id))
                    {
                        beforeCount++;
                    }
                }
                var adjustedDrop = Math.Clamp(dropIndex - beforeCount, 0, remaining.Count);

                remaining.InsertRange(adjustedDrop, moving);

                Queue.Clear();
                Queue.AddRange(remaining);
                finalOrder = Queue.ToList();
            }

            UpdateQueueOrder();

            var waits = new List<Task>();
            for (int i = 0; i < finalOrder.Count; i++)
            {
                var item = finalOrder[i];
                waits.Add(ClientQueueUpdate(new QueueUpdate()
                {
                    Type = UpdateType.Move,
                    Item = item,
                    Position = i
                }));
            }
            return Task.WhenAll(waits);
        }

        private Task WriteTextBytes(QueueState state, QueueItem item, string text)
        {
            string formatted = String.Format(text, state, item.Id, Path.GetFileName(item.SrcPath));
            return WriteTextBytes(Util.AmatsukazeDefaultEncoding.GetBytes(formatted));
        }
    }
}
