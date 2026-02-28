using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using Amatsukaze.Lib;
using Amatsukaze.Shared;

namespace Amatsukaze.Server.Rest
{
    public sealed class TrimAdjustSession : IDisposable
    {
        // フレーム番号ごとのペアキャッシュ項目。
        // 映像JPEGと波形JPEGを同時に保持して「同一フレームのペア」を保証する。
        private sealed class CacheEntry
        {
            public int FrameNumber { get; set; }
            public byte[] VideoJpeg { get; set; } = Array.Empty<byte>();
            public byte[] WaveformJpeg { get; set; } = Array.Empty<byte>();
            public DateTime LastAccessUtc { get; set; }
        }

        // AviSynth環境 + AMTSource + JPEG変換処理を1セットとして持つ実行コンテキスト。
        // 1コンテキスト内は lock で直列化し、コンテキストを複数持つことで並列性を確保する。
        private sealed class DecodeBundleContext : IDisposable
        {
            private readonly object syncRoot = new object();
            private readonly AMTContext ctx;
            private readonly TrimAdjust trimadj;

            public int NumFrames => trimadj.NumFrames;
            public int Width => trimadj.Width;
            public int Height => trimadj.Height;

            public DecodeBundleContext(string datFilePath, int scaleMode)
            {
                ctx = new AMTContext();
                trimadj = new TrimAdjust(ctx, datFilePath, scaleMode);
            }

            public CacheEntry DecodePair(int frameNumber)
            {
                lock (syncRoot)
                {
                    var video = trimadj.GetFrameJpeg(frameNumber);
                    if (video == null || video.Length == 0)
                    {
                        throw new IOException($"フレームJPEG取得に失敗しました: n={frameNumber}");
                    }
                    var waveform = trimadj.GetWaveformJpeg(frameNumber) ?? Array.Empty<byte>();
                    return new CacheEntry
                    {
                        FrameNumber = frameNumber,
                        VideoJpeg = video,
                        WaveformJpeg = waveform,
                        LastAccessUtc = DateTime.UtcNow
                    };
                }
            }

            public bool GetFrameInfo(int frameNumber, out long pts, out long duration, out int keyFrame, out int cmType)
            {
                lock (syncRoot)
                {
                    return trimadj.GetFrameInfo(frameNumber, out pts, out duration, out keyFrame, out cmType);
                }
            }

            public void Dispose()
            {
                trimadj?.Dispose();
                ctx?.Dispose();
            }
        }

        // キャッシュサイズと先読み半径は少し積極寄りに設定。
        // サーバーCPU/メモリを使ってでもスクロール応答を優先する。
        private const int MaxCacheEntries = 320;
        private const int PrefetchRadiusDefault = 20;
        private const int PrefetchRadiusExpanded = 36;
        private const int PrefetchRadiusJump = 10;

        public string Id { get; }
        public int QueueItemId { get; }
        public string SrcPath { get; }
        public string TempDir { get; }
        public DateTime LastAccessUtc => new DateTime(Interlocked.Read(ref lastAccessTicks), DateTimeKind.Utc);

        private readonly DecodeBundleContext onDemandContext;
        private readonly DecodeBundleContext prefetchForwardContext;
        private readonly DecodeBundleContext prefetchBackwardContext;

        private readonly object cacheLock = new object();
        private readonly Dictionary<int, CacheEntry> cache = new Dictionary<int, CacheEntry>();
        private readonly ConcurrentDictionary<int, Lazy<Task<CacheEntry>>> inFlight = new ConcurrentDictionary<int, Lazy<Task<CacheEntry>>>();

        private readonly CancellationTokenSource prefetchCts = new CancellationTokenSource();
        private readonly SemaphoreSlim prefetchForwardSignal = new SemaphoreSlim(0);
        private readonly SemaphoreSlim prefetchBackwardSignal = new SemaphoreSlim(0);
        private readonly Task prefetchForwardTask;
        private readonly Task prefetchBackwardTask;

        private int prefetchGeneration;
        private int prefetchCenter = -1;
        private int prefetchRadius = PrefetchRadiusDefault;
        private int lastRequestedFrame = -1;

        private long lastAccessTicks;

        public int NumFrames => onDemandContext.NumFrames;
        public int FrameWidth => onDemandContext.Width;
        public int FrameHeight => onDemandContext.Height;

