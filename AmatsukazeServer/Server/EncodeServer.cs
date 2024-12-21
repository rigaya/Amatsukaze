﻿#define PROFILE
using Amatsukaze.Lib;
using Livet;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net;
using System.Reflection;
using System.Runtime.Serialization;
using System.Text;
using System.Threading.Tasks;
using System.Threading.Tasks.Dataflow;

namespace Amatsukaze.Server
{
    public class EncodeServer : NotificationObject, IEncodeServer, ISleepCancel, IDisposable
    {
        [DataContract]
        public class AppData : IExtensibleDataObject
        {
            [DataMember]
            public Setting setting;
            [DataMember, Obsolete("EncodeServer.UIState_を使ってください", false)]
            public UIState uiState;
            [DataMember]
            public MakeScriptData scriptData;
            [DataMember]
            public ServiceSetting services;
            [DataMember]
            public FinishSetting finishSetting;

            // 0: ～4.0.3
            // 1: 4.1.0～ DRCS文字のハッシュ並びを修正
            // 2: 8.6.0～ uiState(History)を別ファイル化
            [DataMember]
            public int Version;

            public ExtensionDataObject ExtensionData { get; set; }
        }

        private class EncodeException : Exception
        {
            public EncodeException(string message)
                : base(message)
            {
            }
        }

        internal IUserClient Client { get; private set; }
        public Task ServerTask { get; private set; }
        internal AppData AppData_ { get; private set; }

        private Action finishRequested;

        private QueueManager queueManager;
        private ScheduledQueue scheduledQueue;
        private WorkerPool workerPool;

        private FinishActionRunner finishActionRunner;

        private LogData logData = new LogData();
        private CheckLogData checkLogData = new CheckLogData();
        private SortedDictionary<string, DiskItem> diskMap = new SortedDictionary<string, DiskItem>();

        internal readonly AffinityCreator affinityCreator = new AffinityCreator();

        private Dictionary<string, ProfileSetting> profiles = new Dictionary<string, ProfileSetting>(StringComparer.OrdinalIgnoreCase);
        private Dictionary<string, AutoSelectProfile> autoSelects = new Dictionary<string, AutoSelectProfile>(StringComparer.OrdinalIgnoreCase);
        private List<string> JlsCommandFiles = new List<string>();
        private List<string> MainScriptFiles = new List<string>();
        private List<string> PostScriptFiles = new List<string>();
        private List<string> AddQueueBatFiles = new List<string>();
        private List<string> PreBatFiles = new List<string>();
        private List<string> PostBatFiles = new List<string>();
        private DRCSManager drcsManager;

        private UIState UIState_ = new UIState() { OutputPathHistory = new List<string>() };
        private OrderedSet<string> OutPathHistory = new OrderedSet<string>();

        // キューに追加されるTSを解析するスレッド
        private Task queueThread;
        private BufferBlock<object> queueQ = new BufferBlock<object>();

        // ロゴファイルやJLSコマンドファイルを監視するスレッド
        private Task watchFileThread;
        private BufferBlock<int> watchFileQ = new BufferBlock<int>();
        private bool serviceListUpdated;

        // 設定を保存するスレッド
        private Task saveSettingThread;
        private BufferBlock<int> saveSettingQ = new BufferBlock<int>();
        private bool settingUpdated;
        private bool uiStateUpdated;
        private bool autoSelectUpdated;

        // DRCS処理用スレッド
        private Task drcsThread;
        private BufferBlock<int> drcsQ = new BufferBlock<int>();

        private PreventSuspendContext preventSuspend;

        // プロファイル未選択状態のダミープロファイル
        public ProfileSetting PendingProfile { get; private set; }

        internal ResourceManager ResourceManager { get; private set; } = new ResourceManager();

        private PauseScheduler pauseScheduler;

        // データファイル
        private DataFile<LogItem> logFile;
        private DataFile<CheckLogItem> checkLogFile;

        #region QueuePaused変更通知プロパティ
        private bool _QueuePaused = false;

        public bool QueuePaused {
            get { return _QueuePaused; }
            set { 
                if (_QueuePaused == value)
                    return;
                _QueuePaused = value;
                RaisePropertyChanged();
            }
        }
        #endregion

        #region EncodePaused変更通知プロパティ
        private bool encodePaused = false;

        public bool EncodePaused {
            get { return encodePaused; }
            set {
                if (encodePaused == value)
                    return;
                encodePaused = value;
                RaisePropertyChanged();
            }
        }
        #endregion

        #region NowEncoding変更通知プロパティ
        private bool nowEncoding = false;

        public bool NowEncoding {
            get { return nowEncoding; }
            set {
                if (nowEncoding == value)
                    return;
                nowEncoding = value;
                RaisePropertyChanged();
            }
        }
        #endregion

        #region Progress変更通知プロパティ
        private double _Progress;

        public double Progress {
            get { return _Progress; }
            set { 
                if (_Progress == value)
                    return;
                _Progress = value;
                RaisePropertyChanged();
            }
        }
        #endregion

        #region SleepCancelData変更通知プロパティ
        private FinishSetting _SleepCancelData;

        public FinishSetting SleepCancel {
            get { return _SleepCancelData; }
            set { 
                if (_SleepCancelData == value)
                    return;
                _SleepCancelData = value;
                RaisePropertyChanged();
            }
        }
        #endregion

        public ClientManager ClientManager {
            get { return Client as ClientManager; }
        }

        public Dictionary<int, ServiceSettingElement> ServiceMap { get { return AppData_.services.ServiceMap; } }

        public string Version {
            get {
                return FileVersionInfo.GetVersionInfo(
                        Assembly.GetExecutingAssembly().Location).FileVersion;
            }
        }

        public string LastUsedProfile {
            get { return UIState_.LastUsedProfile; }
            set {
                if (UIState_.LastUsedProfile != value)
                {
                    UIState_.LastUsedProfile = value;
                    uiStateUpdated = true;
                }
            }
        }

        public string LastAddQueueBat {
            get { return UIState_.LastAddQueueBat; }
            set {
                if (UIState_.LastAddQueueBat != value)
                {
                    UIState_.LastAddQueueBat = value;
                    uiStateUpdated = true;
                }
            }
        }

        public EncodeServer(int port, IUserClient client, Action finishRequested)
        {
#if PROFILE
            var prof = new Profile();
#endif
            this.finishRequested = finishRequested;

            queueManager = new QueueManager(this);
            drcsManager = new DRCSManager(this);

            LoadAppData();
            LoadUIState();
            LoadAutoSelectData();
            if (client != null)
            {
                // スタンドアロン
                this.Client = client;

                // 終了待機
                var fs = ServerSupport.CreateStandaloneMailslot();
                ServerSupport.WaitStandaloneMailslot(fs).ContinueWith(task =>
                {
                    // 終了リクエストが来た
                    client.Finish();
                });
            }
            else
            {
                var clientManager = new ClientManager(this);
                ServerTask = clientManager.Listen(port);
                this.Client = clientManager;
                RaisePropertyChanged("ClientManager");
            }
#if PROFILE
            prof.PrintTime("EncodeServer 1");
#endif
            PendingProfile = new ProfileSetting()
            {
                Name = "プロファイル未選択",
                LastUpdate = DateTime.MinValue,
            };

            scheduledQueue = new ScheduledQueue();
            workerPool = new WorkerPool()
            {
                Queue = scheduledQueue,
                NewWorker = id => new TranscodeWorker(id, this),
                OnStart = async () => {
                    if(!Directory.Exists(GetDRCSDirectoryPath()))
                    {
                        Directory.CreateDirectory(GetDRCSDirectoryPath());
                    }
                    if(!File.Exists(GetDRCSMapPath()))
                    {
                        using (File.Create(GetDRCSMapPath())) { }
                    }
                    if (AppData_.setting.ClearWorkDirOnStart)
                    {
                        CleanTmpDir();
                    }
                    await CancelSleep(); // 大丈夫だと思うけどきれいにしておく
                    NowEncoding = true;
                    Progress = 0;
                    await RequestState(StateChangeEvent.WorkersStarted);
                },
                OnFinish = async ()=> {
                    NowEncoding = false;
                    Progress = 1;
                    if(disposedValue)
                    {
                        return;
                    }
                    await RequestState(StateChangeEvent.WorkersFinished);
                    if (preventSuspend != null)
                    {
                        preventSuspend.Dispose();
                        preventSuspend = null;
                    }
                    if (AppData_.finishSetting.Action != FinishAction.None
                        && (!AppData_.setting.NoActionExe || !FinishActionRunner.CheckNoActionExeExists(AppData_.setting.NoActionExeList)))
                    {
                        await CancelSleep(); // 2重に走るのは回避する
                        finishActionRunner = new FinishActionRunner(
                            AppData_.finishSetting.Action, AppData_.finishSetting.Seconds, AppData_.setting.NoActionExe, AppData_.setting.NoActionExeList);
                        SleepCancel = new FinishSetting()
                        {
                            Action = AppData_.finishSetting.Action,
                            Seconds = AppData_.finishSetting.Seconds - 2
                        };
                        await Client?.OnUIData(new UIData()
                        {
                            SleepCancel = SleepCancel
                        });
                    }
                },
                OnError = (id, mes, e) =>
                {
                    if(e != null)
                    {
                        return FatalError(id, mes, e);
                    }
                    else
                    {
                        return NotifyMessage(id, mes, true);
                    }
                }
            };
            workerPool.SetNumParallel(AppData_.setting.NumParallel);
            scheduledQueue.WorkerPool = workerPool;
            
            SetScheduleParam(AppData_.setting.SchedulingEnabled, 
                AppData_.setting.NumGPU, AppData_.setting.MaxGPUResources);

            pauseScheduler = new PauseScheduler(this, workerPool);

#if PROFILE
            prof.PrintTime("EncodeServer 2");
#endif
        }

        // コンストラクタはasyncにできないのでasyncする処理は分離
        public async Task Init()
        {
#if PROFILE
            var prof = new Profile();
#endif
            // 古いバージョンからの更新処理
            UpdateFromOldVersion();
#if PROFILE
            prof.PrintTime("EncodeServer A");
#endif
            // エンコードを開始する前にログは読み込んでおく
            logFile = new DataFile<LogItem>(GetHistoryFilePathV2());
            checkLogFile = new DataFile<CheckLogItem>(GetCheckHistoryFilePath());

            logData.Items = await logFile.Read();
            checkLogData.Items = await checkLogFile.Read();
#if PROFILE
            prof.PrintTime("EncodeServer B");
#endif
            // キュー状態を戻す
            queueManager.LoadAppData();
            if (AppData_.setting.PauseOnStarted && queueManager.Queue.Any(s => s.IsActive))
            {
                // アクティブなアイテムがある状態から開始する場合はキューを凍結する
                QueuePaused = true;
                workerPool.SetPause(true, false);
                queueManager.UpdateQueueItems(null);
            }

            // DRCS文字情報解析に回す
            foreach (var item in logData.Items)
            {
                drcsManager.AddLogFile(GetLogFileBase(item.EncodeStartDate) + ".txt",
                    item.SrcPath, item.EncodeFinishDate);
            }
            foreach (var item in checkLogData.Items)
            {
                drcsManager.AddLogFile(GetCheckLogFileBase(item.CheckStartDate) + ".txt",
                    item.SrcPath, item.CheckStartDate);
            }
#if PROFILE
            prof.PrintTime("EncodeServer C");
#endif
            watchFileThread = WatchFileThread();
            saveSettingThread = SaveSettingThread();
            queueThread = QueueThread();
            drcsThread = DrcsThread();
#if PROFILE
            prof.PrintTime("EncodeServer D");
#endif
        }

        #region IDisposable Support
        private bool disposedValue = false; // 重複する呼び出しを検出するには

