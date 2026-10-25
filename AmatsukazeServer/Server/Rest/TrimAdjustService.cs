using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Diagnostics;
using System.Linq;
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
            private readonly object audioSyncRoot = new object();
            private bool disposed;
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
                    if (disposed) throw new ObjectDisposedException(nameof(DecodeBundleContext));
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
                    if (disposed) throw new ObjectDisposedException(nameof(DecodeBundleContext));
                    return trimadj.GetFrameInfo(frameNumber, out pts, out duration, out keyFrame, out cmType);
                }
            }

            public byte[] GetAudioWav(int startFrame, int endFrame)
            {
                // 映像デコードとは独立して、ネイティブバッファのコピーまで排他する。
                lock (audioSyncRoot)
                {
                    if (disposed) throw new ObjectDisposedException(nameof(DecodeBundleContext));
                    var pcm = trimadj.GetAudioPcm(startFrame, endFrame, out var sampleRate, out var channels);
                    return pcm == null ? null : BuildAudioWav(pcm, sampleRate, channels);
                }
            }

            public void Dispose()
            {
                lock (syncRoot)
                lock (audioSyncRoot)
                {
                    if (disposed) return;
                    disposed = true;
                    trimadj?.Dispose();
                    ctx?.Dispose();
                }
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
        public CmSegAnnotationStore CmSegAnnotation { get; set; }
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

        public byte[] GetAudioWav(int startFrame, int endFrame)
        {
            Touch();
            return onDemandContext.GetAudioWav(startFrame, endFrame);
        }

        internal static byte[] BuildAudioWav(byte[] pcm, int sampleRate, int channels)
        {
            if (pcm == null || sampleRate <= 0 || channels != 2 || pcm.Length % (channels * 2) != 0)
                throw new ArgumentException("音声PCMの形式が不正です");
            using var ms = new MemoryStream(checked(44 + pcm.Length));
            using var writer = new BinaryWriter(ms, Encoding.ASCII, leaveOpen: true);
            writer.Write(Encoding.ASCII.GetBytes("RIFF"));
            writer.Write(checked(36 + pcm.Length));
            writer.Write(Encoding.ASCII.GetBytes("WAVEfmt "));
            writer.Write(16);
            writer.Write((short)1);
            writer.Write((short)channels);
            writer.Write(sampleRate);
            writer.Write(checked(sampleRate * channels * 2));
            writer.Write((short)(channels * 2));
            writer.Write((short)16);
            writer.Write(Encoding.ASCII.GetBytes("data"));
            writer.Write(pcm.Length);
            writer.Write(pcm);
            writer.Flush();
            return ms.ToArray();
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
                // キャンセル後にワーカーが WaitAsync で詰まらないようシグナルを解放する
                prefetchForwardSignal.Release();
                prefetchBackwardSignal.Release();
            }
            catch
            {
            }

            // ネイティブデコード中のタスクが use-after-free を起こさないよう、
            // タイムアウトなしでタスクの完了を待ってからコンテキストを破棄する
            try
            {
                Task.WaitAll(prefetchForwardTask, prefetchBackwardTask);
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

    public class TrimAdjustService : IDisposable
    {
        public static bool AudioPlaybackEnabled => Environment.GetEnvironmentVariable("AMT_TRIM_AUDIO_PLAYBACK") == "1";
        private static readonly Regex TempDirRegex = new Regex(@"一時フォルダ\s*[:：]\s*(.+)", RegexOptions.Compiled);
        private static readonly Regex TrimRegex = new Regex(@"Trim\s*\(\s*(\d+)\s*,\s*(\d+)\s*\)", RegexOptions.Compiled);
        // jls0.txt: "開始 終了 秒数 端数フレーム ロゴ秒数 :ラベル"(ラベルなしの旧形式もあり)
        private static readonly Regex JlsLineRegex = new Regex(@"^\s*(\d+)\s+(\d+)\s+(\d+)\s+(-?\d+)\s+(\d+)(?:\s*:(\S*))?", RegexOptions.Compiled);
        private static readonly TimeSpan SessionTtl = TimeSpan.FromMinutes(5);

        private readonly EncodeServer server;
        private readonly RestStateStore state;
        private readonly ConcurrentDictionary<string, TrimAdjustSession> sessions = new ConcurrentDictionary<string, TrimAdjustSession>();
        // TTL経過後に確実にクリーンアップされるよう、SessionTtl間隔でバックグラウンド実行する
        private readonly Timer cleanupTimer;
        // 復元先はサービス内で保持する。ログ・キューへの新規永続項目は不要。
        private readonly TrimAdjustTempDirRegistry restoredTempDirs;
        private readonly object tempDirMutationLock = new object();
        // 元ブランチと同じく、全体で復元は1件だけ。追加要求を待ち行列に積まない。
        private readonly object _restoreLock = new object();
        private (int queueItemId, int scaleMode)? _restoringKey = null;
        private readonly CancellationTokenSource _restoreLifetimeCts = new CancellationTokenSource();
        private Task<(string restoredDir, string error)> _restoreJobTask;
        private bool _disposed = false;

        internal TrimAdjustService(EncodeServer server, RestStateStore state, TrimAdjustTempDirRegistry restoredTempDirs)
        {
            this.server = server;
            this.state = state;
            this.restoredTempDirs = restoredTempDirs ?? throw new ArgumentNullException(nameof(restoredTempDirs));
            cleanupTimer = new Timer(_ => CleanupExpired(), null, SessionTtl, SessionTtl);
        }

        public bool TryCreateSession(TrimAdjustSessionRequest request, out TrimAdjustSessionResponse response, out string error)
            => TryCreateSession(request, out response, out error, out _);

        public bool TryCreateSession(TrimAdjustSessionRequest request, out TrimAdjustSessionResponse response,
            out string error, out string errorCode)
            => TryCreateSession(request, ResolveTempDir(request?.QueueItemId ?? 0), out response, out error, out errorCode);

        private bool TryCreateSession(TrimAdjustSessionRequest request, string tempDir,
            out TrimAdjustSessionResponse response, out string error, out string errorCode)
        {
            lock (tempDirMutationLock)
                return TryCreateSessionCore(request, tempDir, out response, out error, out errorCode);
        }
        private bool TryCreateSessionCore(TrimAdjustSessionRequest request, string tempDir,
            out TrimAdjustSessionResponse response, out string error, out string errorCode)
        {
            response = null;
            error = null;
            errorCode = null;

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

            // 復元で作ったフォルダーを直接受け取り、ログから探し直さない。
            if (string.IsNullOrEmpty(tempDir))
            {
                error = "amts0.datが見つかりません（一時キャッシュがありません）";
                errorCode = RestApiErrorCodes.TrimCacheUnavailable;
                return false;
            }

            // amts0.datの存在確認
            var datFilePath = Path.Combine(tempDir, "amts0.dat");
            if (!File.Exists(datFilePath))
            {
                error = $"amts0.datが見つかりません: {datFilePath}";
                errorCode = RestApiErrorCodes.TrimCacheUnavailable;
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
                var cmSegEnabled = CmSegAnnotationStore.IsEnabled;
                var durationCounts = new Dictionary<long, int>();
                const double ptsToSeconds = 1.0 / 90000.0; // 90kHz PTS→秒
                for (int i = 0; i < session.NumFrames; i++)
                {
                    if (session.GetFrameInfo(i, out var pts, out var duration, out _, out _))
                    {
                        framePts.Add(pts * ptsToSeconds);
                        if (cmSegEnabled && duration > 0)
                        {
                            durationCounts.TryGetValue(duration, out var count);
                            durationCounts[duration] = count + 1;
                        }
                    }
                }

                // Trim AVSを読み込み
                var trims = LoadTrims(item.SrcPath, tempDir);
                var divisionPoints = LoadDivisionPoints(item.SrcPath, tempDir, session.NumFrames);

                var jlsSegments = LoadJlsSegments(tempDir);
                string cmSegError = null;
                if (cmSegEnabled)
                {
                    session.CmSegAnnotation = CmSegAnnotationStore.OpenForSession(item.SrcPath, tempDir, session.NumFrames,
                        durationCounts, jlsSegments, out cmSegError);
                    if (cmSegError != null) Util.AddLog("[CmSeg] " + cmSegError, null);
                    if (session.CmSegAnnotation?.Get()?.Warning is string warning)
                        Util.AddLog("[CmSeg] " + warning, null);
                }

                response = new TrimAdjustSessionResponse
                {
                    SessionId = sessionId,
                    NumFrames = session.NumFrames,
                    FrameWidth = session.FrameWidth,
                    FrameHeight = session.FrameHeight,
                    Trims = trims,
                    DivisionPoints = divisionPoints,
                    FramePts = framePts,
                    JlsSegments = jlsSegments,
                    AudioPlaybackEnabled = AudioPlaybackEnabled,
                    CmSegAnnotationEnabled = session.CmSegAnnotation != null,
                    CmSegAnnotationError = cmSegError
                };
                return true;
            }
            catch (Exception ex)
            {
                RemoveSession(sessionId);
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

        // CM解析タスクの一時フォルダを削除する。
        // キューからの削除は呼び出し元が別途行うこと。
        // 成功時はtrue、フォルダが存在しない・取得できない場合もtrueを返す（エラーがなければ成功扱い）。
        // 失敗時はfalseと理由をerrorに設定する。
        public bool TryDeleteQueueTaskTempDirs(QueueItem item, string loggedWorkDir, IEnumerable<QueueItem> remainingOwners, out bool handled, out string error)
        {
            lock (tempDirMutationLock)
            {
                handled = false;
                error = null;
                if (item == null || string.IsNullOrWhiteSpace(item.SrcPath))
                {
                    error = "キューアイテムまたは録画元パスが不正です";
                    return false;
                }
                lock (_restoreLock)
                {
                    if (_restoringKey.HasValue && _restoringKey.Value.queueItemId == item.Id)
                    {
                        error = "一時ファイルを復元中のためフォルダー削除を保留しました";
                        handled = true;
                        return false;
                    }
                }
                var paths = restoredTempDirs.GetPathsForSource(item.SrcPath);
                if (!string.IsNullOrWhiteSpace(loggedWorkDir)) paths.Add(loggedWorkDir);
                return restoredTempDirs.TryDeletePaths(item.SrcPath, paths, remainingOwners,
                    ReleaseSessionsForTempDir, out handled, out error);
            }
        }

        internal bool ReleaseSessionsForTempDir(string tempDir)
        {
            var comparison = OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
            foreach (var pair in sessions.Where(pair => string.Equals(Path.GetFullPath(pair.Value.TempDir), Path.GetFullPath(tempDir), comparison)).ToArray())
            {
                RemoveSession(pair.Key);
            }
            return !sessions.Values.Any(session => string.Equals(Path.GetFullPath(session.TempDir), Path.GetFullPath(tempDir), comparison));
        }
        public bool TryDeleteCmTaskTempDir(int queueItemId, out string error)
        {
            lock (tempDirMutationLock)
                return TryDeleteCmTaskTempDirCore(queueItemId, out error);
        }
        private bool TryDeleteCmTaskTempDirCore(int queueItemId, out string error)
        {
            error = null;
            lock (_restoreLock)
            {
                if (_restoringKey.HasValue && _restoringKey.Value.queueItemId == queueItemId)
                {
                    error = "一時ファイルを復元中のためフォルダー削除を保留しました";
                    return false;
                }
            }

            if (!state.TryGetQueueItem(queueItemId, out var item))
            {
                error = "キューアイテムが見つかりません";
                return false;
            }

            // CMCheck済みアイテムのみ対象
            if (item.Mode != Amatsukaze.Server.ProcMode.CMCheck)
            {
                error = "CM解析タスク以外は削除できません";
                return false;
            }

            var comparison = OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
            var sourceIdentity = Path.GetFullPath(item.SrcPath);
            var shared = server.GetQueueItemsSnapshot().Any(other => other != null && other.Id != item.Id
                && (Path.GetFullPath(other.SrcPath).Equals(sourceIdentity, comparison)
                    || (!string.IsNullOrWhiteSpace(item.ResumeDir) && !string.IsNullOrWhiteSpace(other.ResumeDir)
                        && Path.GetFullPath(other.ResumeDir).Equals(Path.GetFullPath(item.ResumeDir), comparison))));
            if (shared)
            {
                error = "同じ録画または再利用フォルダーを別キューが参照中です";
                return false;
            }
            // 処理中の場合は一時フォルダを削除できない
            if (item.State == Amatsukaze.Server.QueueState.Encoding)
            {
                error = "処理中のタスクの一時フォルダは削除できません";
                return false;
            }

            var tempDir = ResolveTempDir(queueItemId);
            if (string.IsNullOrEmpty(tempDir))
            {
                return true;
            }
            var sharedResumeDir = server.GetQueueItemsSnapshot().Any(other => other != null && other.Id != item.Id
                && !string.IsNullOrWhiteSpace(other.ResumeDir)
                && Path.GetFullPath(other.ResumeDir).Equals(Path.GetFullPath(tempDir), comparison));
            if (sharedResumeDir)
            {
                error = "別キューのResumeDirとして使用中の一時フォルダーは削除できません";
                return false;
            }

            if (!Directory.Exists(tempDir))
            {
                // 既に存在しない場合はスキップ
                Util.AddLog($"[TrimAdjust] CM解析タスク({queueItemId})の一時フォルダは既に存在しません: {tempDir}", null);
                return true;
            }

            try
            {
                restoredTempDirs.MarkDeletePending(item.SrcPath, tempDir);
                foreach (var pair in sessions.Where(pair => pair.Value.TempDir == tempDir))
                {
                    RemoveSession(pair.Key);
                }
                Directory.Delete(tempDir, true);
                restoredTempDirs.CompleteDelete(item.SrcPath, tempDir);
                Util.AddLog($"[TrimAdjust] CM解析タスク({queueItemId})の一時フォルダを削除しました: {tempDir}", null);
                return true;
            }
            catch (Exception ex)
            {
                error = $"一時フォルダの削除に失敗しました: {ex.Message}";
                Util.AddLog($"[TrimAdjust] CM解析タスク({queueItemId})の一時フォルダ削除に失敗しました: {tempDir}", ex);
                return false;
            }
        }

        // 変更されたTrimと分割点だけを保存する
        public bool TrySaveTrims(string sessionId, TrimSaveRequest request, out string error)
        {
            error = null;
            var session = GetSession(sessionId);
            if (session == null)
            {
                error = "セッションが見つかりません";
                return false;
            }

            if (request == null || (!request.SaveTrims && !request.SaveDivisionPoints))
            {
                error = "保存対象が指定されていません";
                return false;
            }

            if (request.SaveTrims && (request.Trims == null || request.Trims.Count == 0))
            {
                error = "Trimデータが空です";
                return false;
            }

            // バリデーション
            if (request.SaveTrims)
            {
                foreach (var trim in request.Trims)
                {
                    if (trim.Start < 0 || trim.End < trim.Start || trim.End >= session.NumFrames)
                    {
                        error = $"Trim範囲が不正です: ({trim.Start}, {trim.End})";
                        return false;
                    }
                }
            }

            var divisionPoints = new SortedSet<int>();
            if (request.SaveDivisionPoints && request.DivisionPoints != null)
            {
                foreach (var point in request.DivisionPoints)
                {
                    if (point <= 0 || point >= session.NumFrames)
                    {
                        error = $"分割点が範囲外です: {point}";
                        return false;
                    }
                    divisionPoints.Add(point);
                }
            }

            try
            {
                if (request.SaveTrims)
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
                }

                if (request.SaveDivisionPoints)
                {
                    SaveDivisionPoints(session.SrcPath, divisionPoints);
                }
                return true;
            }
            catch (Exception ex)
            {
                error = $"カット情報の保存に失敗しました: {ex.Message}";
                return false;
            }
        }

        public async Task<TrimRequeueResponse> RequeueAsync(TrimRequeueRequest request)
        {
            if (request == null || request.QueueItemId <= 0)
            {
                throw new ArgumentException("QueueItemIdが不正です");
            }
            if (string.IsNullOrWhiteSpace(request.Profile))
            {
                throw new ArgumentException("プロファイルが指定されていません");
            }
            if (!state.TryGetQueueItem(request.QueueItemId, out var sourceItem))
            {
                throw new InvalidOperationException("元のキューアイテムが見つかりません");
            }
            if (sourceItem.State != Amatsukaze.Server.QueueState.Complete)
            {
                throw new InvalidOperationException("完了していないキューアイテムは再投入できません");
            }
            if (sourceItem.Mode != Amatsukaze.Server.ProcMode.CMCheck && !sourceItem.IsBatch)
            {
                throw new InvalidOperationException("CM解析またはエンコード済みのキューアイテムだけ再投入できます");
            }
            // 移動済みの入力ファイルはキューへの再投入時に元の場所へ戻す
            if (string.IsNullOrEmpty(sourceItem.SrcPath))
            {
                throw new InvalidOperationException("入力ファイルが見つかりません");
            }

            var tempDir = ResolveTempDir(sourceItem.Id);
            string resumeDir = !string.IsNullOrEmpty(tempDir) && File.Exists(Path.Combine(tempDir, "resume.dat"))
                ? tempDir : null;
            if (string.IsNullOrEmpty(resumeDir))
            {
                Util.AddLog($"[TrimAdjust] 再開情報が見つからないため通常処理として再投入します: ItemId={sourceItem.Id}", null);
            }

            var newItem = await server.RequeueTrimItem(
                sourceItem.Id,
                request.Profile,
                request.Priority,
                request.Tags,
                resumeDir,
                request.RemoveSourceItem);
            return new TrimRequeueResponse
            {
                QueueItemId = newItem.Id,
                ReuseTempDir = !string.IsNullOrEmpty(resumeDir)
            };
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
                avsPath = ResolveMovedCutInfoPath(srcPath, ".trim.avs") ?? Path.Combine(tempDir, "trim0.avs");
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

        // join_logo_scpの構成区間を読み込み: {tempDir}/jls0.txt
        // JlsKeepはjls出力のTrim({tempDir}/trim0.avs)で区間中央が残るかどうか
        private static List<JlsSegment> LoadJlsSegments(string tempDir)
        {
            var segments = new List<JlsSegment>();
            var jlsPath = Path.Combine(tempDir, "jls0.txt");
            if (!File.Exists(jlsPath))
            {
                return segments;
            }

            try
            {
                foreach (var line in File.ReadLines(jlsPath))
                {
                    var m = JlsLineRegex.Match(line);
                    if (!m.Success)
                    {
                        continue;
                    }
                    var start = int.Parse(m.Groups[1].Value);
                    var end = int.Parse(m.Groups[2].Value);
                    if (end < start)
                    {
                        continue;
                    }
                    segments.Add(new JlsSegment
                    {
                        Start = start,
                        End = end,
                        Seconds = int.Parse(m.Groups[3].Value),
                        FrameDiff = int.Parse(m.Groups[4].Value),
                        LogoSeconds = int.Parse(m.Groups[5].Value),
                        Label = m.Groups[6].Success ? m.Groups[6].Value : ""
                    });
                }

                // {srcPath}.trim.avs ではなく jls自身の出力を基準にする
                var jlsTrims = new List<TrimRange>();
                var trimPath = Path.Combine(tempDir, "trim0.avs");
                if (File.Exists(trimPath))
                {
                    foreach (Match match in TrimRegex.Matches(File.ReadAllText(trimPath)))
                    {
                        jlsTrims.Add(new TrimRange
                        {
                            Start = int.Parse(match.Groups[1].Value),
                            End = int.Parse(match.Groups[2].Value)
                        });
                    }
                }
                foreach (var seg in segments)
                {
                    var mid = (seg.Start + seg.End) / 2;
                    seg.JlsKeep = jlsTrims.Exists(t => t.Start <= mid && mid <= t.End);
                }
            }
            catch
            {
                // 読み込み失敗時は表示しないだけなので空で返す
                segments.Clear();
            }
            return segments;
        }

        // 分割点を読み込み: 1行につき1フレーム番号
        private static List<int> LoadDivisionPoints(string srcPath, string tempDir, int numFrames)
        {
            var points = new SortedSet<int>();
            var divPath = srcPath + ".div.txt";
            if (!File.Exists(divPath))
            {
                divPath = ResolveMovedCutInfoPath(srcPath, ".div.txt") ?? Path.Combine(tempDir, "div0.txt");
            }
            if (!File.Exists(divPath))
            {
                return new List<int>();
            }

            var lineNumber = 0;
            foreach (var rawLine in File.ReadLines(divPath))
            {
                lineNumber++;
                var line = rawLine.Trim();
                if (line.Length == 0)
                {
                    continue;
                }
                if (!int.TryParse(line, out var point))
                {
                    throw new FormatException($"分割点ファイルの{lineNumber}行目が整数ではありません: {line}");
                }
                // JLSの出力には先頭・終端が含まれることがあるため、編集点からは除外する
                if (point == 0 || point == numFrames)
                {
                    continue;
                }
                if (point < 0 || point > numFrames)
                {
                    throw new FormatException($"分割点ファイルの{lineNumber}行目が範囲外です: {point}");
                }
                points.Add(point);
            }
            return new List<int>(points);
        }

        private static void SaveDivisionPoints(string srcPath, SortedSet<int> points)
        {
            var divPath = srcPath + ".div.txt";
            var lines = new List<string>();
            foreach (var point in points)
            {
                lines.Add(point.ToString());
            }
            File.WriteAllLines(divPath, lines);
        }

        private static string ResolveMovedCutInfoPath(string srcPath, string suffix)
        {
            if (ServerSupport.TryResolveInputFilePath(srcPath, out var actualPath)
                && actualPath != srcPath && File.Exists(actualPath + suffix))
            {
                return actualPath + suffix;
            }
            return null;
        }

        private string ResolveTempDir(int queueItemId)
        {
            if (!state.TryGetQueueItem(queueItemId, out var item)) return null;
            var restored = restoredTempDirs.GetRestoredPath(item.SrcPath);
            if (!string.IsNullOrWhiteSpace(restored) && File.Exists(Path.Combine(restored, "amts0.dat"))) return restored;
            if (!string.IsNullOrEmpty(item.ResumeDir) && File.Exists(Path.Combine(item.ResumeDir, "amts0.dat")))
                return item.ResumeDir;
            // 既存の一時キャッシュの探索にのみ、ログを任意で利用する。
            var logPath = state.ResolveTaskLogPathById(queueItemId);
            var legacy = !string.IsNullOrEmpty(logPath) && File.Exists(logPath) ? ExtractTempDirFromLog(logPath) : null;
            return !string.IsNullOrEmpty(legacy) && File.Exists(Path.Combine(legacy, "amts0.dat")) ? legacy : null;
        }

        // 復元ジョブはHTTP要求より長く生存する。要求tokenは結果を待つ側だけに使う。
        public async Task<(TrimAdjustSessionResponse response, string error)> TryRestoreAndCreateSessionAsync(
            int queueItemId, int scaleMode, CancellationToken ct = default)
        {
            if (ct.IsCancellationRequested) return (null, "復元要求の待機が中断されました");
            var request = new TrimAdjustSessionRequest { QueueItemId = queueItemId, ScaleMode = scaleMode };
            var existing = ResolveTempDir(queueItemId);
            if (existing != null)
                return TryCreateSession(request, existing, out var ready, out var readyError, out _) ? (ready, null) : (null, readyError);

            Task<(string restoredDir, string error)> restoreTask;
            lock (tempDirMutationLock)
            {
                lock (_restoreLock)
                {
                    if (_disposed) return (null, "復元サービスは停止中です");
                    if (_restoringKey != null)
                    {
                        var current = _restoringKey.Value;
                        if (current.queueItemId == queueItemId && current.scaleMode == scaleMode)
                            return (null, $"queueItemId={queueItemId} は現在キャッシュ復元中です。完了後にリトライしてください");
                        return (null, $"別のキャッシュ復元が実行中です (queueItemId={current.queueItemId})。完了後にリトライしてください");
                    }
                    _restoringKey = (queueItemId, scaleMode);
                    restoreTask = Task.Run(() => RestoreTempDirAsync(queueItemId, scaleMode, _restoreLifetimeCts.Token));
                    _restoreJobTask = restoreTask;
                    _ = restoreTask.ContinueWith(task =>
                    {
                        try { Util.AddLog($"[TrimAdjust] 復元ジョブ予期しない例外: ItemId={queueItemId}", task.Exception); }
                        catch { /* 例外を観測する継続処理から例外を漏らさない */ }
                    }, CancellationToken.None, TaskContinuationOptions.OnlyOnFaulted | TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);
                }
            }

            try
            {
                var (restoredDir, error) = await restoreTask.WaitAsync(ct);
                if (restoredDir == null) return (null, error ?? "一時ファイルの再生成に失敗しました");
                return TryCreateSession(request, restoredDir, out var response, out var sessionError, out _)
                    ? (response, null) : (null, sessionError);
            }
            catch (OperationCanceledException) when (ct.IsCancellationRequested)
            {
                // HTTP切断や待機期限はCLI復元へ伝播しない。ジョブはregistry登録まで継続する。
                Util.AddLog($"[TrimAdjust] 復元要求の待機終了: ItemId={queueItemId}。復元ジョブは継続します", null);
                return (null, "復元要求の待機が終了しました。復元処理は継続中です。完了後にリトライしてください");
            }
        }

        private async Task<(string restoredDir, string error)> RestoreTempDirAsync(
            int queueItemId, int scaleMode, CancellationToken jobCt)
        {
            string ownedDir = null;
            string ownedRoot = null;
            string ownedSourcePath = null;
            bool keepCache = false;
            try
            {
                jobCt.ThrowIfCancellationRequested();
                if (queueItemId <= 0 || !state.TryGetQueueItem(queueItemId, out var item) || string.IsNullOrEmpty(item.SrcPath))
                    return (null, "キューアイテムが見つかりません");
                if (item.State != QueueState.Complete || (!item.IsBatch && item.Mode != ProcMode.CMCheck))
                    return (null, "完了したエンコードまたはCM解析タスクだけ復元できます");
                ownedSourcePath = item.SrcPath;
                var existing = ResolveTempDir(item.Id);
                if (existing != null) return (existing, null);
                if (!ServerSupport.TryResolveInputFilePath(item.SrcPath, out var actualSrcPath))
                    return (null, "復元元のTSファイルが見つかりません");
                var setting = state.GetSetting();
                if (string.IsNullOrWhiteSpace(setting?.AmatsukazePath)) return (null, "AmatsukazeCLIが設定されていません");
                var workRoot = Path.GetFullPath(setting.ActualWorkPath);
                ownedRoot = workRoot;
                ownedDir = Path.Combine(workRoot, "amt-trim-" + Guid.NewGuid().ToString("N"));
                Directory.CreateDirectory(ownedDir);
                var resumePath = item.SrcPath + ".resume.dat";
                if (!File.Exists(resumePath) && File.Exists(actualSrcPath + ".resume.dat")) resumePath = actualSrcPath + ".resume.dat";
                var hasResume = File.Exists(resumePath);
                var psi = new ProcessStartInfo(setting.AmatsukazePath)
                {
                    UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true,
                    StandardOutputEncoding = Util.AmatsukazeDefaultEncoding, StandardErrorEncoding = Util.AmatsukazeDefaultEncoding,
                    CreateNoWindow = true, WorkingDirectory = Directory.GetCurrentDirectory()
                };
                if (item.StreamFormat != VideoStreamFormat.MPEG2 && item.StreamFormat != VideoStreamFormat.H264)
                    psi.ArgumentList.Add("--loadv2");
                foreach (var arg in new[] { "--mode", "reform_only", "-i", actualSrcPath, "-s",
                    item.ServiceId.ToString(System.Globalization.CultureInfo.InvariantCulture), "-w", workRoot,
                    "--tmpdir", ownedDir, "--no-remove-tmp" }) psi.ArgumentList.Add(arg);
                if (hasResume)
                {
                    psi.ArgumentList.Add("--restore-info");
                    psi.ArgumentList.Add(resumePath);
                }
                Util.AddLog($"[TrimAdjust] 復元開始: ItemId={item.Id}, Src={actualSrcPath}, Resume={resumePath}, HasResume={hasResume}, Temp={ownedDir}", null);
                var (exitCode, output) = await RunRestoreProcessAsync(psi, jobCt);
                jobCt.ThrowIfCancellationRequested();
                Util.AddLog($"[TrimAdjust] 復元実行結果: ItemId={item.Id}, ExitCode={exitCode}\n{output}", null);
                if (exitCode != 0) return (null, $"一時ファイルの再生成に失敗しました（終了コード: {exitCode}）");
                if (!hasResume)
                {
                    // 古い録画はログを代替の復元元にできる。ログがなくても映像の表示は可能。
                    var trimLine = ExtractTrimLineFromLog(state.ResolveTaskLogPathById(item.Id));
                    if (!string.IsNullOrEmpty(trimLine)) File.WriteAllText(Path.Combine(ownedDir, "trim0.avs"), trimLine);
                }
                jobCt.ThrowIfCancellationRequested();
                restoredTempDirs.RegisterRestored(item.SrcPath, ownedDir);
                keepCache = true;
                return (ownedDir, null);
            }
            catch (OperationCanceledException) when (jobCt.IsCancellationRequested)
            {
                return (null, "サーバー停止により一時ファイルの再生成が中断されました");
            }
            catch (Exception ex)
            {
                Util.AddLog($"[TrimAdjust] 復元失敗: ItemId={queueItemId}, Temp={ownedDir}", ex);
                return (null, $"一時ファイルの復元に失敗しました: {ex.Message}");
            }
            finally
            {
                try
                {
                    if (!keepCache && ownedDir != null) CleanupFailedRestore(ownedSourcePath, ownedDir, ownedRoot);
                }
                finally { lock (_restoreLock) { _restoringKey = null; } }
            }
        }
        private static async Task<(int exitCode, string output)> RunRestoreProcessAsync(ProcessStartInfo psi, CancellationToken ct)
        {
            using var process = Process.Start(psi);
            if (process == null) return (-1, "AmatsukazeCLIを起動できません");
            var stdout = process.StandardOutput.ReadToEndAsync();
            var stderr = process.StandardError.ReadToEndAsync();
            try
            {
                await process.WaitForExitAsync(ct);
            }
            catch (OperationCanceledException)
            {
                // 子プロセスを停止してからフォルダーを削除できる状態にする。
                if (!process.HasExited) process.Kill(entireProcessTree: true);
                await process.WaitForExitAsync();
                await Task.WhenAll(stdout, stderr);
                throw;
            }
            return (process.ExitCode, await stdout + await stderr);
        }

        private void CleanupFailedRestore(string sourcePath, string path, string workRoot)
        {
            try
            {
                if (!ReleaseSessionsForTempDir(path))
                {
                    Util.AddLog("[TrimAdjust] 復元失敗後もセッションが残っているため、フォルダーを保持します: " + path, null);
                    return;
                }
                // 削除待ちを先に永続化し、削除途中の失敗後も次回起動で追跡できるようにする。
                restoredTempDirs.MarkDeletePending(sourcePath, path);
                DeleteOwnedRestoreDir(path, workRoot);
                if (Directory.Exists(path)) return;
                restoredTempDirs.CompleteDelete(sourcePath, path);
            }
            catch (Exception ex)
            {
                Util.AddLog("[TrimAdjust] 復元失敗後の一時フォルダー管理に失敗しました。フォルダーを保持します: " + path, ex);
            }
        }

        private static void DeleteOwnedRestoreDir(string path, string workRoot)
        {
            // 削除するパスは設定した作業ルート内の新規復元フォルダーに限定する。
            var rootPrefix = Path.TrimEndingDirectorySeparator(Path.GetFullPath(workRoot)) + Path.DirectorySeparatorChar;
            var comparison = OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
            if (!Path.IsPathFullyQualified(path) || !Path.GetFullPath(path).StartsWith(rootPrefix, comparison)
                || !Path.GetFileName(path).StartsWith("amt-trim-", StringComparison.Ordinal))
                throw new InvalidOperationException("復元フォルダーの削除対象が不正です");
            try { if (Directory.Exists(path)) Directory.Delete(path, recursive: true); }
            catch (Exception ex) { Util.AddLog("[TrimAdjust] 復元フォルダー削除に失敗しました: " + path, ex); }
        }

        private static string ExtractTrimLineFromLog(string logPath)
        {
            if (string.IsNullOrEmpty(logPath) || !File.Exists(logPath)) return null;
            try
            {
                var lines = File.ReadAllLines(logPath, Util.AmatsukazeDefaultEncoding);
                for (int index = 0; index < lines.Length; index++)
                {
                    if (!lines[index].Contains("[CM解析結果 - TrimAVS]")) continue;
                    for (int next = index + 1; next < lines.Length; next++)
                    {
                        var candidate = lines[next].Trim();
                        if (candidate.Length == 0) continue;
                        if (candidate.StartsWith("Trim(", StringComparison.Ordinal)) return candidate;
                        break;
                    }
                }
            }
            catch (Exception ex) { Util.AddLog("[TrimAdjust] ログからのTrim抽出に失敗しました", ex); }
            return null;
        }

        public void Dispose()
        {
            // 停止時だけ復元ジョブをキャンセルし、子プロセス停止と所有フォルダー処理の完了を待つ
            Task<(string restoredDir, string error)> restoreTask;
            lock (_restoreLock)
            {
                if (_disposed) return;
                _disposed = true;
                _restoreLifetimeCts.Cancel();
                restoreTask = _restoreJobTask;
            }
            try { restoreTask?.GetAwaiter().GetResult(); }
            catch (Exception ex) { Util.AddLog("[TrimAdjust] 停止時の復元ジョブ終了待ちに失敗しました", ex); }
            _restoreLifetimeCts.Dispose();
            // タイマーを停止してから全セッションを破棄する
            cleanupTimer?.Dispose();
            foreach (var pair in sessions)
            {
                if (sessions.TryRemove(pair.Key, out var session))
                {
                    session.Dispose();
                }
            }
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