        public TrimAdjustSession(string id, int queueItemId, string srcPath, string tempDir, string datFilePath, int scaleMode)
        {
            Id = id;
            QueueItemId = queueItemId;
            SrcPath = srcPath;
            TempDir = tempDir;

            onDemandContext = new DecodeBundleContext(datFilePath, scaleMode);
            prefetchForwardContext = new DecodeBundleContext(datFilePath, scaleMode);
            prefetchBackwardContext = new DecodeBundleContext(datFilePath, scaleMode);

            Touch();

            prefetchForwardTask = Task.Run(() => PrefetchWorker(true, prefetchCts.Token));
            prefetchBackwardTask = Task.Run(() => PrefetchWorker(false, prefetchCts.Token));
        }

        public void Touch()
        {
            Interlocked.Exchange(ref lastAccessTicks, DateTime.UtcNow.Ticks);
        }

        public byte[] GetFrameBundle(int frameNumber)
        {
            Touch();
            var entry = EnsurePairCached(frameNumber, onDemandContext, waitForExisting: true, prefetchCts.Token);
            if (entry == null)
            {
                return null;
            }
            RequestPrefetch(frameNumber);
            return BuildBundle(entry);
        }

        public byte[] GetFrameJpeg(int frameNumber)
        {
            Touch();
            var entry = EnsurePairCached(frameNumber, onDemandContext, waitForExisting: true, prefetchCts.Token);
            if (entry == null)
            {
                return null;
            }
            RequestPrefetch(frameNumber);
            return entry.VideoJpeg;
        }

        public bool GetFrameInfo(int frameNumber, out long pts, out long duration, out int keyFrame, out int cmType)
        {
            return onDemandContext.GetFrameInfo(frameNumber, out pts, out duration, out keyFrame, out cmType);
        }

        public byte[] GetWaveformJpeg(int frameNumber)
        {
            Touch();
            var entry = EnsurePairCached(frameNumber, onDemandContext, waitForExisting: true, prefetchCts.Token);
            if (entry == null)
            {
                return null;
            }
            RequestPrefetch(frameNumber);
            return entry.WaveformJpeg;
        }

        private static byte[] BuildBundle(CacheEntry entry)
        {
            using var ms = new MemoryStream(8 + entry.VideoJpeg.Length + entry.WaveformJpeg.Length);
            using var bw = new BinaryWriter(ms, Encoding.UTF8, leaveOpen: true);
            bw.Write(entry.VideoJpeg.Length);
            bw.Write(entry.WaveformJpeg.Length);
            bw.Write(entry.VideoJpeg);
            bw.Write(entry.WaveformJpeg);
            bw.Flush();
            return ms.ToArray();
        }

        private CacheEntry EnsurePairCached(int frameNumber, DecodeBundleContext context, bool waitForExisting, CancellationToken token)
        {
            // 1) まずキャッシュヒットを確認（最短経路）。
            if (TryGetCached(frameNumber, touch: true, out var hit))
            {
                return hit;
            }

            // 2) ミス時は in-flight map で「同一フレーム生成の多重実行」を抑止する。
            var newLazy = new Lazy<Task<CacheEntry>>(
                () => Task.Run(() => BuildAndCache(frameNumber, context, token), token),
                LazyThreadSafetyMode.ExecutionAndPublication);
            var lazy = inFlight.GetOrAdd(frameNumber, newLazy);

            // 3) 先読みは既存ジョブがある場合に待たずスキップして次へ進む。
            if (!waitForExisting && !ReferenceEquals(lazy, newLazy))
            {
                return null;
            }

            try
            {
                var entry = lazy.Value.GetAwaiter().GetResult();
                if (entry != null)
                {
                    entry.LastAccessUtc = DateTime.UtcNow;
                }
                return entry;
            }
            catch
            {
                if (TryGetCached(frameNumber, touch: true, out var fallback))
                {
                    return fallback;
                }
                throw;
            }
            finally
            {
                if (lazy.IsValueCreated && lazy.Value.IsCompleted)
                {
                    inFlight.TryRemove(new KeyValuePair<int, Lazy<Task<CacheEntry>>>(frameNumber, lazy));
                }
            }
        }

        private CacheEntry BuildAndCache(int frameNumber, DecodeBundleContext context, CancellationToken token)
        {
            token.ThrowIfCancellationRequested();
            var entry = context.DecodePair(frameNumber);
            PutCache(entry);
            return entry;
        }

