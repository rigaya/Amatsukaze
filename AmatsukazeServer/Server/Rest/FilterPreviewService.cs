using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Amatsukaze.Lib;

namespace Amatsukaze.Server.Rest
{
    public sealed class FilterPreviewConfigRequest
    {
        public long Gen { get; set; }
        public FilterOption FilterOption { get; set; }
        public FilterSetting FilterSetting { get; set; }
        public EncoderFilterSetting EncoderFilterSetting { get; set; }
        public string QSVEncFilterOption { get; set; }
        public string NVEncFilterOption { get; set; }
        public string VCEEncFilterOption { get; set; }
    }

    public sealed class FilterPreviewSaveRequest
    {
        public bool SaveAs { get; set; }
        public bool PreserveCustomFilter { get; set; }
        public string Name { get; set; }
    }

    public sealed class FilterPreviewFrameRequest
    {
        public long Gen { get; set; }
        public int Pos { get; set; }
        public string Side { get; set; } = "after";
        public int Scale { get; set; } = 1;
        public string Format { get; set; } = "yuv";
        public int[] Crop { get; set; }
        public int? Step { get; set; }
        public bool Decimate { get; set; } = true;
    }

    public sealed class FilterPreviewViewRequest
    {
        public long Gen { get; set; }
        public string Op { get; set; }
        public double? Seconds { get; set; }
        public string Phase { get; set; }
        public string Direction { get; set; }
        public string Mode { get; set; }
        public long? Revision { get; set; }
        public int Scale { get; set; } = 1;
        public string Format { get; set; } = "yuv";
        public int[] Crop { get; set; }

        internal Dictionary<string, object> ToCommand()
        {
            if (Scale is < 1 or > 16 || Format is not ("yuv" or "jpeg" or "png")
                || Crop != null && (Crop.Length != 4 || Crop[0] < 0 || Crop[1] < 0 || Crop[2] <= 0 || Crop[3] <= 0))
                throw new ArgumentException("表示画像の転送形式が不正です");

            var command = new Dictionary<string, object> { ["op"] = Op, ["scale"] = Scale, ["format"] = Format };
            if (Crop != null) command["crop"] = Crop;
            switch (Op)
            {
                case "seek" when Seconds is double seconds && double.IsFinite(seconds) && seconds >= 0
                    && Phase is null or "drag" or "commit":
                    command["seconds"] = seconds;
                    if (Phase != null) command["phase"] = Phase;
                    break;
                case "step" when Direction is "previous" or "next":
                    command["direction"] = Direction;
                    break;
                case "display" when Mode is "after" or "before" or "split" or "sideBySide":
                    command["mode"] = Mode;
                    break;
                case "snapshot" when Revision is null or >= 0:
                    if (Revision.HasValue) command["revision"] = Revision.Value;
                    break;
                default:
                    throw new ArgumentException("表示操作が不正です");
            }
            return command;
        }
    }

    public sealed class FilterPreviewService : IDisposable
    {
        private sealed class Session : IDisposable
        {
            public string Directory, WorkDirectory, Id, Dat, ModuleDirectory, SourceName;
            public ProfileSetting Profile;
            public FilterOption OriginalFilterOption;
            public Setting Setting;
            public FilterPreviewSourceFormat Format;
            public FilterPreviewEngine Engine;
            public long Generation, CommittedGeneration;
            public string FpsKey;
            private readonly TimeProvider clock;
            public long LastUsed;
            public Session(TimeProvider clock)
            {
                this.clock = clock;
                Touch();
            }
            public readonly SemaphoreSlim ConfigLock = new(1, 1);
            public readonly ConcurrentDictionary<string, FilterPreviewEngine.Reply> JpegCache = new();
            public readonly ConcurrentQueue<string> JpegKeys = new();
            public void Touch() => Interlocked.Exchange(ref LastUsed, clock.GetUtcNow().UtcTicks);
            public void Dispose()
            {
                if (!Engine.Stop()) { Util.AddLog("[FilterPreview] エンジンを停止できず作業フォルダを残しました", null); return; }
                try
                {
                    var root = Path.GetFullPath(Directory).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
                    var work = Path.GetFullPath(WorkDirectory).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
                    var comparison = OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
                    if (Guid.TryParseExact(Id, "N", out _) && Path.GetFileName(root) == "filter_preview" &&
                        string.Equals(Path.GetDirectoryName(work), root, comparison) && string.Equals(Path.GetFileName(work), Id, comparison) &&
                        System.IO.Directory.Exists(work) && (File.GetAttributes(work) & FileAttributes.ReparsePoint) == 0)
                        System.IO.Directory.Delete(work, true);
                }
                catch (Exception ex) when (ex is IOException || ex is UnauthorizedAccessException)
                { Util.AddLog("[FilterPreview] 作業フォルダを削除できません", ex); }
            }
        }
        private static readonly TimeSpan Ttl = TimeSpan.FromMinutes(5);
        private readonly RestStateStore state;
        private readonly Func<string, FilterPreviewSourceFormat> readSourceFormat;
        private readonly ConcurrentDictionary<string, Session> sessions = new();
        private readonly FilterPreviewEngineTiming timing;
        private readonly ITimer timer;

