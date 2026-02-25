using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Text.RegularExpressions;
using Amatsukaze.Lib;
using Amatsukaze.Shared;

namespace Amatsukaze.Server.Rest
{
    public sealed class TrimAdjustSession : IDisposable
    {
        public string Id { get; }
        public int QueueItemId { get; }
        public string SrcPath { get; }
        public string TempDir { get; }
        public DateTime LastAccessUtc { get; private set; }
        public object SyncRoot { get; } = new object();

        private readonly AMTContext ctx;
        private readonly TrimAdjust trimadj;

        public int NumFrames => trimadj.NumFrames;
        public int FrameWidth => trimadj.Width;
        public int FrameHeight => trimadj.Height;

        public TrimAdjustSession(string id, int queueItemId, string srcPath, string tempDir, string datFilePath, int scaleMode)
        {
            Id = id;
            QueueItemId = queueItemId;
            SrcPath = srcPath;
            TempDir = tempDir;
            ctx = new AMTContext();
            trimadj = new TrimAdjust(ctx, datFilePath, scaleMode);
            Touch();
        }

        public void Touch()
        {
            LastAccessUtc = DateTime.UtcNow;
        }

        // フレーム画像をJPEGバイト列として取得
        public byte[] GetFrameJpeg(int frameNumber)
        {
            Touch();
            lock (SyncRoot)
            {
                return trimadj.GetFrameJpeg(frameNumber);
            }
        }

        // フレームメタ情報を取得
        public bool GetFrameInfo(int frameNumber, out long pts, out long duration, out int keyFrame, out int cmType)
        {
            return trimadj.GetFrameInfo(frameNumber, out pts, out duration, out keyFrame, out cmType);
        }

        // 波形画像をJPEGバイト列として取得
        public byte[] GetWaveformJpeg(int frameNumber)
        {
            Touch();
            lock (SyncRoot)
            {
                return trimadj.GetWaveformJpeg(frameNumber);
            }
        }

        public void Dispose()
        {
            trimadj?.Dispose();
            ctx?.Dispose();
        }
    }

    public class TrimAdjustService
    {
        private static readonly Regex TempDirRegex = new Regex(@"一時フォルダ:\s*(.+)", RegexOptions.Compiled);
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
                var framePts = new List<FramePtsInfo>();
                const double ptsToSeconds = 1.0 / 90000.0; // 90kHz PTS→秒
                for (int i = 0; i < session.NumFrames; i++)
                {
                    if (session.GetFrameInfo(i, out var pts, out var duration, out var keyFrame, out var cmType))
                    {
                        framePts.Add(new FramePtsInfo
                        {
                            Frame = i,
                            PtsSeconds = pts * ptsToSeconds,
                            KeyFrame = keyFrame,
                            CmType = cmType
                        });
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
                error = $"セッション作成に失敗しました: {ex.Message}";
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
                using var reader = new StreamReader(logPath);
                string line;
                while ((line = reader.ReadLine()) != null)
                {
                    var match = TempDirRegex.Match(line);
                    if (match.Success)
                    {
                        var dir = match.Groups[1].Value.Trim();
                        if (Directory.Exists(dir))
                        {
                            return dir;
                        }
                    }
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