        private bool TryGetCached(int frameNumber, bool touch, out CacheEntry entry)
        {
            lock (cacheLock)
            {
                if (cache.TryGetValue(frameNumber, out entry))
                {
                    if (touch)
                    {
                        entry.LastAccessUtc = DateTime.UtcNow;
                    }
                    return true;
                }
            }
            entry = null;
            return false;
        }

        private bool ContainsCached(int frameNumber)
        {
            lock (cacheLock)
            {
                return cache.ContainsKey(frameNumber);
            }
        }

        private void PutCache(CacheEntry entry)
        {
            lock (cacheLock)
            {
                entry.LastAccessUtc = DateTime.UtcNow;
                cache[entry.FrameNumber] = entry;
                while (cache.Count > MaxCacheEntries)
                {
                    int oldestKey = -1;
                    DateTime oldestTime = DateTime.MaxValue;
                    foreach (var pair in cache)
                    {
                        if (pair.Value.LastAccessUtc < oldestTime)
                        {
                            oldestTime = pair.Value.LastAccessUtc;
                            oldestKey = pair.Key;
                        }
                    }
                    if (oldestKey < 0)
                    {
                        break;
                    }
                    cache.Remove(oldestKey);
                }
            }
        }

        private void RequestPrefetch(int frameNumber)
        {
            // 直近移動量に応じて先読み半径を調整。
            // 連続1フレーム移動は大きめ、ジャンプ後は小さめで即応性を優先する。
            var prev = Interlocked.Exchange(ref lastRequestedFrame, frameNumber);
            var delta = (prev < 0) ? 1 : Math.Abs(frameNumber - prev);
            var radius = delta <= 2 ? PrefetchRadiusExpanded : (delta >= 30 ? PrefetchRadiusJump : PrefetchRadiusDefault);

            // generation を更新して古い先読み計画を無効化する。
            Volatile.Write(ref prefetchCenter, frameNumber);
            Volatile.Write(ref prefetchRadius, radius);
            Interlocked.Increment(ref prefetchGeneration);

            // 前方/後方の両ワーカーを起床させる。
            if (!prefetchCts.IsCancellationRequested)
            {
                prefetchForwardSignal.Release();
                prefetchBackwardSignal.Release();
            }
        }

        private async Task PrefetchWorker(bool forward, CancellationToken token)
        {
            // 前方/後方ワーカーを共通化。
            // forward=true  : n+1..n+R
            // forward=false : n-R..n-1 （昇順で生成して後方側もなるべく前進デコードさせる）
            var signal = forward ? prefetchForwardSignal : prefetchBackwardSignal;
            var context = forward ? prefetchForwardContext : prefetchBackwardContext;

            while (!token.IsCancellationRequested)
            {
                // Step 1: 新しい先読みリクエストが来るまで待機。
                await signal.WaitAsync(token).ConfigureAwait(false);

                while (!token.IsCancellationRequested)
                {
                    // Step 2: 現在の先読み計画（世代/中心/半径）をスナップショットとして読む。
                    var generation = Volatile.Read(ref prefetchGeneration);
                    var center = Volatile.Read(ref prefetchCenter);
                    var radius = Volatile.Read(ref prefetchRadius);
                    if (center < 0 || radius <= 0)
                    {
                        break;
                    }

                    var restart = false;
                    for (var i = 1; i <= radius; i++)
                    {
                        // Step 3: 先読み中に generation が変わったら古い計画は中断して再開する。
                        if (generation != Volatile.Read(ref prefetchGeneration))
                        {
                            restart = true;
                            break;
                        }

                        // Step 4: forward/backward の方針に従って対象フレームを決める。
                        var target = forward ? center + i : center - radius + (i - 1);
                        if (target < 0 || target >= NumFrames)
                        {
                            continue;
                        }

                        // Step 5: 既にキャッシュ済みなら処理しない。
                        if (ContainsCached(target))
                        {
                            continue;
                        }

                        try
                        {
                            // Step 6: 先読みは waitForExisting=false で非ブロッキング投入。
                            // 既存 in-flight があれば待たずに次フレームへ進む。
                            EnsurePairCached(target, context, waitForExisting: false, token);
                        }
                        catch (OperationCanceledException)
                        {
                            return;
                        }
                        catch
                        {
                            // Step 7: 先読み失敗は握りつぶす（オンデマンド要求の遅延を避ける）。
                        }
                    }

                    // Step 8: 先読み中に新計画が来ていなければ一旦待機へ戻る。
                    if (!restart)
                    {
                        break;
                    }
                }
            }
        }

