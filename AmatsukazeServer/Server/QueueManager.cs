﻿using Amatsukaze.Lib;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.Serialization;
using System.Text;
using System.Threading.Tasks;

namespace Amatsukaze.Server
{
    class QueueManager
    {
        private EncodeServer server;

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
                    Queue = (List<QueueItem>)s.ReadObject(fs);
                }
                catch
                {
                    // 古いバージョンのファイルだとエラーになる
                    // キューの復旧は必須ではないのでエラーは無視する
                    Queue = new List<QueueItem>();
                    return;
                }
                foreach(var item in Queue)
                {
                    // エンコードするアイテムはリセットしておく
                    if (item.State == QueueState.Encoding || item.State == QueueState.Queue)
                    {
                        item.Reset();
                    }
                    if(item.Profile == null || item.Profile.LastUpdate == DateTime.MinValue)
                    {
                        item.Profile = server.PendingProfile;
                    }
                    // IDを振り直す
                    item.Order = item.Id = nextItemId++;
                }
            }
        }

        public void SaveQueueData(bool force)
        {
            if(queueUpdated || force)
            {
                queueUpdated = false;
                string path = server.GetQueueFilePath();
                string tmp = path + ".tmp";
                Directory.CreateDirectory(Path.GetDirectoryName(path));
                using (FileStream fs = new FileStream(tmp, FileMode.Create))
                {
                    var s = new DataContractSerializer(typeof(List<QueueItem>));
                    s.WriteObject(fs, Queue);
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
            var formatted = DateTime.Now.ToString("yyyy/MM/dd HH:mm:ss") + " " + line + "\n";
            return WriteTextBytes(Util.AmatsukazeDefaultEncoding.GetBytes(formatted));
        }

        private Task ClientQueueUpdate(QueueUpdate update)
        {
            queueUpdated = true;
            return server.ClientQueueUpdate(update);
        }

        private void UpdateProgress()
        {
            // 進捗を更新
            double enabledCount = Queue.Count(s =>
                s.State != QueueState.LogoPending && s.State != QueueState.PreFailed);
            double remainCount = Queue.Count(s =>
                s.State == QueueState.Queue || s.State == QueueState.Encoding);
            // 完全にゼロだと分からないので・・・
            server.Progress = ((enabledCount - remainCount) + 0.1) / (enabledCount + 0.1);
        }

        public List<Task> UpdateQueueItems(List<Task> waits)
        {
            foreach (var item in Queue.ToArray())
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

            // 既に追加されているファイルは除外する
            // バッチのときは全ファイルが対象だが、バッチじゃなければバッチのみが対象
            var ignores = req.IsBatch ? Queue : Queue.Where(t => t.IsBatch);

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
                        for (int i = 0; i < list.Length; ++i)
                        {
                            var prog = list[i];
                            if (prog.HasVideo &&
                                videopids.Contains(prog.VideoPid) == false)
                            {
                                videopids.Add(prog.VideoPid);

                                var serviceName = "不明";
                                var tsTime = DateTime.MinValue;
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
                                        Id = nextItemId++,
                                        Mode = req.Mode,
                                        SrcPath = additem.Path,
                                        Hash = additem.Hash,
                                        DstPath = Path.Combine(outitem.DstPath, outname),
                                        StreamFormat = (VideoStreamFormat)prog.Stream,
                                        ServiceId = prog.ServiceId,
                                        ImageWidth = prog.Width,
                                        ImageHeight = prog.Height,
                                        TsTime = tsTime,
                                        ServiceName = serviceName,
                                        EventName = prog.EventName,
                                        State = QueueState.LogoPending,
                                        Priority = outitem.Priority,
                                        AddTime = DateTime.Now,
                                        ProfileName = outitem.Profile,
                                        Genre = genre,
                                        Tags = new List<string>()
                                    };

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

                                        ++numFiles;
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
                                Id = nextItemId++,
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
                                Tags = new List<string>()
                            };

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
                        item.Order = Queue.Count;
                        Queue.Add(item);
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
            newItem.Id = nextItemId++;
            newItem.Order = Queue.Count;
            Queue.Add(newItem);

            // 状態はリセットしておく
            newItem.Reset();
            UpdateQueueItem(newItem, null);

            waits.Add(ClientQueueUpdate(new QueueUpdate()
            {
                Type = UpdateType.Add,
                Item = newItem
            }));
        }

        internal Task NotifyQueueItemUpdate(QueueItem item)
        {
            UpdateProgress();
            if (Queue.Contains(item))
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
            for(int i = 0; i < Queue.Count; ++i)
            {
                Queue[i].Order = i;
            }
            server.ReScheduleQueue();
        }

        private void RemoveCompleted(List<Task> waits)
        {
            var removeItems = Queue.Where(s => s.State == QueueState.Complete || s.State == QueueState.PreFailed).ToArray();
            if(removeItems.Length > 0)
            {
                foreach (var item in removeItems)
                {
                    Queue.Remove(item);
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
            var target = Queue.FirstOrDefault(s => s.Id == data.ItemId);
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

                if (target.IsBatch)
                {
                    // バッチモードでfailed/succeededフォルダに移動されていたら戻す
                    if (target.State == QueueState.Failed || target.State == QueueState.Complete)
                    {
                        if (Queue.Where(s => s.SrcPath == target.SrcPath).Any(s => s.IsActive) == false)
                        {
                            var dirPath = Path.GetDirectoryName(target.SrcPath);
                            var movedDir = (target.State == QueueState.Failed) ? 
                                ServerSupport.FAIL_DIR : 
                                ServerSupport.SUCCESS_DIR;
                            var movedPath = Path.Combine(dirPath, movedDir, Path.GetFileName(target.SrcPath));
                            if (File.Exists(movedPath))
                            {
                                // EDCB関連ファイルも移動したかどうかは分からないが、あれば戻す
                                try
                                {
                                    ServerSupport.MoveTSFile(movedPath, dirPath, true);
                                }
                                catch (Exception e)
                                {
                                    return server.FatalError(
                                        "ファイルの移動に失敗しました", e);
                                }
                            }
                        }
                    }
                }

                if (data.ChangeType == ChangeItemType.ResetState)
                {
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
                target.State = QueueState.Canceled;
                Queue.Remove(target);
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
                if(Queue.Where(s => s.SrcPath == target.SrcPath).Any(s => s.IsActive))
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
                Queue.Remove(target);
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
                if(data.Position >= Queue.Count)
                {
                    return server.NotifyError("位置が範囲外です", false);
                }

                Queue.Remove(target);
                Queue.Insert(data.Position, target);
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

        private Task WriteTextBytes(QueueState state, QueueItem item, string text)
        {
            string formatted = String.Format(text, state, item.Id, Path.GetFileName(item.SrcPath));
            return WriteTextBytes(Util.AmatsukazeDefaultEncoding.GetBytes(formatted));
        }
    }
}