        public FilterPreviewService(RestStateStore state) : this(state, FilterPreviewSourceReader.Read) { }

        internal FilterPreviewService(RestStateStore state, Func<string, FilterPreviewSourceFormat> readSourceFormat,
            FilterPreviewEngineTiming timing = null)
        {
            this.state = state;
            this.readSourceFormat = readSourceFormat;
            this.timing = timing ?? FilterPreviewEngineTiming.Default;
            timer = this.timing.Clock.CreateTimer(_ => Cleanup(), null, Ttl, Ttl);
        }

        public async Task<string> CreateAsync(int queueItemId, CancellationToken cancellation)
        {
            if (!state.TryGetQueueItem(queueItemId, out var item) || item.Profile == null)
                throw new ArgumentException("キューアイテムまたはプロファイルが見つかりません");
            var log = state.ResolveTaskLogPathById(queueItemId);
            if (string.IsNullOrEmpty(log) || !File.Exists(log)) throw new FileNotFoundException("ログファイルが見つかりません");
            var temp = ServerSupport.ExtractTempDirFromLog(log);
            if (string.IsNullOrEmpty(temp)) throw new IOException("一時フォルダが取得できません");
            var dat = Path.Combine(temp, "amts0.dat");
            if (!File.Exists(dat)) throw new FileNotFoundException("amts0.datが見つかりません", dat);
            var setting = state.GetSetting() ?? throw new InvalidOperationException("サーバー設定が見つかりません");
            var module = AppContext.BaseDirectory;
            // GUI版 HWEncVppPreview とは分離し、stdioエンジン専用の CLI を既定とする。
            var executable = string.IsNullOrWhiteSpace(setting.FilterPreviewEnginePath)
                ? Path.Combine(module, "HWEncVppPreviewCLI" + (OperatingSystem.IsWindows() ? ".exe" : "")) : setting.FilterPreviewEnginePath;
            if (string.IsNullOrWhiteSpace(setting.FilterPreviewEnginePath) && !File.Exists(executable))
                throw new FileNotFoundException("フィルタプレビューエンジンが見つかりません。HWEncVppPreviewCLIをサーバーと同じフォルダへ配置してください", executable);
            var dir = Path.Combine(temp, "filter_preview");
            System.IO.Directory.CreateDirectory(dir);
            var format = readSourceFormat(dat);
            var sourcePath = Path.Combine(dir, "src.avs");
            var temporarySource = Path.Combine(dir, "src_" + Guid.NewGuid().ToString("N") + ".tmp");
            try
            {
                FilterPreviewScripts.WriteScript(temporarySource, FilterPreviewScripts.Source(module, dat));
                File.Move(temporarySource, sourcePath, true);
            }
            finally { if (File.Exists(temporarySource)) File.Delete(temporarySource); }
            var args = new List<string> { "--amatsukaze-engine", "--tmp", temp, "--video", "0", "--amatsukaze-dir", module };
            foreach (var pair in new[] { ("--qsvenc", setting.QSVEncPath), ("--nvenc", setting.NVEncPath), ("--vceenc", setting.VCEEncPath) })
                if (!string.IsNullOrEmpty(pair.Item2)) { args.Add(pair.Item1); args.Add(pair.Item2); }
            var id = Guid.NewGuid().ToString("N");
            var workDirectory = Path.Combine(dir, id);
            var profile = ServerSupport.DeepCopy(item.Profile);
            var engine = new FilterPreviewEngine(executable, args, timing);
            try { System.IO.Directory.CreateDirectory(workDirectory); }
            catch { engine.Dispose(); throw; }
            var session = new Session(timing.Clock) { OriginalFilterOption = profile.FilterOption, SourceName = item.SrcPath?[(item.SrcPath.LastIndexOfAny(new[] { '/', '\\' }) + 1)..], Id = id, Directory = dir, WorkDirectory = workDirectory, Dat = dat, ModuleDirectory = module, Profile = profile, Setting = setting, Format = format, Engine = engine };
            try
            {
                await session.Engine.WaitReadyAsync(cancellation).ConfigureAwait(false);
                if (!sessions.TryAdd(id, session)) throw new InvalidOperationException("セッションを作成できません");
                return id;
            }
            catch { session.Dispose(); throw; }
        }