        public void Dispose()
        {
            prefetchCts.Cancel();
            try
            {
                prefetchForwardSignal.Release();
                prefetchBackwardSignal.Release();
            }
            catch
            {
            }

            try
            {
                Task.WaitAll(new[] { prefetchForwardTask, prefetchBackwardTask }, TimeSpan.FromSeconds(1));
            }
            catch
            {
            }

            prefetchForwardSignal.Dispose();
            prefetchBackwardSignal.Dispose();
            prefetchCts.Dispose();

            prefetchBackwardContext?.Dispose();
            prefetchForwardContext?.Dispose();
            onDemandContext?.Dispose();
        }
    }

    public class TrimAdjustService
    {
        private static readonly Regex TempDirRegex = new Regex(@"一時フォルダ\s*[:：]\s*(.+)", RegexOptions.Compiled);
        private static readonly Regex TrimRegex = new Regex(@"Trim\s*\(\s*(\d+)\s*,\s*(\d+)\s*\)", RegexOptions.Compiled);
        private static readonly TimeSpan SessionTtl = TimeSpan.FromMinutes(5);

        private readonly RestStateStore state;
        private readonly ConcurrentDictionary<string, TrimAdjustSession> sessions = new ConcurrentDictionary<string, TrimAdjustSession>();

        public TrimAdjustService(RestStateStore state)
        {
            this.state = state;
        }

        public bool TryCreateSession(TrimAdjustSessionRequest request, out TrimAdjustSessionResponse response, out string error)
        {
            response = null;
            error = null;

            if (request == null || request.QueueItemId <= 0)
            {
                error = "QueueItemIdが不正です";
                return false;
            }

            if (!state.TryGetQueueItem(request.QueueItemId, out var item) || string.IsNullOrEmpty(item.SrcPath))
            {
                error = "キューアイテムが見つかりません";
                return false;
            }

            // ログファイルから一時フォルダを取得
            var logPath = state.ResolveTaskLogPathById(request.QueueItemId);
            if (string.IsNullOrEmpty(logPath) || !File.Exists(logPath))
            {
                error = "ログファイルが見つかりません";
                return false;
            }

            var tempDir = ExtractTempDirFromLog(logPath);
            if (string.IsNullOrEmpty(tempDir))
            {
                error = "ログファイルから一時フォルダが取得できませんでした";
                return false;
            }

            // amts0.datの存在確認
            var datFilePath = Path.Combine(tempDir, "amts0.dat");
            if (!File.Exists(datFilePath))
            {
                error = $"amts0.datが見つかりません: {datFilePath}";
                return false;
            }

            CleanupExpired();

            var sessionId = Guid.NewGuid().ToString("N");
            var scaleMode = request.ScaleMode == 0 || request.ScaleMode == 1 || request.ScaleMode == 2
                ? request.ScaleMode
                : 1;
            try
            {
                var session = new TrimAdjustSession(sessionId, request.QueueItemId, item.SrcPath, tempDir, datFilePath, scaleMode);
                sessions[sessionId] = session;

                // 全フレームPTS情報を取得
                var framePts = new List<double>();
                const double ptsToSeconds = 1.0 / 90000.0; // 90kHz PTS→秒
                for (int i = 0; i < session.NumFrames; i++)
                {
                    if (session.GetFrameInfo(i, out var pts, out _, out _, out _))
                    {
                        framePts.Add(pts * ptsToSeconds);
                    }
                }

                // Trim AVSを読み込み
                var trims = LoadTrims(item.SrcPath, tempDir);

                response = new TrimAdjustSessionResponse
                {
                    SessionId = sessionId,
                    NumFrames = session.NumFrames,
                    FrameWidth = session.FrameWidth,
                    FrameHeight = session.FrameHeight,
                    Trims = trims,
                    FramePts = framePts
                };
                return true;
            }
            catch (Exception ex)
            {
                Util.AddLog($"[TrimAdjust] セッション作成失敗: queueItemId={request.QueueItemId}, srcPath={item?.SrcPath}, tempDir={tempDir}, dat={datFilePath}", ex);
                error = $"セッション作成に失敗しました: {ex.GetType().Name}: {ex.Message}";
                return false;
            }
        }