        protected virtual void Dispose(bool disposing)
        {
            if (!disposedValue)
            {
                disposedValue = true;

                if (disposing)
                {
                    // TODO: マネージ状態を破棄します (マネージ オブジェクト)。

                    // キュー状態を保存する
                    try
                    {
                        queueManager.SaveQueueData(false);
                    }
                    catch(Exception)
                    {
                        // Dispose中の例外は仕方ないので無視する
                    }

                    // 終了時にプロセスが残らないようにする
                    if (workerPool != null)
                    {
                        workerPool.SetNumParallel(0);
                        foreach (var worker in workerPool.Workers.Cast<TranscodeWorker>())
                        {
                            if (worker != null)
                            {
                                worker.CancelCurrentItem();
                            }
                        }
                    }

                    queueQ.Complete();
                    watchFileQ.Complete();
                    saveSettingQ.Complete();
                    drcsQ.Complete();
                    pauseScheduler.Complete();

                    if (settingUpdated)
                    {
                        settingUpdated = false;
                        try
                        {
                            SaveAppData();
                        }
                        catch (Exception)
                        {
                            // Dispose中の例外は仕方ないので無視する
                        }
                    }

                    if (uiStateUpdated)
                    {
                        uiStateUpdated = false;
                        try
                        {
                            SaveUIState();
                        }
                        catch (Exception)
                        {
                            // Dispose中の例外は仕方ないので無視する
                        }
                    }

                    if (autoSelectUpdated)
                    {
                        autoSelectUpdated = false;
                        try
                        {
                            SaveAutoSelectData();
                        }
                        catch (Exception)
                        {
                            // Dispose中の例外は仕方ないので無視する
                        }
                    }

                    if (preventSuspend != null)
                    {
                        preventSuspend.Dispose();
                        preventSuspend = null;
                    }

                    DeleteOldLogFile();
                }

                // TODO: アンマネージ リソース (アンマネージ オブジェクト) を解放し、下のファイナライザーをオーバーライドします。
                // TODO: 大きなフィールドを null に設定します。

            }
        }

        // TODO: 上の Dispose(bool disposing) にアンマネージ リソースを解放するコードが含まれる場合にのみ、ファイナライザーをオーバーライドします。
        // ~EncodeServer() {
        //   // このコードを変更しないでください。クリーンアップ コードを上の Dispose(bool disposing) に記述します。
        //   Dispose(false);
        // }

        // このコードは、破棄可能なパターンを正しく実装できるように追加されました。
        public void Dispose()
        {
            // このコードを変更しないでください。クリーンアップ コードを上の Dispose(bool disposing) に記述します。
            Dispose(true);
            // TODO: 上のファイナライザーがオーバーライドされる場合は、次の行のコメントを解除してください。
            // GC.SuppressFinalize(this);
        }
        #endregion

        #region Path
        private string GetSettingFilePath()
        {
            return "config\\AmatsukazeServer.xml";
        }

        private string GetUIStateFilePath()
        {
            return "config\\UIState.xml";
        }

        private string GetAutoSelectFilePath()
        {
            return "config\\AutoSelectProfile.xml";
        }

        internal string GetQueueFilePath()
        {
            return "data\\Queue.xml";
        }

        private string GetHistoryFilePathV1()
        {
            return "data\\EncodeHistory.xml";
        }

        private string GetHistoryFilePathV2()
        {
            return "data\\EncodeHistoryV2.xml";
        }

        internal string GetLogFileBase(DateTime start)
        {
            return "data\\logs\\" + start.ToString("yyyy-MM-dd_HHmmss.fff");
        }

        private string ReadLogFIle(DateTime start)
        {
            var logpath = GetLogFileBase(start) + ".txt";
            if (File.Exists(logpath) == false)
            {
                return "ログファイルが見つかりません。パス: " + logpath;
            }
            return File.ReadAllText(logpath, Encoding.Default);
        }

        private string GetCheckHistoryFilePath()
        {
            return "data\\CheckHistory.xml";
        }

        internal string GetCheckLogFileBase(DateTime start)
        {
            return "data\\checklogs\\" + start.ToString("yyyy-MM-dd_HHmmss.fff");
        }

        private string ReadCheckLogFIle(DateTime start)
        {
            var logpath = GetCheckLogFileBase(start) + ".txt";
            if (File.Exists(logpath) == false)
            {
                return "ログファイルが見つかりません。パス: " + logpath;
            }
            return File.ReadAllText(logpath, Encoding.Default);
        }

        private string GetLogoDirectoryPath()
        {
            return Path.GetFullPath("logo");
        }

        private string GetLogoFilePath(string fileName)
        {
            return GetLogoDirectoryPath() + "\\" + fileName;
        }

        private string GetJLDirectoryPath()
        {
            return Path.GetFullPath("JL");
        }

        internal string GetAvsDirectoryPath()
        {
            return Path.GetFullPath("avs");
        }

        internal string GetAvsCacheDirectoryPath()
        {
            return Path.GetFullPath("avscache");
        }

        internal string GetBatDirectoryPath()
        {
            return Path.GetFullPath("bat");
        }

        internal string GetDRCSDirectoryPath()
        {
            return Path.GetFullPath("drcs");
        }

        internal string GetSoundDirectoryPath()
        {
            return Path.GetFullPath("sound");
        }

        internal string GetDRCSImagePath(string md5)
        {
            return GetDRCSImagePath(GetDRCSDirectoryPath(), md5);
        }

        private string GetDRCSImagePath(string dirPath, string md5)
        {
            return dirPath + "\\" + md5 + ".bmp";
        }

        internal string GetDRCSMapPath()
        {
            return GetDRCSMapPath(GetDRCSDirectoryPath());
        }

        internal string GetDRCSMapPath(string dirPath)
        {
            return dirPath + "\\drcs_map.txt";
        }

        private string GetProfileDirectoryPath()
        {
            return Path.GetFullPath("profile");
        }

        private string GetProfilePath(string dirpath, string name)
        {
            return dirpath + "\\" + name + ".profile";
        }
        #endregion

        public void Finish()
        {
            // Finishはスタブの接続を切るためのインターフェースなので
            // サーバ本体では使われない
            throw new NotImplementedException();
        }

        private void SetScheduleParam(bool enable, int numGPU, int[] maxGPU)
        {
            ResourceManager.SetGPUResources(numGPU, maxGPU);
            scheduledQueue.SetGPUResources(numGPU, maxGPU);
            scheduledQueue.EnableResourceScheduling = enable;
        }

        private void UpdateFromVersion0()
        {
            // DRCS文字の並びを変更する //
            int NextVersion = 1;

            if (AppData_.Version < NextVersion)
            {
                drcsManager.UpdateFromOldVersion();

                // ログファイルを移行
                string path = GetHistoryFilePathV1();
                if (File.Exists(path))
                {
                    try
                    {
                        using (FileStream fs = new FileStream(path, FileMode.Open))
                        {
                            var s = new DataContractSerializer(typeof(LogData));
                            var data = (LogData)s.ReadObject(fs);
                            if (data.Items != null)
                            {
                                var file = new DataFile<LogItem>(GetHistoryFilePathV2());
                                file.Delete();
                                file.Add(data.Items);
                            }
                        }
                        File.Delete(path);
                    }
                    catch (IOException e)
                    {
                        Util.AddLog("ログファイルの移行に失敗", e);
                    }
                }

                // 現在バージョンに更新
                AppData_.Version = NextVersion;

                // 起動処理で落ちると２重に処理することになるので、
                // ここで設定ファイルに書き込んでおく
                SaveAppData();
            }
        }

        private void UpdateFromVersion1()
        {
            // uiStateを別ファイル化する //
            int NextVersion = 2;

            if (AppData_.Version < NextVersion)
            {
#pragma warning disable 0618
                UIState_ = AppData_.uiState;
                AppData_.uiState = null;
#pragma warning restore 0618
                SaveUIState();
                LoadUIState();

                // 現在バージョンに更新
                AppData_.Version = NextVersion;

                // 起動処理で落ちると２重に処理することになるので、
                // ここで設定ファイルに書き込んでおく
                SaveAppData();
            }
        }

        private void UpdateFromOldVersion()
        {
            // 古いバージョンからのアップデート処理
            UpdateFromVersion0();
            UpdateFromVersion1();
        }

        #region メッセージ出力
        private Task doNotify(int id, string message, Exception e, bool error, bool log)
        {
            if (log)
            {
                Util.AddLog(id, message, e);
            }
            return Client?.OnOperationResult(new OperationResult()
            {
                IsFailed = error,
                Message = Util.ErrorMessage(id, message, e),
                StackTrace = e?.StackTrace
            });
        }

        internal Task NotifyMessage(int id, string message, bool log)
        {
            return doNotify(id, message, null, false, log);
        }

        internal Task NotifyMessage(string message, bool log)
        {
            return doNotify(-1, message, null, false, log);
        }

        internal Task NotifyError(int id, string message, bool log)
        {
            return doNotify(id, message, null, true, log);
        }

        internal Task NotifyError(string message, bool log)
        {
            return doNotify(-1, message, null, true, log);
        }

        internal Task FatalError(int id, string message, Exception e)
        {
            return doNotify(id, message, e, true, true);
        }

        internal Task FatalError(string message, Exception e)
        {
            return doNotify(-1, message, e, true, true);
        }
        #endregion

        private static string GetExePath(string basePath, string pattern)
        {
            foreach (var path in Directory.GetFiles(basePath))
            {
                var fname = Path.GetFileName(path);
                if (fname.StartsWith(pattern) && fname.EndsWith(".exe"))
                {
                    return path;
                }
            }
            return null;
        }

        struct EncodeExeFileInfo
        {
            public string Path;
            public int[] version;
        }
        private static int[] GetEncoderExeVersionFromFilename(string filename, EncoderType type)
        {
            switch (type)
            {
                case EncoderType.x264:
                    // x264はx264_3186_x64.exeのような形式なので、このうち3186をintに変換して返す
                    var match = System.Text.RegularExpressions.Regex.Match(filename, @"x264_(\d+)_");
                    if (match.Success)
                    {
                        return new int[] { int.Parse(match.Groups[1].Value), 0, 0, 0 };
                    }
                    break;
                case EncoderType.x265:
                    // x265はx265_3.6+7_x64.exeのような形式なので、このうち、3, 6, 7をintに変換して配列で返す
                    match = System.Text.RegularExpressions.Regex.Match(filename, @"x265_(\d+)\.(\d+)\+(\d+)_");
                    if (match.Success)
                    {
                        return new int[] { int.Parse(match.Groups[1].Value), int.Parse(match.Groups[2].Value), 0, int.Parse(match.Groups[3].Value) };
                    }
                    break;
                case EncoderType.SVTAV1:
                    //svt-av1はSvtAv1EncApp_2.0.0-31_x64.exeのような形式なので、このうち、2, 0, 0, 31をintに変換して配列で返す
                    match = System.Text.RegularExpressions.Regex.Match(filename, @"SvtAv1EncApp_(\d+)\.(\d+)\.(\d+)-(\d+)_");
                    if (match.Success)
                    {
                        return new int[] { int.Parse(match.Groups[1].Value), int.Parse(match.Groups[2].Value), int.Parse(match.Groups[3].Value), int.Parse(match.Groups[4].Value) };
                    }
                    //svt-av1はSvtAv1EncApp_v1.8.0_x64のような形式もあるので、このうち、1, 8, 0, 0をintに変換して配列で返す
                    match = System.Text.RegularExpressions.Regex.Match(filename, @"SvtAv1EncApp_v(\d+)\.(\d+)\.(\d+)_");
                    if (match.Success)
                    {
                        return new int[] { int.Parse(match.Groups[1].Value), int.Parse(match.Groups[2].Value), int.Parse(match.Groups[3].Value), 0 };
                    }
                    break;
                default:
                    break;
            }
            return null;
        }

        private static string GetEncoderExePath(string basePath, EncoderType type)
        {
            string pattern = "";
            switch (type)
            {
                case EncoderType.x264:
                    pattern = "x264";
                    break;
                case EncoderType.x265:
                    pattern = "x265";
                    break;
                case EncoderType.SVTAV1:
                    pattern = "SvtAv1EncApp";
                    break;
                default:
                    return null;
            }
            var exeList = new List<EncodeExeFileInfo>();
            foreach (var path in Directory.GetFiles(basePath))
            {
                var fname = Path.GetFileName(path);
                if (fname.StartsWith(pattern) && fname.EndsWith(".exe"))
                {
                    // pathの「ファイル バージョン」を取得してリストに追加する
                    int[] version = null;
                    // versionはa.b.c.dの形式なので、a,b,c,dをintに変換して配列にする
                    try
                    {
                        var versionStr = FileVersionInfo.GetVersionInfo(path).FileVersion;
                        version = versionStr.Split('.').Select(int.Parse).ToArray();
                    }
                    catch (Exception)
                    {
                        // バージョン情報が取得できない場合はファイル名から取得
                        try
                        {
                            version = GetEncoderExeVersionFromFilename(fname, type);
                        }
                        catch (Exception)
                        { }
                    }
                    exeList.Add(new EncodeExeFileInfo() { Path = path, version = version });
                }
            }
            // exeList内のバージョン情報を比較して最新のものを返す
            var maxVersion = new int[] { 0, 0, 0, 0 };
            string maxPath = null;
            foreach (var exe in exeList)
            {
                if (exe.version == null)
                {
                    continue;
                }
                bool isNewer = false;
                for (int i = 0; i < 4; i++)
                {
                    if (exe.version[i] > maxVersion[i])
                    {
                        isNewer = true;
                        break;
                    }
                    else if (exe.version[i] < maxVersion[i])
                    {
                        break;
                    }
                }
                if (isNewer)
                {
                    maxVersion = exe.version;
                    maxPath = exe.Path;
                }
            }
            if (maxPath == null && exeList.Count > 0)
            {
                //nullの場合は、exeListの先頭のものを返す
                maxPath = exeList[0].Path;
            }
            return maxPath;
        }