        private Session Get(string id, bool touch = true)
        {
            if (!sessions.TryGetValue(id, out var session)) throw new KeyNotFoundException("セッションが見つかりません");
            if (touch) session.Touch();
            return session;
        }

        public static string[] GetNotices(FilterOption option, FilterSetting filter, string encoderOptions = null)
        {
            var notices = new List<string>();
            if (option == FilterOption.Setting && filter?.EnableDeinterlace == true && filter.DeinterlaceAlgorithm == DeinterlaceAlgorithm.AutoVfr)
                notices.Add("Auto_Vfrはプレビュー非対応のため、インタレ解除なしで表示しています");
            if (ProfileSettingExtensions.GetFilterEncoderType(option).HasValue && encoderOptions != null
                && new[] { "--output-res", "--crop", "--vpp-pad" }.Any(encoderOptions.Contains))
                notices.Add("エンコーダフィルタによる解像度変更はコンテナのメタデータ(解像度/SAR)に反映されません");
            return notices.ToArray();
        }

        private static string[] SplitOptions(string options)
        {
            var result = new List<string>();
            var current = new StringBuilder();
            bool quoted = false, started = false;
            options ??= "";
            for (var i = 0; i < options.Length; i++)
            {
                var c = options[i];
                if (c == '\\')
                {
                    int start = i;
                    while (i < options.Length && options[i] == '\\') i++;
                    int count = i - start;
                    if (i < options.Length && options[i] == '"')
                    {
                        current.Append('\\', count / 2);
                        if ((count & 1) != 0) current.Append('"');
                        else quoted = !quoted;
                        started = true;
                    }
                    else { current.Append('\\', count); i--; started = true; }
                }
                else if (c == '"') { quoted = !quoted; started = true; }
                else if (char.IsWhiteSpace(c) && !quoted) { if (started) { result.Add(current.ToString()); current.Clear(); started = false; } }
                else { current.Append(c); started = true; }
            }
            if (quoted) throw new ArgumentException("フィルタオプションの引用符が閉じられていません");
            if (started) result.Add(current.ToString());
            return result.ToArray();
        }