        public TrimAdjustSession GetSession(string sessionId)
        {
            CleanupExpired();
            if (string.IsNullOrEmpty(sessionId))
            {
                return null;
            }
            sessions.TryGetValue(sessionId, out var session);
            return session;
        }

        public bool RemoveSession(string sessionId)
        {
            if (string.IsNullOrEmpty(sessionId))
            {
                return false;
            }
            if (sessions.TryRemove(sessionId, out var session))
            {
                session.Dispose();
                return true;
            }
            return false;
        }

        // Trimを保存: {srcPath}.trim.avs に書き出し
        public bool TrySaveTrims(string sessionId, TrimSaveRequest request, out string error)
        {
            error = null;
            var session = GetSession(sessionId);
            if (session == null)
            {
                error = "セッションが見つかりません";
                return false;
            }

            if (request?.Trims == null || request.Trims.Count == 0)
            {
                error = "Trimデータが空です";
                return false;
            }

            // バリデーション
            foreach (var trim in request.Trims)
            {
                if (trim.Start < 0 || trim.End < trim.Start || trim.End >= session.NumFrames)
                {
                    error = $"Trim範囲が不正です: ({trim.Start}, {trim.End})";
                    return false;
                }
            }

            try
            {
                var avsPath = session.SrcPath + ".trim.avs";
                var lines = new List<string>();
                var trimParts = new List<string>();
                foreach (var trim in request.Trims)
                {
                    trimParts.Add($"Trim({trim.Start},{trim.End})");
                }
                lines.Add(string.Join(" ++ ", trimParts));
                File.WriteAllLines(avsPath, lines);
                return true;
            }
            catch (Exception ex)
            {
                error = $"Trim保存に失敗しました: {ex.Message}";
                return false;
            }
        }

        // ログファイルから「一時フォルダ: {path}」を抽出
        private static string ExtractTempDirFromLog(string logPath)
        {
            try
            {
                var bytes = File.ReadAllBytes(logPath);
                var content = Util.AmatsukazeDefaultEncoding.GetString(bytes);
                var lines = content.Replace("\r\n", "\n").Replace('\r', '\n').Split('\n');

                string rootedPathFallback = null;
                foreach (var line in lines)
                {
                    var match = TempDirRegex.Match(line);
                    if (match.Success)
                    {
                        var dir = match.Groups[1].Value.Trim().Trim('"');
                        if (string.IsNullOrEmpty(dir))
                        {
                            continue;
                        }
                        if (Directory.Exists(dir))
                        {
                            return dir;
                        }
                        if (Path.IsPathRooted(dir))
                        {
                            rootedPathFallback = dir;
                        }
                    }
                }

                if (!string.IsNullOrEmpty(rootedPathFallback))
                {
                    return rootedPathFallback;
                }
            }
            catch
            {
                // ファイル読み込み失敗は無視
            }
            return null;
        }

        // Trim AVSを読み込み: {srcPath}.trim.avs 優先、なければ {tempDir}/trim0.avs
        private static List<TrimRange> LoadTrims(string srcPath, string tempDir)
        {
            var trims = new List<TrimRange>();

            // {srcPath}.trim.avs を優先
            var avsPath = srcPath + ".trim.avs";
            if (!File.Exists(avsPath))
            {
                avsPath = Path.Combine(tempDir, "trim0.avs");
            }
            if (!File.Exists(avsPath))
            {
                return trims;
            }

            try
            {
                var content = File.ReadAllText(avsPath);
                var matches = TrimRegex.Matches(content);
                foreach (Match match in matches)
                {
                    if (int.TryParse(match.Groups[1].Value, out var start) &&
                        int.TryParse(match.Groups[2].Value, out var end))
                    {
                        trims.Add(new TrimRange { Start = start, End = end });
                    }
                }
            }
            catch
            {
                // パース失敗時は空リストを返す
            }

            return trims;
        }

        private void CleanupExpired()
        {
            var now = DateTime.UtcNow;
            foreach (var pair in sessions)
            {
                if (now - pair.Value.LastAccessUtc > SessionTtl)
                {
                    if (sessions.TryRemove(pair.Key, out var session))
                    {
                        session.Dispose();
                    }
                }
            }
        }
    }
}