        private Setting SetDefaultPath(Setting setting)
        {
            string basePath = Path.GetDirectoryName(GetType().Assembly.Location);
            if (string.IsNullOrEmpty(setting.AmatsukazePath))
            {
                setting.AmatsukazePath = Path.Combine(basePath, "AmatsukazeCLI.exe");
            }
            if (string.IsNullOrEmpty(setting.X264Path))
            {
                setting.X264Path = GetEncoderExePath(basePath, EncoderType.x264);
            }
            if (string.IsNullOrEmpty(setting.X265Path))
            {
                setting.X265Path = GetEncoderExePath(basePath, EncoderType.x265);
            }
            if (string.IsNullOrEmpty(setting.SVTAV1Path))
            {
                setting.SVTAV1Path = GetEncoderExePath(basePath, EncoderType.SVTAV1);
            }
            if (string.IsNullOrEmpty(setting.MuxerPath))
            {
                setting.MuxerPath = Path.Combine(basePath, "muxer.exe");
            }
            if (string.IsNullOrEmpty(setting.MKVMergePath))
            {
                setting.MKVMergePath = Path.Combine(basePath, "mkvmerge.exe");
            }
            if (string.IsNullOrEmpty(setting.MP4BoxPath))
            {
                setting.MP4BoxPath = Path.Combine(basePath, "mp4box.exe");
            }
            if (string.IsNullOrEmpty(setting.TimelineEditorPath))
            {
                setting.TimelineEditorPath = Path.Combine(basePath, "timelineeditor.exe");
            }
            if (string.IsNullOrEmpty(setting.ChapterExePath))
            {
                setting.ChapterExePath = GetExePath(basePath, "chapter_exe");
            }
            if (string.IsNullOrEmpty(setting.JoinLogoScpPath))
            {
                setting.JoinLogoScpPath = GetExePath(basePath, "join_logo_scp");
            }
            if (string.IsNullOrEmpty(setting.TsReplacePath))
            {
                setting.TsReplacePath = GetExePath(basePath, "tsreplace");
            }
            return setting;
        }

        private Setting GetDefaultSetting()
        {
            string basePath = Path.GetDirectoryName(GetType().Assembly.Location);
            return SetDefaultPath(new Setting() { NumParallel = 1, DeleteOldLogsDays = 180 });
        }



        private void LoadAppData()
        {
            string path = GetSettingFilePath();
            if (File.Exists(path) == false)
            {
                AppData_ = new AppData();
            }
            else
            {
                using (FileStream fs = new FileStream(path, FileMode.Open))
                {
                    var s = new DataContractSerializer(typeof(AppData));
                    AppData_ = (AppData)s.ReadObject(fs);
                }
            }
            if (AppData_.setting == null)
            {
                AppData_.setting = GetDefaultSetting();
            }
            if(string.IsNullOrWhiteSpace(AppData_.setting.WorkPath) ||
                Directory.Exists(AppData_.setting.WorkPath) == false)
            {
                // 一時フォルダにアクセスできないときは、デフォルト一時フォルダを設定
                AppData_.setting.WorkPath = Path.GetTempPath().TrimEnd(Path.DirectorySeparatorChar);
            }
            if (AppData_.setting.NumGPU == 0)
            {
                AppData_.setting.NumGPU = 1;
            }
            if (AppData_.setting.MaxGPUResources == null ||
                AppData_.setting.MaxGPUResources.Length < ResourceManager.MAX_GPU)
            {
                AppData_.setting.MaxGPUResources = Enumerable.Repeat(100, ResourceManager.MAX_GPU).ToArray();
            }
            if (AppData_.setting.RunHours == null)
            {
                AppData_.setting.RunHours = Enumerable.Repeat(true, 24).ToArray();
            }
            if (AppData_.setting.DeleteOldLogsDays < 0)
            {
                AppData_.setting.DeleteOldLogsDays = 180;
            }
            if (AppData_.scriptData == null)
            {
                AppData_.scriptData = new MakeScriptData();
            }
            if (AppData_.services == null)
            {
                AppData_.services = new ServiceSetting();
            }
            if (AppData_.services.ServiceMap == null)
            {
                AppData_.services.ServiceMap = new Dictionary<int, ServiceSettingElement>();
            }
            if(AppData_.finishSetting == null)
            {
                AppData_.finishSetting = new FinishSetting();
            }
            if(AppData_.finishSetting.Seconds <= 0)
            {
                AppData_.finishSetting.Seconds = 45;
            }
        }

        private void SaveAppData()
        {
            string path = GetSettingFilePath();
            Directory.CreateDirectory(Path.GetDirectoryName(path));
            using (FileStream fs = new FileStream(path, FileMode.Create))
            {
                var s = new DataContractSerializer(typeof(AppData));
                s.WriteObject(fs, AppData_);
            }
        }

        private void LoadUIState()
        {
            string path = GetUIStateFilePath();
            if (File.Exists(path) == false)
            {
                UIState_ = new  UIState();
            }
            else
            {
                using (FileStream fs = new FileStream(path, FileMode.Open))
                {
                    var s = new DataContractSerializer(typeof(UIState));
                    UIState_ = (UIState)s.ReadObject(fs);
                }
            }
            if(UIState_ == null)
            {
                UIState_ = new UIState();
            }
            if (UIState_.OutputPathHistory == null)
            {
                UIState_.OutputPathHistory = new List<string>();
            }
            OutPathHistory.Clear();
            foreach (var item in UIState_.OutputPathHistory)
            {
                OutPathHistory.Add(item);
            }
            UIState_.OutputPathHistory = OutPathHistory.ToList();
        }

        private void SaveUIState()
        {
            string path = GetUIStateFilePath();
            Directory.CreateDirectory(Path.GetDirectoryName(path));
            using (FileStream fs = new FileStream(path, FileMode.Create))
            {
                var s = new DataContractSerializer(typeof(UIState));
                s.WriteObject(fs, UIState_);
            }
        }

        [DataContract]
        public class AutoSelectData
        {
            [DataMember]
            public List<AutoSelectProfile> Profiles { get; set; }
        }

        private void LoadAutoSelectData()
        {
            string path = GetAutoSelectFilePath();
            if (File.Exists(path) == false)
            {
                return;
            }
            using (FileStream fs = new FileStream(path, FileMode.Open))
            {
                var s = new DataContractSerializer(typeof(AutoSelectData));
                try
                {
                    var data = (AutoSelectData)s.ReadObject(fs);
                    autoSelects.Clear();
                    foreach (var profile in data.Profiles)
                    {
                        foreach (var cond in profile.Conditions)
                        {
                            if (cond.ContentConditions == null)
                            {
                                cond.ContentConditions = new List<GenreItem>();
                            }
                            if (cond.ServiceIds == null)
                            {
                                cond.ServiceIds = new List<int>();
                            }
                            if (cond.VideoSizes == null)
                            {
                                cond.VideoSizes = new List<VideoSizeCondition>();
                            }
                        }
                        autoSelects.Add(profile.Name, profile);
                    }
                }
                catch(Exception e)
                {
                    FatalError("自動選択プロファイルを読み込めませんでした", e);
                }
            }
        }

        private void SaveAutoSelectData()
        {
            string path = GetAutoSelectFilePath();
            Directory.CreateDirectory(Path.GetDirectoryName(path));
            using (FileStream fs = new FileStream(path, FileMode.Create))
            {
                var s = new DataContractSerializer(typeof(AutoSelectData));
                s.WriteObject(fs, new AutoSelectData()
                {
                    Profiles = autoSelects.Values.ToList()
                });
            }
        }

        private ProfileSetting ReadProfile(string filepath)
        {
            using (FileStream fs = new FileStream(filepath, FileMode.Open))
            {
                var s = new DataContractSerializer(typeof(ProfileSetting));
                var profile = (ProfileSetting)s.ReadObject(fs);
                return ServerSupport.NormalizeProfile(profile);
            }
        }