        public async Task<FilterPreviewEngine.Reply> ConfigureAsync(string id, FilterPreviewConfigRequest request, CancellationToken cancellation)
        {
            var session = Get(id);
            await session.ConfigLock.WaitAsync(cancellation).ConfigureAwait(false);
            Task<FilterPreviewEngine.Reply> pending;
            ProfileSetting candidate;
            try
            {
                if (request.Gen <= session.Generation) throw new ArgumentException("設定の世代が古いです");
                var profile = ServerSupport.DeepCopy(session.Profile);
                candidate = profile;
                profile.FilterOption = request.FilterOption;
                profile.FilterSetting = request.FilterSetting ?? profile.FilterSetting ?? new FilterSetting();
                profile.EncoderFilterSetting = request.EncoderFilterSetting ?? profile.EncoderFilterSetting ?? new EncoderFilterSetting();
                profile.QSVEncFilterOption = request.QSVEncFilterOption;
                profile.NVEncFilterOption = request.NVEncFilterOption;
                profile.VCEEncFilterOption = request.VCEEncFilterOption;
                string interlace = session.Format.Progressive != 0 ? "progressive" : "tff";
                var cmd = new Dictionary<string, object> { ["cmd"] = "config", ["sarnum"] = session.Format.SarWidth > 0 && session.Format.SarHeight > 0 ? session.Format.SarWidth : 0, ["sarden"] = session.Format.SarWidth > 0 && session.Format.SarHeight > 0 ? session.Format.SarHeight : 0, ["interlace"] = interlace };
                if (profile.FilterOption == FilterOption.None || profile.FilterOption == FilterOption.Setting)
                {
                    var filter = profile.FilterOption == FilterOption.None ? new FilterSetting() : profile.FilterSetting;
                    var path = Path.Combine(session.WorkDirectory, "preview_" + request.Gen + ".avs");
                    FilterPreviewScripts.WriteScript(path, FilterPreviewScripts.Preview(session.ModuleDirectory, session.Dat, session.WorkDirectory, filter, session.Setting, 0));
                    cmd["kind"] = "avs"; cmd["path"] = path;
                    // 実際に生成するプレビュー用スクリプトのインタレ解除有無を伝える。
                    cmd["afterInterlace"] = AvsScriptCreator.AppliesDeinterlace(filter, preview: true) ? "progressive" : interlace;
                }
                else
                {
                    var encoder = ProfileSettingExtensions.GetFilterEncoderType(profile.FilterOption);
                    if (!encoder.HasValue) throw new ArgumentException("カスタムフィルタはプレビューできません");
                    cmd["kind"] = "encoder"; cmd["encoder"] = encoder.Value.ToString();
                    cmd["vpp"] = SplitOptions(ProfileSettingExtensions.GetFilterEncoderOption(profile));
                }
                var primaryNames = new Dictionary<int, string> { [1] = "bt709", [4] = "bt470m", [5] = "bt470bg", [6] = "smpte170m", [7] = "smpte240m", [8] = "film", [9] = "bt2020", [10] = "st428", [11] = "st431-2", [12] = "st432-1", [22] = "ebu3213-e" };
                var transferNames = new Dictionary<int, string> { [1] = "bt709", [4] = "bt470m", [5] = "bt470bg", [6] = "smpte170m", [7] = "smpte240m", [8] = "linear", [9] = "log100", [10] = "log316", [11] = "iec61966-2-4", [12] = "bt1361e", [13] = "iec61966-2-1", [14] = "bt2020-10", [15] = "bt2020-12", [16] = "smpte2084", [17] = "smpte428", [18] = "arib-std-b67" };
                var matrixNames = new Dictionary<int, string> { [0] = "GBR", [1] = "bt709", [4] = "fcc", [5] = "bt470bg", [6] = "smpte170m", [7] = "smpte240m", [8] = "YCgCo", [9] = "bt2020nc", [10] = "bt2020c", [11] = "smpte2085", [12] = "derived-ncl", [13] = "derived-cl", [14] = "ictco" };
                if (primaryNames.TryGetValue(session.Format.ColorPrimaries, out var primaries)) cmd["primaries"] = primaries;
                if (transferNames.TryGetValue(session.Format.TransferCharacteristics, out var transfer)) cmd["transfer"] = transfer;
                if (matrixNames.TryGetValue(session.Format.ColorSpace, out var matrix)) cmd["matrix"] = matrix;
                pending = session.Engine.RequestAsync(cmd, request.Gen, false, cancellation);
                session.Generation = request.Gen;
            }
            finally { session.ConfigLock.Release(); }
            var reply = await pending.ConfigureAwait(false);
            await session.ConfigLock.WaitAsync(cancellation).ConfigureAwait(false);
            try
            {
                if (session.Generation == request.Gen)
                {
                    session.Profile = candidate;
                    session.CommittedGeneration = request.Gen;
                    session.FpsKey = reply.Metadata.GetValueOrDefault("fpsnum", "") + "/" + reply.Metadata.GetValueOrDefault("fpsden", "");
                }
            }
            finally { session.ConfigLock.Release(); }
            return reply;
        }

        public async Task<FilterPreviewEngine.Reply> FrameAsync(string id, FilterPreviewFrameRequest request, CancellationToken cancellation)
        {
            var session = Get(id);
            await session.ConfigLock.WaitAsync(cancellation).ConfigureAwait(false);
            try { ValidateKind(session, request.Gen, encoder: false); }
            finally { session.ConfigLock.Release(); }
            if (request.Pos < 0 || (request.Side != "before" && request.Side != "after") || request.Scale < 1 || request.Scale > 16 || (request.Format != "yuv" && request.Format != "jpeg" && request.Format != "png") || (request.Crop != null && request.Crop.Length != 4) || (request.Step.HasValue && request.Step != -1 && request.Step != 1)) throw new ArgumentException("フレーム要求が不正です");
            var cmd = new Dictionary<string, object> { ["cmd"] = "frame", ["pos"] = request.Pos, ["side"] = request.Side, ["scale"] = request.Scale, ["format"] = request.Format, ["decimate"] = request.Decimate };
            if (request.Crop != null) cmd["crop"] = request.Crop;
            if (request.Step.HasValue) cmd["step"] = request.Step.Value;
            string cacheKey = null;
            if (request.Format == "jpeg")
            {
                var generationKey = request.Side == "before" && !request.Step.HasValue && session.CommittedGeneration == request.Gen
                    ? "before:" + session.FpsKey : "gen:" + request.Gen;
                cacheKey = string.Join("|", generationKey, request.Pos, request.Side, request.Scale, request.Step, request.Decimate, request.Crop == null ? "" : string.Join(",", request.Crop));
                if (session.JpegCache.TryGetValue(cacheKey, out var cached))
                {
                    var metadata = cached.Metadata.ToDictionary(pair => pair.Key, pair => pair.Value);
                    metadata["gen"] = request.Gen.ToString(System.Globalization.CultureInfo.InvariantCulture);
                    return new FilterPreviewEngine.Reply(metadata, cached.Data);
                }
            }
            var reply = await session.Engine.RequestAsync(cmd, request.Gen, true, cancellation).ConfigureAwait(false);
            if (cacheKey != null && session.Generation == request.Gen && session.JpegCache.TryAdd(cacheKey, reply))
            {
                session.JpegKeys.Enqueue(cacheKey);
                while (session.JpegCache.Count > 8 && session.JpegKeys.TryDequeue(out var old)) session.JpegCache.TryRemove(old, out _);
            }
            return reply;
        }

        public async Task StreamViewAsync(string id, FilterPreviewViewRequest request,
            Func<FilterPreviewEngine.ViewRecord, CancellationToken, Task> deliver, CancellationToken cancellation)
        {
            var session = Get(id);
            var command = request.ToCommand();
            Task streaming;
            await session.ConfigLock.WaitAsync(cancellation).ConfigureAwait(false);
            try
            {
                ValidateKind(session, request.Gen, encoder: true);
                streaming = session.Engine.StreamViewAsync(command, request.Gen, deliver, cancellation);
            }
            finally { session.ConfigLock.Release(); }
            await streaming.ConfigureAwait(false);
        }

        private static void ValidateKind(Session session, long generation, bool encoder)
        {
            if (generation <= 0 || generation != session.Generation || generation != session.CommittedGeneration)
                throw new ArgumentException("設定の世代が一致しません");
            if (ProfileSettingExtensions.GetFilterEncoderType(session.Profile.FilterOption).HasValue != encoder)
                throw new ArgumentException(encoder ? "エンコーダフィルタが設定されていません" : "AviSynthフィルタが設定されていません");
        }

        public async Task<FilterPreviewConfigRequest> GetConfigurationAsync(string id, CancellationToken cancellation)
        {
            var session = Get(id);
            await session.ConfigLock.WaitAsync(cancellation).ConfigureAwait(false);
            try
            {
                var profile = ServerSupport.DeepCopy(session.Profile);
                return new FilterPreviewConfigRequest
                {
                    Gen = session.Generation, FilterOption = profile.FilterOption,
                    FilterSetting = profile.FilterSetting, EncoderFilterSetting = profile.EncoderFilterSetting,
                    QSVEncFilterOption = profile.QSVEncFilterOption, NVEncFilterOption = profile.NVEncFilterOption,
                    VCEEncFilterOption = profile.VCEEncFilterOption
                };
            }
            finally { session.ConfigLock.Release(); }
        }

        public async Task<ProfileSetting> GetEditedProfileAsync(string id, CancellationToken cancellation, bool preserveCustomFilter = false)
        {
            var session = Get(id);
            await session.ConfigLock.WaitAsync(cancellation).ConfigureAwait(false);
            try
            {
                var profile = ServerSupport.DeepCopy(session.Profile);
                // カスタムを表示用に置き換えただけなら、保存では元の指定を維持する。
                if (preserveCustomFilter && session.OriginalFilterOption == FilterOption.Custom)
                    profile.FilterOption = FilterOption.Custom;
                return profile;
            }
            finally { session.ConfigLock.Release(); }
        }

        public string GetSourceName(string id) => Get(id, false).SourceName;

        public string[] GetLogs(string id) => Get(id, false).Engine.GetLogs();

        public bool Close(string id) { if (!sessions.TryRemove(id, out var session)) return false; session.Dispose(); return true; }
        private void Cleanup()
        {
            long cutoff = timing.Clock.GetUtcNow().Subtract(Ttl).UtcTicks;
            foreach (var item in sessions)
                if (Interlocked.Read(ref item.Value.LastUsed) < cutoff) Close(item.Key);
        }
        public void Dispose() { timer.Dispose(); foreach (var item in sessions) Close(item.Key); }
    }
}