        private void SaveProfile(string filepath, ProfileSetting profile)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(filepath));
            using (FileStream fs = new FileStream(filepath, FileMode.Create))
            {
                var s = new DataContractSerializer(typeof(ProfileSetting));
                s.WriteObject(fs, profile);
            }
        }

        internal Task AddEncodeLog(LogItem item)
        {
            try
            {
                logData.Items.Add(item);
                logFile.Add(new List<LogItem>() { item });
                drcsManager.AddLogFile(GetLogFileBase(item.EncodeStartDate) + ".txt",
                    item.SrcPath, item.EncodeFinishDate);
                return Client.OnUIData(new UIData()
                {
                    LogItem = item
                });
            }
            catch (IOException e)
            {
                Util.AddLog("ログファイル書き込み失敗", e);
            }
            return Task.FromResult(0);
        }

        internal Task AddCheckLog(CheckLogItem item)
        {
            try
            {
                checkLogData.Items.Add(item);
                checkLogFile.Add(new List<CheckLogItem>() { item });
                drcsManager.AddLogFile(GetCheckLogFileBase(item.CheckStartDate) + ".txt",
                    item.SrcPath, item.CheckStartDate);
                return Client.OnUIData(new UIData()
                {
                    CheckLogItem = item
                });
            }
            catch (IOException e)
            {
                Util.AddLog("ログファイル書き込み失敗", e);
            }
            return Task.FromResult(0);
        }

        private void DeleteOldLogFile()
        {
            if (!AppData_.setting.DeleteOldLogs) return;

            // 現在の日付を取得する
            var now = DateTime.Now.Date;

            for (int i = logData.Items.Count - 1; i >= 0; i--)
            {
                if (logData.Items[i].EncodeStartDate.Date <= now.AddDays(-AppData_.setting.DeleteOldLogsDays))
                {
                    string logpath = GetLogFileBase(logData.Items[i].EncodeStartDate) + ".txt";
                    if (File.Exists(logpath))
                    {
                        try
                        {
                            File.Delete(logpath);
                        }
                        catch (Exception e)
                        {
                            Util.AddLog("ログファイル " + logpath + " の削除に失敗", e);
                        }
                    }
                    string jsonpath = GetLogFileBase(logData.Items[i].EncodeStartDate) + ".json";
                    if (File.Exists(jsonpath))
                    {
                        try
                        {
                            File.Delete(jsonpath);
                        }
                        catch (Exception e)
                        {
                            Util.AddLog("ログファイル " + jsonpath + " の削除に失敗", e);
                        }
                    }
                    logData.Items.RemoveAt(i);
                }
            }
            // 現在のリストを上書き保存する
            logFile.Save(logData.Items);

            for (int i = checkLogData.Items.Count - 1; i >= 0; i--)
            {
                if (checkLogData.Items[i].CheckStartDate.Date <= now.AddDays(-AppData_.setting.DeleteOldLogsDays))
                {
                    string logpath = GetCheckLogFileBase(checkLogData.Items[i].CheckStartDate) + ".txt";
                    if (File.Exists(logpath))
                    {
                        try
                        {
                            File.Delete(logpath);
                        }
                        catch (Exception e)
                        {
                            Util.AddLog("ログファイル " + logpath + " の削除に失敗", e);
                        }
                    }
                    checkLogData.Items.RemoveAt(i);
                }
            }
            // 現在のリストを上書き保存する
            checkLogFile.Save(checkLogData.Items);
        }

        private static string GetEncoderPath(EncoderType encoderType, Setting setting)
        {
            if (encoderType == EncoderType.x264)
            {
                return setting.X264Path;
            }
            else if (encoderType == EncoderType.x265)
            {
                return setting.X265Path;
            }
            else if (encoderType == EncoderType.QSVEnc)
            {
                return setting.QSVEncPath;
            }
            else if (encoderType == EncoderType.NVEnc)
            {
                return setting.NVEncPath;
            }
            else if (encoderType == EncoderType.VCEEnc)
            {
                return setting.VCEEncPath;
            }
            else
            {
                return setting.SVTAV1Path;
            }
        }

        private string GetEncoderOption(ProfileSetting profile)
        {
            if (profile.EncoderType == EncoderType.x264)
            {
                return profile.X264Option;
            }
            else if (profile.EncoderType == EncoderType.x265)
            {
                return profile.X265Option;
            }
            else if (profile.EncoderType == EncoderType.QSVEnc)
            {
                return profile.QSVEncOption;
            }
            else if (profile.EncoderType == EncoderType.NVEnc)
            {
                return profile.NVEncOption;
            }
            else if (profile.EncoderType == EncoderType.VCEEnc)
            {
                return profile.VCEEncOption;
            }
            else
            {
                return profile.SVTAV1Option;
            }
        }

        private string GetEncoderName(EncoderType encoderType)
        {
            if (encoderType == EncoderType.x264)
            {
                return "x264";
            }
            else if (encoderType == EncoderType.x265)
            {
                return "x265";
            }
            else if (encoderType == EncoderType.QSVEnc)
            {
                return "QSVEnc";
            }
            else if (encoderType == EncoderType.NVEnc)
            {
                return "NVEnc";
            }
            else if (encoderType == EncoderType.VCEEnc)
            {
                return "VCEEnc";
            }
            else
            {
                return "SVT-AV1";
            }
        }

        private static string GetAudioEncoderPath(AudioEncoderType encoderType, Setting setting)
        {
            if (encoderType == AudioEncoderType.NeroAac)
            {
                return setting.NeroAacEncPath;
            }
            else if (encoderType == AudioEncoderType.Qaac)
            {
                return setting.QaacPath;
            }
            else if (encoderType == AudioEncoderType.Fdkaac)
            {
                return setting.FdkaacPath;
            }
            else
            {
                return setting.OpusEncPath;
            }
        }

        private string GetAudioEncoderOption(ProfileSetting profile)
        {
            if (profile.AudioEncoderType == AudioEncoderType.NeroAac)
            {
                return profile.NeroAacOption;
            }
            else if (profile.AudioEncoderType == AudioEncoderType.Qaac)
            {
                return profile.QaacOption;
            }
            else if (profile.AudioEncoderType == AudioEncoderType.Fdkaac)
            {
                return profile.FdkaacOption;
            }
            else
            {
                return profile.OpusEncOption;
            }
        }

        private string GetAudioEncoderName(AudioEncoderType encoderType)
        {
            if (encoderType == AudioEncoderType.NeroAac)
            {
                return "neroAac";
            }
            else if (encoderType == AudioEncoderType.Qaac)
            {
                return "qaac";
            }
            else if (encoderType == AudioEncoderType.Fdkaac)
            {
                return "fdkaac";
            }
            else
            {
                return "opusenc";
            }
        }

        internal string MakeAmatsukazeArgs(
            ProcMode mode,
            ProfileSetting profile,
            Setting setting,
            bool isGeneric,
            string src, string srcOrg, string dst, string json,
            VideoStreamFormat streamFormat,
            int serviceId, string[] logofiles,
            bool ignoreNoLogo, string jlscommand, string jlsopt, string ceopt, string trimavs,
            string inHandle, string outHandle, int pid)
        {
            StringBuilder sb = new StringBuilder();

            bool loadV2 = false;
            if (   streamFormat != VideoStreamFormat.MPEG2
                && streamFormat != VideoStreamFormat.H264)
            {
                sb.Append(" --loadv2");
                loadV2 = true;
            }

            if (mode == ProcMode.CMCheck)
            {
                sb.Append(" --mode cm");
            }
            else if(mode == ProcMode.DrcsCheck)
            {
                sb.Append(" --mode drcs");
            }
            else if (isGeneric)
            {
                sb.Append(" --mode g");
            }

            if(setting.DumpFilter)
            {
                sb.Append(" --dump-filter");
            }

            if(setting.PrintTimePrefix)
            {
                sb.Append(" --print-prefix time");
            }

            sb.Append(" -i \"")
                .Append(src)
                .Append("\" -s ")
                .Append(serviceId)
                .Append(" --drcs \"")
                .Append(GetDRCSMapPath())
                .Append("\"");

            if (srcOrg != null)
            {
                sb.Append(" --original-input-file \"").Append(srcOrg).Append("\"");
            }

            if(inHandle != null)
            {
                sb.Append(" --resource-manager ")
                    .Append(inHandle)
                    .Append(':')
                    .Append(outHandle);
            }

            // スケジューリングが有効な場合はエンコード時にアフィニティを設定するので
            // ここでは設定しない
            if (setting.SchedulingEnabled == false &&
                setting.AffinitySetting != (int)ProcessGroupKind.None)
            {
                var mask = affinityCreator.GetMask(
                    (ProcessGroupKind)setting.AffinitySetting, pid);
                sb.Append(" --affinity ")
                    .Append(mask.Group)
                    .Append(':')
                    .Append(mask.Mask);
            }

            if (mode == ProcMode.DrcsCheck)
            {
                sb.Append(" --subtitles");
            }
            else {
                int outputMask = profile.OutputMask;
                if (outputMask == 0)
                {
                    outputMask = 1;
                }

                sb.Append(" -w \"")
                    .Append(setting.WorkPath)
                    .Append("\" --chapter-exe \"")
                    .Append(setting.ChapterExePath)
                    .Append("\" --jls \"")
                    .Append(setting.JoinLogoScpPath)
                    .Append("\" --cmoutmask ")
                    .Append(outputMask);


                if (mode == ProcMode.CMCheck)
                {
                    sb.Append(" --chapter");
                }
                else {
                    double bitrateCM = profile.BitrateCM;
                    if (bitrateCM == 0)
                    {
                        bitrateCM = 1;
                    }

                    sb.Append(" -o \"")
                        .Append(dst)
                        .Append("\" -et ")
                        .Append(GetEncoderName(profile.EncoderType))
                        .Append(" -e \"")
                        .Append(GetEncoderPath(profile.EncoderType, setting))
                        .Append("\" -j \"")
                        .Append(json)
                        .Append("\"");

                    if (profile.EncoderType == EncoderType.SVTAV1
                        && profile.ForceSAR
                        && profile.ForceSARWidth > 0 && profile.ForceSARHeight > 0)
                    {
                        sb.Append(" --sar ")
                            .Append(profile.ForceSARWidth)
                            .Append(':')
                            .Append(profile.ForceSARHeight);
                    }

                    if (profile.OutputFormat == FormatType.MP4 || profile.OutputFormat == FormatType.TSREPLACE)
                    {
                        sb.Append(" --mp4box \"")
                            .Append(setting.MP4BoxPath)
                            .Append("\" -t \"")
                            .Append(setting.TimelineEditorPath)
                            .Append("\"");
                    }

                    var encoderOption = GetEncoderOption(profile);
                    if (string.IsNullOrEmpty(encoderOption) == false)
                    {
                        sb.Append(" -eo \"")
                            .Append(encoderOption)
                            .Append("\"");
                    }

                    if (profile.OutputFormat == FormatType.MP4)
                    {
                        sb.Append(" -fmt mp4 -m \"" + setting.MuxerPath + "\"");
                    }
                    else if (profile.OutputFormat == FormatType.MKV)
                    {
                        sb.Append(" -fmt mkv -m \"" + setting.MKVMergePath + "\"");
                    }
                    else if (profile.OutputFormat == FormatType.M2TS)
                    {
                        sb.Append(" -fmt m2ts -m \"" + setting.TsMuxeRPath + "\"");
                    }
                    else if (profile.OutputFormat == FormatType.TS)
                    {
                        sb.Append(" -fmt ts -m \"" + setting.TsMuxeRPath + "\"");
                    }
                    else if (profile.OutputFormat == FormatType.TSREPLACE)
                    {
                        sb.Append(" -fmt tsreplace -m \"" + setting.TsReplacePath + "\"");
                    }
                    if (profile.OutputFormat == FormatType.MP4 && profile.UseMKVWhenSubExists)
                    {
                        sb.Append(" --use-mkv-when-sub-exists")
                            .Append(" --mkvmerge \"")
                            .Append(setting.MKVMergePath)
                            .Append("\"");
                    }
                    if (profile.OutputFormat == FormatType.TSREPLACE && profile.TsreplaceRemoveTypeD)
                    {
                        sb.Append(" --tsreplace-remove-typed");
                    }

                    if (bitrateCM != 1)
                    {
                        sb.Append(" -bcm ").Append(bitrateCM);
                    }
                    if (profile.CMQualityOffset != 0)
                    {
                        sb.Append(" --cm-quality-offset ").Append(profile.CMQualityOffset);
                    }
                    if (setting.EnableX265VFRTimeFactor &&
                        (profile.EncoderType == EncoderType.x265 || profile.EncoderType == EncoderType.NVEnc))
                    {
                        sb.Append(" --timefactor ")
                            .Append(setting.X265VFRTimeFactor.ToString("N2"));
                    }
                    if(profile.NumEncodeBufferFrames > 0)
                    {
                        sb.Append(" -eb ").Append(profile.NumEncodeBufferFrames);
                    }
                    if (profile.SplitSub)
                    {
                        sb.Append(" --splitsub");
                    }
                    if (!profile.DisableChapter)
                    {
                        sb.Append(" --chapter");
                    }
                    if (profile.EnableNicoJK)
                    {
                        sb.Append(" --nicojk");
                        if (profile.IgnoreNicoJKError)
                        {
                            sb.Append(" --ignore-nicojk-error");
                        }
                        if (profile.NicoJK18)
                        {
                            sb.Append(" --nicojk18");
                        }
                        if (profile.NicoJKLog)
                        {
                            sb.Append(" --nicojklog");
                        }
                        sb.Append(" --nicojkmask ")
                            .Append(profile.NicoJKFormatMask);
                        sb.Append(" --nicoass \"")
                            .Append(setting.NicoConvASSPath)
                            .Append("\"");
                    }

                    if(profile.FilterOption == FilterOption.Setting)
                    {
                        sb.Append(" -f \"")
                            .Append(CachedAvsScript.GetAvsFilePath(
                                profile.FilterSetting, setting, GetAvsCacheDirectoryPath()))
                            .Append("\"");
                    }
                    else if(profile.FilterOption == FilterOption.Custom)
                    {
                        if (string.IsNullOrEmpty(profile.FilterPath) == false)
                        {
                            sb.Append(" -f \"")
                                .Append(GetAvsDirectoryPath() + "\\" + profile.FilterPath)
                                .Append("\"");
                        }

                        if (string.IsNullOrEmpty(profile.PostFilterPath) == false)
                        {
                            sb.Append(" -pf \"")
                                .Append(GetAvsDirectoryPath() + "\\" + profile.PostFilterPath)
                                .Append("\"");
                        }
                    }

                    if (profile.AutoBuffer)
                    {
                        sb.Append(" --bitrate ")
                            .Append(profile.Bitrate.A)
                            .Append(":")
                            .Append(profile.Bitrate.B)
                            .Append(":")
                            .Append(profile.Bitrate.H264);
                    }

                    if (profile.TwoPass)
                    {
                        sb.Append(" --2pass");
                    }

                    if(string.IsNullOrEmpty(profile.AdditionalEraseLogo) == false)
                    {
                        foreach (var logo in profile.AdditionalEraseLogo.Split(
                            new string[] { Environment.NewLine }, StringSplitOptions.RemoveEmptyEntries))
                        {
                            sb.Append(" --erase-logo \"")
                                .Append(logo)
                                .Append("\"");
                        }
                    }

                    if (profile.EnableAudioEncode)
                    {
                        sb.Append(" -aet ")
                            .Append(GetAudioEncoderName(profile.AudioEncoderType))
                            .Append(" -ae \"")
                            .Append(GetAudioEncoderPath(profile.AudioEncoderType, setting))
                            .Append("\"");

                        var audioEncoderOption = GetAudioEncoderOption(profile);
                        if (string.IsNullOrEmpty(audioEncoderOption) == false)
                        {
                            sb.Append(" -aeo \"")
                                .Append(audioEncoderOption)
                                .Append("\"");
                        }
                        if (profile.EnableAudioBitrate)
                        {
                            sb.Append(" -ab ").Append(profile.AudioBitrateInKbps);
                        }

                    }
                } // if (mode != ProcMode.CMCheck)

                if (!profile.DisableSubs)
                {
                    sb.Append(" --subtitles");
                }

                if (string.IsNullOrEmpty(jlscommand) == false)
                {
                    sb.Append(" --jls-cmd \"")
                        .Append(GetJLDirectoryPath() + "\\" + jlscommand)
                        .Append("\"");
                }
                if (string.IsNullOrEmpty(jlsopt) == false)
                {
                    sb.Append(" --jls-option \"")
                        .Append(jlsopt)
                        .Append("\"");
                }
                if (string.IsNullOrEmpty(ceopt) == false)
                {
                    sb.Append(" --chapter-exe-options \"")
                        .Append(ceopt)
                        .Append("\"");
                }

                string[] decoderNames = new string[] { "default", "QSV", "CUVID" };
                if (!loadV2)
                {
                    decoderNames[1] = "default";
                }
                if (profile.Mpeg2Decoder != DecoderType.Default)
                {
                    sb.Append("  --mpeg2decoder ");
                    sb.Append(decoderNames[(int)profile.Mpeg2Decoder]);
                }

                if (profile.H264Deocder != DecoderType.Default)
                {
                    sb.Append("  --h264decoder ");
                    sb.Append(decoderNames[(int)profile.H264Deocder]);
                }

                if (profile.HEVCDecoder != DecoderType.Default)
                {
                    sb.Append("  --hevcdecoder ");
                    sb.Append(decoderNames[(int)profile.HEVCDecoder]);
                }
                if (ignoreNoLogo)
                {
                    sb.Append(" --ignore-no-logo");
                }
                if (profile.LooseLogoDetection)
                {
                    sb.Append(" --loose-logo-detection");
                }
                if (profile.IgnoreNoDrcsMap)
                {
                    sb.Append(" --ignore-no-drcsmap");
                }
                if (profile.NoDelogo)
                {
                    sb.Append(" --no-delogo");
                }
                if (profile.ParallelLogoAnalysis)
                {
                    sb.Append(" --parallel-logo-analysis");
                }
                if (profile.NoRemoveTmp)
                {
                    sb.Append(" --no-remove-tmp");
                }
                if(profile.EnablePmtCut)
                {
                    sb.Append(" --pmt-cut ")
                        .Append(profile.PmtCutHeadRate / 100)
                        .Append(":")
                        .Append(profile.PmtCutTailRate / 100);
                }
                if (profile.EnableMaxFadeLength)
                {
                    sb.Append(" --max-fade-length ").Append(profile.MaxFadeLength);
                }
                if (string.IsNullOrEmpty(trimavs) == false)
                {
                    sb.Append(" --trimavs \"").Append(trimavs).Append("\"");
                }

                if (logofiles != null)
                {
                    foreach (var logo in logofiles)
                    {
                        sb.Append(" --logo \"").Append(GetLogoFilePath(logo)).Append("\"");
                    }
                }
                if (profile.SystemAviSynthPlugin)
                {
                    sb.Append(" --systemavsplugin");
                }
            }

            return sb.ToString();
        }

        public Task ClientQueueUpdate(QueueUpdate update)
        {
            return Client.OnUIData(new UIData()
            {
                QueueUpdate = update
            });
        }

        private void CleanTmpDir()
        {
            // amtディレクトリ
            foreach (var dir in Directory
                .GetDirectories(AppData_.setting.ActualWorkPath, "amt*"))
            {
                try
                {
                    Directory.Delete(dir, true);
                }
                catch (Exception) { } // 例外は無視
            }
            // amtファイル
            foreach (var file in Directory
                .GetFiles(AppData_.setting.ActualWorkPath, "amt*"))
            {
                try
                {
                    File.Delete(file);
                }
                catch (Exception) { } // 例外は無視
            }
            // slimtsファイル
            foreach (var file in Directory
                .GetFiles(AppData_.setting.ActualWorkPath, "slimts*.ts"))
            {
                try
                {
                    // 拡張子なしファイルがある場合は使用中なので除く
                    var meta = AppData_.setting.ActualWorkPath + "\\" + Path.GetFileNameWithoutExtension(file);
                    if(!File.Exists(meta))
                    {
                        File.Delete(file);
                    }
                }
                catch (Exception) { } // 例外は無視
            }
        }

        private static void CheckPath(string name, string path)
        {
            if(!string.IsNullOrEmpty(path) && !File.Exists(path))
            {
                throw new InvalidOperationException(name + "パスが無効です: " + path);
            }
        }

        private static void CheckSetting(ProfileSetting profile, Setting setting)
        {
            if (!File.Exists(setting.AmatsukazePath))
            {
                throw new InvalidOperationException(
                    "AmtasukazeCLIパスが無効です: " + setting.AmatsukazePath);
            }

            if(setting.WorkPath.EndsWith(Path.DirectorySeparatorChar.ToString()))
            {
                setting.WorkPath = setting.WorkPath.TrimEnd(Path.DirectorySeparatorChar);
            }
            string workPath = setting.ActualWorkPath;
            if (!Directory.Exists(workPath))
            {
                throw new InvalidOperationException(
                    "一時フォルダパスが無効です: " + workPath);
            }

            // パスが設定されていたらファイル存在チェック
            CheckPath("x264", setting.X264Path);
            CheckPath("x265", setting.X265Path);
            CheckPath("QSVEnc", setting.QSVEncPath);
            CheckPath("NVEnc", setting.NVEncPath);
            CheckPath("VCEEnc", setting.VCEEncPath);
            CheckPath("SVTAV1", setting.SVTAV1Path);

            CheckPath("L-SMASH Muxer", setting.MuxerPath);
            CheckPath("MP4Box", setting.MP4BoxPath);
            CheckPath("TimelineEditor", setting.TimelineEditorPath);
            CheckPath("MKVMerge", setting.MKVMergePath);
            CheckPath("ChapterExe", setting.ChapterExePath);
            CheckPath("JoinLogoScp", setting.JoinLogoScpPath);
            CheckPath("NicoConvAss", setting.NicoConvASSPath);
            CheckPath("tsMuxeR", setting.TsMuxeRPath);
            CheckPath("SCRename.vbs", setting.SCRenamePath);
            CheckPath("AutoVfr.exe", setting.AutoVfrPath);

            CheckPath("neroAacEnc", setting.NeroAacEncPath);
            CheckPath("qaac", setting.QaacPath);
            CheckPath("fdkaac", setting.FdkaacPath);

            if (profile != null)
            {
                string encoderPath = GetEncoderPath(profile.EncoderType, setting);
                if (string.IsNullOrEmpty(encoderPath))
                {
                    throw new ArgumentException("エンコーダパスが設定されていません");
                }

                if (profile.OutputFormat == FormatType.MP4)
                {
                    if (string.IsNullOrEmpty(setting.MuxerPath))
                    {
                        throw new ArgumentException("L-SMASH Muxerパスが設定されていません");
                    }
                    if (string.IsNullOrEmpty(setting.MP4BoxPath))
                    {
                        throw new ArgumentException("MP4Boxパスが指定されていません");
                    }
                    if (string.IsNullOrEmpty(setting.TimelineEditorPath))
                    {
                        throw new ArgumentException("Timelineeditorパスが設定されていません");
                    }
                }
                else if(profile.OutputFormat == FormatType.MKV)
                {
                    if (string.IsNullOrEmpty(setting.MKVMergePath))
                    {
                        throw new ArgumentException("MKVMergeパスが設定されていません");
                    }
                }
                else if (profile.OutputFormat == FormatType.TSREPLACE)
                {
                    if (string.IsNullOrEmpty(setting.TsReplacePath))
                    {
                        throw new ArgumentException("tsreplaceパスが設定されていません");
                    }
                    if (profile.EncoderType == EncoderType.SVTAV1)
                    {
                        throw new ArgumentException("TS (replace)使用時は、SVT-AV1は使用できません。");
                    }
                }
                else if (profile.OutputFormat == FormatType.TS || profile.OutputFormat == FormatType.M2TS)
                {
                    if (string.IsNullOrEmpty(setting.TsMuxeRPath))
                    {
                        throw new ArgumentException("tsMuxeRパスが設定されていません");
                    }
                }

                if (!profile.DisableChapter)
                {
                    if (string.IsNullOrEmpty(setting.ChapterExePath))
                    {
                        throw new ArgumentException("ChapterExeパスが設定されていません");
                    }
                    if (string.IsNullOrEmpty(setting.JoinLogoScpPath))
                    {
                        throw new ArgumentException("JoinLogoScpパスが設定されていません");
                    }
                }

                if (profile.EnableNicoJK)
                {
                    if (string.IsNullOrEmpty(setting.NicoConvASSPath))
                    {
                        throw new ArgumentException("NicoConvASSパスが設定されていません");
                    }
                }

                if (profile.EnableRename)
                {
                    if (string.IsNullOrEmpty(setting.SCRenamePath))
                    {
                        throw new ArgumentException("SCRenameパスが設定されていません");
                    }
                    var fileName = Path.GetFileName(setting.SCRenamePath);
                    // 間違える人がいるかも知れないので一応チェックしておく
                    if(fileName.Equals("SCRename.bat", StringComparison.OrdinalIgnoreCase) ||
                        fileName.Equals("SCRenameEDCB.bat", StringComparison.OrdinalIgnoreCase))
                    {
                        throw new ArgumentException("SCRenameはbatファイルではなくvbsファイルへのパスを設定してください");
                    }
                    if (string.IsNullOrEmpty(profile.RenameFormat))
                    {
                        throw new ArgumentException("リネームフォーマットが設定されていません");
                    }
                }

                if(profile.FilterOption == FilterOption.Setting &&
                    profile.FilterSetting.EnableDeinterlace &&
                    profile.FilterSetting.DeinterlaceAlgorithm == DeinterlaceAlgorithm.AutoVfr)
                {
                    if(string.IsNullOrEmpty(setting.AutoVfrPath))
                    {
                        throw new ArgumentException("AutoVfr.exeパスが設定されていません");
                    }
                }

                if (profile.EnableAudioEncode)
                {
                    string audioEncoderPath = GetAudioEncoderPath(profile.AudioEncoderType, setting);
                    if (string.IsNullOrEmpty(audioEncoderPath))
                    {
                        throw new ArgumentException("音声エンコーダパスが設定されていません");
                    }
                }
            }
        }

        private void CheckAutoSelect(AutoSelectProfile profile)
        {
            foreach(var cond in profile.Conditions)
            {
                if(cond.Profile != null)
                {
                    if(!profiles.ContainsKey(cond.Profile))
                    {
                        throw new ArgumentException("プロファイル「" + cond.Profile + "」がありません");
                    }
                }
            }
        }

        private async Task QueueThread()
        {
            try
            {
                while (await queueQ.OutputAvailableAsync())
                {
                    string requestId = null;
                    var req = await queueQ.ReceiveAsync();
                    if (req.GetType() == typeof(AddQueueRequest))
                    {
                        AddQueueRequest reqAddQueue = req as AddQueueRequest;
                        await queueManager.AddQueue(reqAddQueue);
                        requestId = reqAddQueue.RequestId;
                    }
                    else if (req.GetType() == typeof(ChangeItemData))
                    {
                        ChangeItemData reqChangeItem = req as ChangeItemData;
                        await ChangeItem(reqChangeItem);
                        requestId = reqChangeItem.RequestId;
                    }
                    await Client.OnAddResult(requestId);
                }
            }
            catch (Exception exception)
            {
                await FatalError("QueueThreadがエラー終了しました", exception);
            }
        }

        internal class ProfileTuple
        {
            public ProfileSetting Profile;
            public int Priority;
        }

        internal ProfileTuple GetProfile(List<string> tags, string fileName, int width, int height,
            List<GenreItem> genre, int serviceId, string profileName)
        {
            bool isAuto = false;
            int resolvedPriority = 0;
            profileName = ServerSupport.ParseProfileName(profileName, out isAuto);
            if (isAuto)
            {
                if (autoSelects.ContainsKey(profileName) == false)
                {
                    return null;
                }
                if(serviceId == -1)
                {
                    // TS情報がない
                    return null;
                }
                var resolvedProfile = ServerSupport.AutoSelectProfile(tags, fileName, width, height,
                    genre, serviceId, autoSelects[profileName], out resolvedPriority);
                if (resolvedProfile == null)
                {
                    return null;
                }
                profileName = resolvedProfile;
            }
            if (profiles.ContainsKey(profileName) == false)
            {
                return null;
            }
            return new ProfileTuple()
            {
                Profile = profiles[profileName],
                Priority = resolvedPriority
            };
        }

        internal ProfileTuple GetProfile(QueueItem item, string profileName)
        {
            return GetProfile(item.Tags, Path.GetFileName(item.SrcPath), item.ImageWidth, item.ImageHeight,
                item.Genre, item.ServiceId, profileName);
        }

        private bool ReadLogoFile(LogoSetting setting, string filepath)
        {
            try
            {
                var logo = new LogoFile(amtcontext, filepath);

                setting.FileName = Path.GetFileName(filepath);
                setting.LogoName = logo.Name;
                setting.ServiceId = logo.ServiceId;

                return true;
            }
            catch(IOException)
            {
                return false;
            }
        }

        private async Task WatchFileThread()
        {
            try
            {
                var completion = watchFileQ.OutputAvailableAsync();

                var logoDirTime = DateTime.MinValue;
                var logoTime = new Dictionary<string,DateTime>();

                var jlsDirTime = DateTime.MinValue;
                var avsDirTime = DateTime.MinValue;
                var batDirTime = DateTime.MinValue;
                var profileDirTime = DateTime.MinValue;

                // 初期化
                foreach (var service in AppData_.services.ServiceMap.Values)
                {
                    foreach (var logo in service.LogoSettings)
                    {
                        // 全てのロゴは存在しないところからスタート
                        logo.Exists = (logo.FileName == LogoSetting.NO_LOGO);
                    }
                }

                while (true)
                {
                    string logopath = GetLogoDirectoryPath();
                    if (Directory.Exists(logopath))
                    {
                        var map = AppData_.services.ServiceMap;

                        var logoDict = new Dictionary<string, LogoSetting>();
                        foreach (var service in map.Values)
                        {
                            foreach (var logo in service.LogoSettings)
                            {
                                if(logo.FileName != LogoSetting.NO_LOGO)
                                {
                                    logoDict.Add(logo.FileName, logo);
                                }
                            }
                        }

                        var updatedServices = new List<int>();

                        var lastModified = Directory.GetLastWriteTime(logopath);
                        if (logoDirTime != lastModified || serviceListUpdated)
                        {
                            logoDirTime = lastModified;

                            // ファイルの個数が変わった or サービスリストが変わった

                            if (serviceListUpdated)
                            {
                                // サービスリストが変わってたら再度追加処理
                                logoTime.Clear();
                            }

                            var newTime = new Dictionary<string, DateTime>();
                            foreach (var filepath in Directory.GetFiles(logopath)
                                .Where(s => s.EndsWith(".lgd", StringComparison.OrdinalIgnoreCase)))
                            {
                                newTime.Add(filepath, File.GetLastWriteTime(filepath));
                            }

                            foreach (var path in logoTime.Keys.Union(newTime.Keys))
                            {
                                var name = Path.GetFileName(path);
                                if (!newTime.ContainsKey(path))
                                {
                                    // 消えた
                                    if (logoDict.ContainsKey(name))
                                    {
                                        logoDict[name].Exists = false;
                                        updatedServices.Add(logoDict[name].ServiceId);
                                    }
                                }
                                else if (!logoTime.ContainsKey(path))
                                {
                                    // 追加された
                                    if (logoDict.ContainsKey(name))
                                    {
                                        if (logoDict[name].Exists == false)
                                        {
                                            logoDict[name].Exists = true;
                                            ReadLogoFile(logoDict[name], path);
                                            updatedServices.Add(logoDict[name].ServiceId);
                                        }
                                    }
                                    else
                                    {
                                        var setting = new LogoSetting();
                                        ReadLogoFile(setting, path);

                                        if (map.ContainsKey(setting.ServiceId))
                                        {
                                            setting.Exists = true;
                                            setting.Enabled = true;
                                            setting.From = new DateTime(2000, 1, 1);
                                            setting.To = new DateTime(2030, 12, 31);

                                            map[setting.ServiceId].LogoSettings.Add(setting);
                                            updatedServices.Add(setting.ServiceId);
                                        }
                                    }
                                }
                                else if (logoTime[path] != newTime[path])
                                {
                                    // 変更されたファイル
                                    if (logoDict.ContainsKey(name))
                                    {
                                        ReadLogoFile(logoDict[name], path);
                                        updatedServices.Add(logoDict[name].ServiceId);
                                    }
                                }
                            }

                            logoTime = newTime;
                        }
                        else
                        {
                            // ファイルは同じなので、個々のファイルの更新を見る
                            foreach (var key in logoTime.Keys)
                            {
                                var lastMod = File.GetLastWriteTime(key);
                                if (logoTime[key] != lastMod)
                                {
                                    logoTime[key] = lastMod;

                                    var name = Path.GetFileName(key);
                                    if (logoDict.ContainsKey(name))
                                    {
                                        ReadLogoFile(logoDict[name], key);
                                        updatedServices.Add(logoDict[name].ServiceId);
                                    }
                                }
                            }
                        }

                        if (serviceListUpdated)
                        {
                            // サービスリストが変わってたら設定保存
                            serviceListUpdated = false;
                            settingUpdated = true;
                        }

                        if (updatedServices.Count > 0)
                        {
                            // 更新をクライアントに通知
                            foreach (var updatedServiceId in updatedServices.Distinct())
                            {
                                await Client.OnServiceSetting(new ServiceSettingUpdate() {
                                    Type = ServiceSettingUpdateType.Update,
                                    ServiceId = updatedServiceId,
                                    Data = map[updatedServiceId]
                                });
                            }
                            // キューを再始動
                            var waits = new List<Task>();
                            UpdateQueueItems(waits);
                            await Task.WhenAll(waits);
                        }
                    }

                    string jlspath = GetJLDirectoryPath();
                    if (Directory.Exists(jlspath))
                    {
                        var lastModified = Directory.GetLastWriteTime(jlspath);
                        if (jlsDirTime != lastModified)
                        {
                            jlsDirTime = lastModified;

                            JlsCommandFiles = Directory.GetFiles(jlspath)
                                .Select(s => Path.GetFileName(s)).ToList();
                            await Client.OnCommonData(new CommonData()
                            {
                                JlsCommandFiles = JlsCommandFiles
                            });
                        }
                    }

                    string avspath = GetAvsDirectoryPath();
                    if (Directory.Exists(avspath))
                    {
                        var lastModified = Directory.GetLastWriteTime(avspath);
                        if (avsDirTime != lastModified)
                        {
                            avsDirTime = lastModified;

                            var files = Directory.GetFiles(avspath)
                                .Where(f => f.EndsWith(".avs", StringComparison.OrdinalIgnoreCase))
                                .Select(f => Path.GetFileName(f));

                            MainScriptFiles = files
                                .Where(f => f.StartsWith("メイン_")).ToList();

                            PostScriptFiles = files
                                .Where(f => f.StartsWith("ポスト_")).ToList();

                            await Client.OnCommonData(new CommonData()
                            {
                                MainScriptFiles = MainScriptFiles,
                                PostScriptFiles = PostScriptFiles
                            });
                        }
                    }

                    string batpath = GetBatDirectoryPath();
                    if (Directory.Exists(batpath))
                    {
                        var lastModified = Directory.GetLastWriteTime(batpath);
                        if (batDirTime != lastModified)
                        {
                            batDirTime = lastModified;

                            var files = Directory.GetFiles(batpath)
                                .Where(f =>
                                    f.EndsWith(".bat", StringComparison.OrdinalIgnoreCase) ||
                                    f.EndsWith(".cmd", StringComparison.OrdinalIgnoreCase))
                                .Select(f => Path.GetFileName(f));

                            AddQueueBatFiles = files
                                .Where(f => f.StartsWith("追加時_")).ToList();
                            PreBatFiles = files
                                .Where(f => f.StartsWith("実行前_")).ToList();
                            PostBatFiles = files
                                .Where(f => f.StartsWith("実行後_")).ToList();

                            await Client.OnCommonData(new CommonData()
                            {
                                AddQueueBatFiles = AddQueueBatFiles,
                                PreBatFiles = PreBatFiles,
                                PostBatFiles = PostBatFiles
                            });
                        }
                    }

                    string profilepath = GetProfileDirectoryPath();
                    if(!Directory.Exists(profilepath))
                    {
                        Directory.CreateDirectory(profilepath);
                    }
                    {
                        var lastModified = Directory.GetLastWriteTime(profilepath);
                        if (profileDirTime != lastModified)
                        {
                            profileDirTime = lastModified;

                            var newProfiles = Directory.GetFiles(profilepath)
                                .Where(s => s.EndsWith(".profile", StringComparison.OrdinalIgnoreCase))
                                .Select(s => Path.GetFileNameWithoutExtension(s))
                                .ToArray();

                            var initialUpdate = (profiles.Count == 0);

                            foreach (var name in newProfiles.Union(profiles.Keys.ToArray(), StringComparer.OrdinalIgnoreCase))
                            {
                                var filepath = GetProfilePath(profilepath, name);
                                if (profiles.ContainsKey(name) == false)
                                {
                                    // 追加された
                                    try
                                    {
                                        var profile = ReadProfile(filepath);
                                        profile.Name = name;
                                        profile.LastUpdate = File.GetLastWriteTime(filepath);
                                        profiles.Add(profile.Name, profile);
                                        await Client.OnProfile(new ProfileUpdate()
                                        {
                                            Type = UpdateType.Add,
                                            Profile = profile
                                        });
                                    }
                                    catch (Exception e)
                                    {
                                        await FatalError("プロファイル「" + filepath + "」の読み込みに失敗", e);
                                    }
                                }
                                else if (newProfiles.Contains(name, StringComparer.OrdinalIgnoreCase) == false)
                                {
                                    // 削除された
                                    var profile = profiles[name];
                                    profiles.Remove(name);
                                    await Client.OnProfile(new ProfileUpdate()
                                    {
                                        Type = UpdateType.Remove,
                                        Profile = profile
                                    });
                                }
                                else
                                {
                                    var profile = profiles[name];
                                    var lastUpdate = File.GetLastWriteTime(filepath);
                                    if (profile.LastUpdate != lastUpdate)
                                    {
                                        try
                                        {
                                            // 変更された
                                            profile = ReadProfile(filepath);
                                            profile.Name = name;
                                            profile.LastUpdate = lastUpdate;
                                            await Client.OnProfile(new ProfileUpdate()
                                            {
                                                Type = UpdateType.Update,
                                                Profile = profile
                                            });
                                        }
                                        catch (Exception e)
                                        {
                                            await FatalError("プロファイル「" + filepath + "」の読み込みに失敗", e);
                                        }
                                    }
                                }
                            }
                            if (profiles.ContainsKey(ServerSupport.GetDefaultProfileName()) == false)
                            {
                                // デフォルトがない場合は追加しておく
                                var profile = ServerSupport.NormalizeProfile(null);
                                profile.Name = ServerSupport.GetDefaultProfileName();
                                var filepath = GetProfilePath(profilepath, profile.Name);
                                SaveProfile(filepath, profile);
                                profile.LastUpdate = File.GetLastWriteTime(filepath);
                                profiles.Add(profile.Name, profile);
                                await Client.OnProfile(new ProfileUpdate()
                                {
                                    Type = UpdateType.Add,
                                    Profile = profile
                                });
                            }
                            if(initialUpdate)
                            {
                                // 初回の更新時はプロファイル関連付けを
                                // 更新するため再度設定を送る
                                await RequestSetting();
                            }
                        }
                    }

                    {
                        // 自動選択「デフォルト」がない場合は追加
                        if(autoSelects.ContainsKey("デフォルト") == false)
                        {
                            var def = new AutoSelectProfile()
                            {
                                Name = "デフォルト",
                                Conditions = new List<AutoSelectCondition>()
                            };
                            autoSelects.Add(def.Name, def);
                            await Client.OnAutoSelect(new AutoSelectUpdate()
                            {
                                Type = UpdateType.Add,
                                Profile = def
                            });
                            autoSelectUpdated = true;
                        }
                    }

                    if (await Task.WhenAny(completion, Task.Delay(2000)) == completion)
                    {
                        // 終了
                        return;
                    }
                }
            }
            catch (Exception exception)
            {
                await FatalError(
                    "WatchFileThreadがエラー終了しました", exception);
            }
        }

        private async Task SaveSettingThread()
        {
            try
            {
                var completion = saveSettingQ.OutputAvailableAsync();

                while (true)
                {
                    if(settingUpdated)
                    {
                        try
                        {
                            SaveAppData();
                        }
                        catch(Exception e)
                        {
                            await FatalError("設定保存に失敗しました", e);
                        }
                        settingUpdated = false;
                    }

                    if (uiStateUpdated)
                    {
                        try
                        {
                            SaveUIState();
                        }
                        catch (Exception e)
                        {
                            await FatalError("UI情報保存に失敗しました", e);
                        }
                        uiStateUpdated = false;
                    }

                    if (autoSelectUpdated)
                    {
                        try
                        {
                            SaveAutoSelectData();
                        }
                        catch (Exception e)
                        {
                            await FatalError("自動選択設定保存に失敗しました", e);
                        }
                        autoSelectUpdated = false;
                    }

                    try
                    {
                        queueManager.SaveQueueData(false);
                    }
                    catch (Exception e)
                    {
                        await FatalError("キュー状態保存に失敗しました", e);
                    }

                    if (await Task.WhenAny(completion, Task.Delay(5000)) == completion)
                    {
                        // 終了
                        return;
                    }
                }
            }
            catch (Exception exception)
            {
                await FatalError(
                    "SaveSettingThreadがエラー終了しました", exception);
            }
        }

        private async Task DrcsThread()
        {
            try
            {
                var completion = drcsQ.OutputAvailableAsync();

                while (true)
                {
                    await drcsManager.Update();

                    if (await Task.WhenAny(completion, Task.Delay(5000)) == completion)
                    {
                        // 終了
                        return;
                    }
                }
            }
            catch (Exception exception)
            {
                await FatalError(
                    "DrcsThreadがエラー終了しました", exception);
            }
        }

        internal void AddOutPathHistory(string path)
        {
            if (UIState_.LastOutputPath != path)
            {
                UIState_.LastOutputPath = path;
                uiStateUpdated = true;
            }
            if (OutPathHistory.AddHistory(path))
            {
                UIState_.OutputPathHistory = OutPathHistory.ToList();
                uiStateUpdated = true;
            }
        }

        private DiskItem MakeDiskItem(string path)
        {
            ulong available = 0;
            ulong total = 0;
            ulong free = 0;
            Util.GetDiskFreeSpaceEx(path, out available, out total, out free);
            return new DiskItem() { Capacity = (long)total, Free = (long)available, Path = path };
        }

        private void RefrechDiskSpace()
        {
            diskMap = new SortedDictionary<string, DiskItem>();
            if (string.IsNullOrEmpty(AppData_.setting.AlwaysShowDisk) == false)
            {
                foreach (var path in AppData_.setting.AlwaysShowDisk.Split(';'))
                {
                    if (string.IsNullOrEmpty(path))
                    {
                        continue;
                    }
                    try
                    {
                        var diskPath = Path.GetPathRoot(path);
                        diskMap.Add(diskPath, MakeDiskItem(diskPath));
                    }
                    catch (Exception e)
                    {
                        Util.AddLog("ディスク情報取得失敗: ", e);
                    }
                }
            }
            foreach(var item in queueManager.Queue.
                Where(s => !string.IsNullOrEmpty(s.DstPath)).
                Select(s => Path.GetPathRoot(s.DstPath)))
            {
                if (diskMap.ContainsKey(item) == false)
                {
                    diskMap.Add(item, MakeDiskItem(item));
                }
            }
            if(string.IsNullOrEmpty(AppData_.setting.WorkPath) == false) {
                var diskPath = Path.GetPathRoot(AppData_.setting.WorkPath);
                if (diskMap.ContainsKey(diskPath) == false)
                {
                    diskMap.Add(diskPath, MakeDiskItem(diskPath));
                }
            }
        }

        private static LogoSetting MakeNoLogoSetting(int serviceId)
        {
            return new LogoSetting() {
                FileName = LogoSetting.NO_LOGO,
                LogoName = "ロゴなし",
                ServiceId = serviceId,
                Exists = true,
                Enabled = false,
                From = new DateTime(2000, 1, 1),
                To = new DateTime(2030, 12, 31)
            };
        }

        private void SetDefaultFormat(ProfileSetting profile)
        {
            if (string.IsNullOrWhiteSpace(profile.RenameFormat))
            {
                profile.RenameFormat = "$SCtitle$\\$SCtitle$ $SCpart$第$SCnumber$話 「$SCsubtitle$」 ($SCservice$) [$SCdate$]";
            }
        }

        public Task SetProfile(ProfileUpdate data)
        {
            try
            {
                var waits = new List<Task>();
                var message = "プロファイル「"+ data.Profile.Name + "」が見つかりません";

                // 面倒だからAddもUpdateも同じ
                var profilepath = GetProfileDirectoryPath();
                var filepath = GetProfilePath(profilepath, data.Profile.Name);

                if (data.NewName != null)
                {
                    // リネーム
                    if (profiles.ContainsKey(data.Profile.Name))
                    {
                        var profile = profiles[data.Profile.Name];
                        var newfilepath = GetProfilePath(profilepath, data.NewName);
                        File.Move(filepath, newfilepath);
                        profile.Name = data.NewName;
                        profiles.Remove(data.Profile.Name);
                        profiles.Add(profile.Name, profile);
                        message = "プロファイル「" + data.Profile.Name + "」を「" + profile.Name + "」にリネームしました";
                    }
                    // キューを再始動
                    UpdateQueueItems(waits);
                }
                else if (data.Type == UpdateType.Add || data.Type == UpdateType.Update)
                {
                    SetDefaultFormat(data.Profile);
                    if (data.Type == UpdateType.Update)
                    {
                        CheckSetting(data.Profile, AppData_.setting);
                    }
                    SaveProfile(filepath, data.Profile);
                    data.Profile.LastUpdate = File.GetLastWriteTime(filepath);
                    if (profiles.ContainsKey(data.Profile.Name))
                    {
                        profiles[data.Profile.Name] = data.Profile;
                        message = "プロファイル「" + data.Profile.Name + "」を更新しました";
                    }
                    else
                    {
                        profiles.Add(data.Profile.Name, data.Profile);
                        message = "プロファイル「" + data.Profile.Name + "」を追加しました";
                    }
                    // キューを再始動
                    UpdateQueueItems(waits);
                }
                else
                {
                    if(profiles.ContainsKey(data.Profile.Name))
                    {
                        File.Delete(filepath);
                        profiles.Remove(data.Profile.Name);
                        message = "プロファイル「" + data.Profile.Name + "」を削除しました";
                    }
                }
                waits.Add(Client.OnProfile(data));
                waits.Add(NotifyMessage(message, false));
                return Task.WhenAll(waits);
            }
            catch (Exception e)
            {
                return NotifyError(e.Message, false);
            }
        }

        public Task SetAutoSelect(AutoSelectUpdate data)
        {
            try
            {
                var waits = new List<Task>();
                var message = "自動選択「" + data.Profile.Name + "」が見つかりません";

                // 面倒だからAddもUpdateも同じ

                if (data.NewName != null)
                {
                    // リネーム
                    if (autoSelects.ContainsKey(data.Profile.Name))
                    {
                        var profile = autoSelects[data.Profile.Name];
                        profile.Name = data.NewName;
                        autoSelects.Remove(data.Profile.Name);
                        autoSelects.Add(profile.Name, profile);
                        message = "自動選択「" + data.Profile.Name + "」を「" + profile.Name + "」にリネームしました";
                        autoSelectUpdated = true;
                    }
                }
                else if (data.Type == UpdateType.Add || data.Type == UpdateType.Update)
                {
                    if (data.Type == UpdateType.Update)
                    {
                        CheckAutoSelect(data.Profile);
                    }
                    if (autoSelects.ContainsKey(data.Profile.Name))
                    {
                        autoSelects[data.Profile.Name] = data.Profile;
                        message = "自動選択「" + data.Profile.Name + "」を更新しました";
                        autoSelectUpdated = true;
                    }
                    else
                    {
                        autoSelects.Add(data.Profile.Name, data.Profile);
                        message = "自動選択「" + data.Profile.Name + "」を追加しました";
                        autoSelectUpdated = true;
                    }
                    // キューを再始動
                    UpdateQueueItems(waits);
                }
                else
                {
                    if (autoSelects.ContainsKey(data.Profile.Name))
                    {
                        autoSelects.Remove(data.Profile.Name);
                        message = "自動選択「" + data.Profile.Name + "」を削除しました";
                        autoSelectUpdated = true;
                    }
                }
                waits.Add(Client.OnAutoSelect(data));
                waits.Add(NotifyMessage(message, false));
                return Task.WhenAll(waits);
            }
            catch (Exception e)
            {
                return NotifyError(e.Message, false);
            }
        }

        public Task SetCommonData(CommonData data)
        {
            try
            {
                if(data.Setting != null)
                {
                    SetDefaultPath(data.Setting);
                    CheckSetting(null, data.Setting);
                    AppData_.setting = data.Setting;
                    workerPool.SetNumParallel(data.Setting.NumParallel);
                    SetScheduleParam(AppData_.setting.SchedulingEnabled,
                        AppData_.setting.NumGPU, AppData_.setting.MaxGPUResources);

                    if(AppData_.setting.EnableShutdownAction == false &&
                        AppData_.finishSetting.Action == FinishAction.Shutdown) {
                        // 無効な設定となった
                        AppData_.finishSetting.Action = FinishAction.None;
                    }
                    pauseScheduler.NotifySettingChanged();

                    settingUpdated = true;
                    return Task.WhenAll(
                        Client.OnCommonData(new CommonData() {
                            Setting = AppData_.setting,
                            FinishSetting = AppData_.finishSetting
                        }),
                        RequestFreeSpace(),
                        NotifyMessage("設定を更新しました", false));
                }
                else if(data.MakeScriptData != null)
                {
                    AppData_.scriptData = data.MakeScriptData;
                    settingUpdated = true;
                    return Client.OnCommonData(new CommonData() {
                        MakeScriptData = data.MakeScriptData
                    });
                }
                else if(data.FinishSetting != null)
                {
                    AppData_.finishSetting = data.FinishSetting;
                    settingUpdated = true;
                    return Client.OnCommonData(new CommonData() {
                        FinishSetting = data.FinishSetting
                    });
                }
                return Task.FromResult(0);
            }
            catch(Exception e)
            {
                return NotifyError(e.Message, false);
            }
        }

        public Task AddQueue(AddQueueRequest req)
        {
            queueQ.Post(req);
            return Task.FromResult(0);
        }

        public Task ChangeItemTask(ChangeItemData data)
        {
            queueQ.Post(data);
            return Task.FromResult(0);
        }

        public async Task PauseEncode(PauseRequest request)
        {
            if (request.IsQueue)
            {
                workerPool.SetPause(request.Pause, false);
            }
            else
            {
                if(request.Index == -1)
                {
                    foreach(var worker in workerPool.Workers.OfType<TranscodeWorker>())
                    {
                        worker.SetSuspend(request.Pause, false);
                    }
                }
                else
                {
                    var workers = workerPool.Workers.ToArray();
                    if(request.Index < workers.Length)
                    {
                        ((TranscodeWorker)workers[request.Index]).SetSuspend(request.Pause, false);
                    }
                }
            }
            Task task = RequestState();
            await task;
        }

        public Task CancelAddQueue()
        {
            queueManager.CancelAddQueue();
            return Task.FromResult(0);
        }

        public Task CancelSleep()
        {
            if(finishActionRunner != null)
            {
                finishActionRunner.Canceled = true;
                finishActionRunner = null;
                SleepCancel = new FinishSetting();
                return Client?.OnUIData(new UIData()
                {
                    SleepCancel = SleepCancel
                });
            }
            return Task.FromResult(0);
        }

        // 指定した名前のプロファイルを取得
        internal ProfileSetting GetProfile(string name)
        {
            return profiles.GetOrDefault(name);
        }

        // プロファイルがペンディングとなっているアイテムに対して
        // プロファイルの決定を試みる
        internal ProfileSetting SelectProfile(QueueItem item, out int itemPriority)
        {
            itemPriority = 0;

            bool isAuto = false;
            var profileName = ServerSupport.ParseProfileName(item.ProfileName, out isAuto);

            if (isAuto)
            {
                if (autoSelects.ContainsKey(profileName) == false)
                {
                    item.FailReason = "自動選択「" + profileName + "」がありません";
                    item.State = QueueState.LogoPending;
                    return null;
                }

                var resolvedProfile = ServerSupport.AutoSelectProfile(item, autoSelects[profileName], out itemPriority);
                if (resolvedProfile == null)
                {
                    item.FailReason = "自動選択「" + profileName + "」でプロファイルが選択されませんでした";
                    item.State = QueueState.LogoPending;
                    return null;
                }

                profileName = resolvedProfile;
            }

            if (profiles.ContainsKey(profileName) == false)
            {
                item.FailReason = "プロファイル「" + profileName + "」がありません";
                item.State = QueueState.LogoPending;
                return null;
            }

            return profiles[profileName];
        }

        // 実行できる状態になったアイテムをスケジューラに登録
        internal void ScheduleQueueItem(QueueItem item)
        {
            scheduledQueue.AddQueue(item);

            // OnStartでやると、エンコードが始まってから設定が
            // 変更されたときに反映されないので、ここでやる
            if (AppData_.setting.SupressSleep)
            {
                // サスペンドを抑止
                if (preventSuspend == null)
                {
                    preventSuspend = new PreventSuspendContext();
                }
            }
        }

        // 指定アイテムをキャンセルする
        internal bool CancelItem(QueueItem item)
        {
            foreach (var worker in workerPool.Workers.Cast<TranscodeWorker>())
            {
                if (worker != null)
                {
                    if(worker.CancelItem(item))
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        internal void ReScheduleQueue()
        {
            scheduledQueue.MakeDirty();
        }

        internal void ForceStartItem(QueueItem item)
        {
            workerPool.ForceStart(item);
        }

        // 新しいサービスを登録
        internal Task AddService(ServiceSettingElement newElement)
        {
            AppData_.services.ServiceMap.Add(newElement.ServiceId, newElement);
            serviceListUpdated = true;
            return Client.OnServiceSetting(new ServiceSettingUpdate()
            {
                Type = ServiceSettingUpdateType.Update,
                ServiceId = newElement.ServiceId,
                Data = newElement
            });
        }

#region QueueManager
        // アイテム状態の更新をクライアントに通知
        internal Task NotifyQueueItemUpdate(QueueItem item)
        {
            return queueManager.NotifyQueueItemUpdate(item);
        }

        // ペンディング <=> キュー 状態を切り替える
        // ペンディングからキューになったらスケジューリングに追加する
        // notifyItem: trueの場合は、ディレクトリ・アイテム両方の更新通知、falseの場合は、ディレクトリの更新通知のみ
        // 戻り値: 状態が変わった
        internal bool UpdateQueueItem(QueueItem item, List<Task> waits)
        {
            return queueManager.UpdateQueueItem(item, waits);
        }

        internal List<Task> UpdateQueueItems(List<Task> waits)
        {
            return queueManager.UpdateQueueItems(waits);
        }

        internal QueueItem[] GetQueueItems(string srcPath)
        {
            return queueManager.Queue.Where(s => s.SrcPath == srcPath).ToArray();
        }

        int getItemIdFromWorkerId(int workerId)
        {
            var workers = workerPool.Workers.ToArray();
            return (0 <= workerId && workerId < workers.Length) ? ((TranscodeWorker)workers[workerId]).GetItemId() : -1;
        }

        public Task ChangeItem(ChangeItemData data)
        {
            if (data.ItemId < 0)
            {
                data.ItemId = getItemIdFromWorkerId(data.workerId);
                if (data.ItemId < 0)
                {
                    return Task.FromResult(0);
                }
            }
            return queueManager.ChangeItem(data);
        }
#endregion

#region Request
        private async Task RequestSetting()
        {
            await Client.OnCommonData(new CommonData() {
                Setting = AppData_.setting,
                UIState = UIState_,
                JlsCommandFiles = JlsCommandFiles,
                MainScriptFiles = MainScriptFiles,
                PostScriptFiles = PostScriptFiles,
                CpuClusters = affinityCreator.GetClusters(),
                ServerInfo = new ServerInfo()
                {
                    HostName = Dns.GetHostName(),
                    MacAddress = ClientManager?.GetMacAddress(),
                    Version = Version
                },
                AddQueueBatFiles = AddQueueBatFiles,
                PreBatFiles = PreBatFiles,
                PostBatFiles = PostBatFiles,
                FinishSetting = AppData_.finishSetting
            });

            // プロファイル
            await Client.OnProfile(new ProfileUpdate()
            {
                Type = UpdateType.Clear
            });
            foreach (var profile in profiles.Values.ToArray())
            {
                await Client.OnProfile(new ProfileUpdate() {
                    Profile = profile,
                    Type = UpdateType.Update
                });
            }

            // 自動選択
            await Client.OnAutoSelect(new AutoSelectUpdate()
            {
                Type = UpdateType.Clear
            });
            foreach (var profile in autoSelects.Values.ToArray())
            {
                await Client.OnAutoSelect(new AutoSelectUpdate()
                {
                    Profile = profile,
                    Type = UpdateType.Update
                });
            }

            // プロファイルがないと関連付けできないため、
            // プロファイルを送った後にこれを送る
            await Client.OnCommonData(new CommonData()
            {
                MakeScriptData = AppData_.scriptData,
            });
        }

        private Task RequestQueue()
        {
            return Client.OnUIData(new UIData()
            {
                QueueData = new QueueData()
                {
                    Items = queueManager.Queue
                }
            });
        }

        private Task RequestLog()
        {
            return Client.OnUIData(new UIData()
            {
                LogData = logData
            });
        }

        private Task RequestCheckLog()
        {
            return Client.OnUIData(new UIData()
            {
                CheckLogData = checkLogData
            });
        }

        private Task RequestConsole()
        {
            return Task.WhenAll(workerPool.Workers.Cast<TranscodeWorker>().Select(w =>
                Client.OnUIData(new UIData()
                {
                    ConsoleData = new ConsoleData()
                    {
                        index = w.Id,
                        text = w.TextLines
                    },
                    EncodeState = w.State
                })).Concat(new Task[] { Client.OnUIData(new UIData()
                {
                    ConsoleData = new ConsoleData()
                    {
                        index = -1,
                        text = queueManager.TextLines
                    }
                }) }));
        }

        internal Task RequestUIState()
        {
            return Client.OnCommonData(new CommonData()
            {
                UIState = UIState_
            });
        }

        internal Task RequestState()
        {
            return RequestState(null);
        }

        private void PlaySound(string name)
        {
            var localClientRunning = ClientManager?.HasLocalClient() ?? true;
            if (localClientRunning == false)
            {
                Util.PlayRandomSound("sound\\" + name);
            }
        }

        internal Task RequestState(StateChangeEvent? changeEvent)
        {
            var workers = workerPool.Workers.OfType<TranscodeWorker>().ToArray();
            QueuePaused = workerPool.IsPaused;
            EncodePaused = workers.All(w => w.Suspended);
            var state = new State()
            {
                Pause = workerPool.UserPaused,
                Suspend = workers.All(w => w.UserSuspended),
                EncoderSuspended = workers.Select(w => w.UserSuspended).ToArray(),
                Running = nowEncoding,
                ScheduledPause = workerPool.ScheduledPaused,
                ScheduledSuspend = workers.FirstOrDefault()?.ScheduledSuspended ?? false,
                Progress = Progress
            };
            if(changeEvent != null)
            {
                switch (changeEvent)
                {
                    case StateChangeEvent.WorkersStarted:
                        PlaySound("start");
                        break;
                    case StateChangeEvent.WorkersFinished:
                        PlaySound("complete");
                        break;
                    case StateChangeEvent.EncodeSucceeded:
                        PlaySound("succeeded");
                        break;
                    case StateChangeEvent.EncodeFailed:
                        PlaySound("failed");
                        break;
                }
            }
            return Client.OnUIData(new UIData()
            {
                State = state,
                StateChangeEvent = changeEvent
            });
        }

        internal Task RequestFreeSpace()
        {
            RefrechDiskSpace();
            return Client.OnCommonData(new CommonData()
            {
                Disks = diskMap.Values.ToList()
            });
        }

        private async Task RequestServiceSetting()
        {
            var serviceMap = AppData_.services.ServiceMap;
            await Client.OnServiceSetting(new ServiceSettingUpdate()
            {
                Type = ServiceSettingUpdateType.Clear
            });
            foreach (var service in serviceMap.Values.ToArray())
            {
                await Client.OnServiceSetting(new ServiceSettingUpdate()
                {
                    Type = ServiceSettingUpdateType.Update,
                    ServiceId = service.ServiceId,
                    Data = service
                });
            }
        }

        public async Task Request(ServerRequest req)
        {
            if ((req & ServerRequest.Setting) != 0)
            {
                await RequestSetting();
            }
            if ((req & ServerRequest.Queue) != 0)
            {
                await RequestQueue();
            }
            if ((req & ServerRequest.Log) != 0)
            {
                await RequestLog();
            }
            if ((req & ServerRequest.CheckLog) != 0)
            {
                await RequestCheckLog();
            }
            if ((req & ServerRequest.Console) != 0)
            {
                await RequestConsole();
            }
            if ((req & ServerRequest.State) != 0)
            {
                await RequestState();
            }
            if ((req & ServerRequest.FreeSpace) != 0)
            {
                await RequestFreeSpace();
            }
            if ((req & ServerRequest.ServiceSetting) != 0)
            {
                await RequestServiceSetting();
            }
        }
#endregion

        public Task RequestLogFile(LogFileRequest req)
        {
            if (req.LogItem != null)
            {
                return Client.OnLogFile(ReadLogFIle(req.LogItem.EncodeStartDate));
            }
            else if (req.CheckLogItem != null)
            {
                return Client.OnLogFile(ReadCheckLogFIle(req.CheckLogItem.CheckStartDate));
            }
            return Task.FromResult(0);
        }

        public Task RequestDrcsImages()
        {
            return drcsManager.RequestDrcsImages();
        }

        public async Task SetServiceSetting(ServiceSettingUpdate update)
        {
            var serviceMap = AppData_.services.ServiceMap;
            var message = "サービスが見つかりません";
            if(serviceMap.ContainsKey(update.ServiceId))
            {
                if (update.Type == ServiceSettingUpdateType.Update)
                {
                    var old = serviceMap[update.ServiceId];
                    if (old.LogoSettings.Count == update.Data.LogoSettings.Count)
                    {
                        // ロゴのExitsフラグだけはこちらのデータを継承させる
                        for (int i = 0; i < old.LogoSettings.Count; ++i)
                        {
                            update.Data.LogoSettings[i].Exists = old.LogoSettings[i].Exists;
                        }
                        serviceMap[update.ServiceId] = update.Data;

                        var waits = new List<Task>();
                        UpdateQueueItems(waits);
                        await Task.WhenAll(waits);
                        message = "サービス「" + update.Data.ServiceName + "」の設定を更新しました";
                    }
                }
                else if (update.Type == ServiceSettingUpdateType.AddNoLogo)
                {
                    var service = serviceMap[update.ServiceId];
                    service.LogoSettings.Add(MakeNoLogoSetting(update.ServiceId));
                    update.Type = ServiceSettingUpdateType.Update;
                    update.Data = service;
                    message = "サービス「" + service.ServiceName + "」にロゴなしを追加しました";
                }
                else if (update.Type == ServiceSettingUpdateType.Remove)
                {
                    var service = serviceMap[update.ServiceId];
                    serviceMap.Remove(update.ServiceId);
                    update.Data = null;
                    message = "サービス「" + service.ServiceName + "」を削除しました";
                }
                else if (update.Type == ServiceSettingUpdateType.RemoveLogo)
                {
                    var service = serviceMap[update.ServiceId];
                    service.LogoSettings.RemoveAt(update.RemoveLogoIndex);
                    update.Type = ServiceSettingUpdateType.Update;
                    update.Data = service;
                    message = "サービス「" + service.ServiceName + "」のロゴを削除しました";
                }
                settingUpdated = true;
            }
            await Client.OnServiceSetting(update);
        }

        private AMTContext amtcontext = new AMTContext();
        public Task RequestLogoData(string fileName)
        {
            if(fileName == LogoSetting.NO_LOGO)
            {
                return NotifyError("不正な操作です", false);
            }
            string logopath = GetLogoFilePath(fileName);
            try
            {
                var logofile = new LogoFile(amtcontext, logopath);
                return Client.OnLogoData(new LogoData() {
                    FileName = fileName,
                    ServiceId = logofile.ServiceId,
                    ImageWith = logofile.ImageWidth,
                    ImageHeight = logofile.ImageHeight,
                    Image = logofile.GetImage(0)
                });
            }
            catch(IOException exception)
            {
                return FatalError(
                    "ロゴファイルを開けません。パス:" + logopath, exception);
            }
        }

        public Task AddDrcsMap(DrcsImage recvitem)
        {
            return drcsManager.AddDrcsMap(recvitem);
        }

        public Task EndServer()
        {
            finishRequested?.Invoke();
            return Task.FromResult(0);
        }
    }
}
