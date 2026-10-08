using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text.Json;
using Amatsukaze.Shared;

namespace Amatsukaze.Server.Rest
{
    // ネイティブデコーダーに依存しない正解ファイル管理。全セッションでファイル操作を直列化する。
    public sealed class CmSegAnnotationStore
    {
        private static readonly object FileSync = new object();
        private static readonly JsonSerializerOptions JsonOptions = new JsonSerializerOptions
        {
            PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
            PropertyNameCaseInsensitive = true,
            WriteIndented = true
        };
        private readonly string filePath;
        private CmSegAnnotation annotation;
        private string expectedFileHash;

        public static bool IsEnabled => Environment.GetEnvironmentVariable("AMT_CMSEG_ANNOTATION") == "1";

        private CmSegAnnotationStore(string filePath, CmSegAnnotation annotation, string expectedFileHash)
        {
            this.filePath = filePath;
            this.annotation = annotation;
            this.expectedFileHash = expectedFileHash;
        }

        // 正解入力の初期化失敗を、映像を開く既存処理から切り離す。
        public static CmSegAnnotationStore OpenForSession(string srcPath, string tempDir, int numFrames,
            IReadOnlyDictionary<long, int> durationCounts, List<JlsSegment> segments, out string error)
        {
            error = null;
            if (!IsEnabled) return null;
            try
            {
                if (!File.Exists(srcPath)) throw new FileNotFoundException("入力TSが見つかりません", srcPath);
                var path = srcPath + ".cmseg.json";
                if (Directory.Exists(path)) throw new IOException("正解ファイルの保存先がディレクトリになっています");
                if (OperatingSystem.IsWindows() && File.Exists(path) && (File.GetAttributes(path) & FileAttributes.ReadOnly) != 0)
                    throw new UnauthorizedAccessException("正解ファイルが読み取り専用です");
                // 本保存と同じ場所に作成できるか確認し、検査ファイルは閉じると削除する。
                var probe = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
                using (new FileStream(probe, FileMode.CreateNew, FileAccess.Write, FileShare.None, 1, FileOptions.DeleteOnClose)) { }
                return Open(srcPath, tempDir, numFrames, ResolveFps(durationCounts), segments);
            }
            catch (Exception ex)
            {
                error = $"この録画の正解入力は利用できません。カット調整は続けられます: {ex.Message}";
                return null;
            }
        }

        // PTSの欠落やドロップによる長い間隔に引きずられないよう、最頻のフレーム間隔を使う。
        // 59.94fps等の整数丸めを放送で使われる有理数へ戻し、それ以外は90kHzとの比を保存する。
        public static string ResolveFps(IReadOnlyDictionary<long, int> durationCounts)
        {
            var ticks = durationCounts.Where(p => p.Key > 0).OrderByDescending(p => p.Value).ThenBy(p => p.Key)
                .Select(p => p.Key).FirstOrDefault();
            if (ticks == 0) throw new InvalidDataException("フレーム間隔からフレームレートを取得できません");
            var rates = new (int Num, int Den)[]
            {
                (24000, 1001), (30000, 1001), (60000, 1001), (24, 1), (25, 1), (30, 1), (50, 1), (60, 1)
            };
            var nearest = rates.OrderBy(r => Math.Abs(90000.0 * r.Den / r.Num - ticks)).First();
            if (Math.Abs(90000.0 * nearest.Den / nearest.Num - ticks) <= 0.5)
                return FormattableString.Invariant($"{nearest.Num}/{nearest.Den}");
            long divisor = 90000, remainder = ticks;
            while (remainder != 0) { var next = divisor % remainder; divisor = remainder; remainder = next; }
            return FormattableString.Invariant($"{90000 / divisor}/{ticks / divisor}");
        }

        public static CmSegAnnotationStore Open(string srcPath, string tempDir, int numFrames, string fps, List<JlsSegment> segments)
        {
            if (!IsEnabled) return null;
            lock (FileSync)
            {
                var source = new FileInfo(srcPath);
                var jlsPath = Path.Combine(tempDir, "jls0.txt");
                var initial = new CmSegAnnotation
                {
                    Source = new CmSegSource { Path = srcPath, Size = source.Length, Mtime = source.LastWriteTimeUtc },
                    TempDir = tempDir, NumFrames = numFrames, Fps = fps,
                    JlsSha1 = File.Exists(jlsPath) ? Convert.ToHexString(SHA1.HashData(File.ReadAllBytes(jlsPath))).ToLowerInvariant() : "",
                    UpdatedAt = DateTime.UtcNow,
                    Segments = segments.Select((s, i) => new CmSegAnnotationSegment
                    {
                        Idx = i + 1, Start = s.Start, End = s.End, JlsLabel = s.Label, JlsKeep = s.JlsKeep,
                        Label = CmSegLabels.InitialLabel(s.Label, s.JlsKeep)
                    }).ToList()
                };
                var path = srcPath + ".cmseg.json";
                string fileHash = null;
                if (File.Exists(path))
                {
                    CmSegAnnotation saved = null;
                    var savedBytes = File.ReadAllBytes(path);
                    try { saved = JsonSerializer.Deserialize<CmSegAnnotation>(savedBytes, JsonOptions); }
                    catch (JsonException) { }
                    if (Matches(saved, initial))
                    {
                        fileHash = Convert.ToHexString(SHA256.HashData(savedBytes));
                        initial.Reviewed = saved.Reviewed;
                        initial.UpdatedAt = saved.UpdatedAt;
                        for (var i = 0; i < initial.Segments.Count; i++)
                        {
                            initial.Segments[i].Label = saved.Segments[i].Label;
                            initial.Segments[i].Edited = initial.Segments[i].Label != CmSegLabels.InitialLabel(initial.Segments[i].JlsLabel, initial.Segments[i].JlsKeep);
                        }
                    }
                    else
                    {
                        var backup = srcPath + ".cmseg." + DateTime.UtcNow.ToString("yyyyMMddHHmmssfffffff", CultureInfo.InvariantCulture) + ".bak.json";
                        File.Move(path, backup);
                        initial.Warning = $"正解ファイルとjls区間が一致しないか形式が不正なため、旧ファイルを退避して初期化しました: {backup}";
                    }
                }
                return new CmSegAnnotationStore(path, initial, fileHash);
            }
        }

        private static bool Matches(CmSegAnnotation saved, CmSegAnnotation initial)
        {
            if (saved == null || saved.Version != 1 || saved.VideoIndex != 0 || saved.NumFrames != initial.NumFrames ||
                saved.Segments == null || saved.Segments.Count != initial.Segments.Count) return false;
            for (var i = 0; i < initial.Segments.Count; i++)
            {
                var a = saved.Segments[i];
                var b = initial.Segments[i];
                if (a == null || a.Idx != b.Idx || a.Start != b.Start || a.End != b.End || a.Label == null || !CmSegLabels.IsValid(a.Label)) return false;
            }
            return true;
        }

        public CmSegAnnotation Get()
        {
            if (!IsEnabled) return null;
            lock (FileSync) return Clone(annotation);
        }

        // APIからは分類だけ受け取り、入力ファイル・境界・jls判定を書き換えさせない。
        public void Save(CmSegSaveRequest request)
        {
            if (!IsEnabled) throw new InvalidOperationException("正解入力は無効です");
            lock (FileSync)
            {
                // 別画面や再解析後のセッションが保存した正解を、古いスナップショットで戻さない。
                var currentHash = File.Exists(filePath) ? Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(filePath))) : null;
                if (currentHash != expectedFileHash)
                    throw new CmSegAnnotationConflictException("別の画面で正解ファイルが変更されました。カット調整画面を開き直してください。");
                var jlsPath = Path.Combine(annotation.TempDir, "jls0.txt");
                var currentJlsHash = File.Exists(jlsPath) ? Convert.ToHexString(SHA1.HashData(File.ReadAllBytes(jlsPath))).ToLowerInvariant() : "";
                if (currentJlsHash != annotation.JlsSha1)
                    throw new CmSegAnnotationConflictException("jls解析結果が変更されました。カット調整画面を開き直してください。");
                if (request?.Segments == null || request.Segments.Count != annotation.Segments.Count)
                    throw new ArgumentException("正解入力の区間数が一致しません");
                var updates = new Dictionary<int, string>();
                foreach (var segment in request.Segments)
                {
                    if (segment == null || segment.Label == null || !CmSegLabels.IsValid(segment.Label) ||
                        !updates.TryAdd(segment.Idx, segment.Label)) throw new ArgumentException("正解入力の区間番号または分類が不正です");
                }
                var next = Clone(annotation);
                foreach (var segment in next.Segments)
                {
                    if (!updates.TryGetValue(segment.Idx, out var label)) throw new ArgumentException("正解入力の区間番号が一致しません");
                    segment.Label = label;
                    segment.Edited = label != CmSegLabels.InitialLabel(segment.JlsLabel, segment.JlsKeep);
                }
                next.Reviewed = request.Reviewed;
                next.UpdatedAt = DateTime.UtcNow;
                next.Warning = null;
                var temporary = filePath + "." + Guid.NewGuid().ToString("N") + ".tmp";
                try
                {
                    // 同じディレクトリで書き終えてから置き換え、途中のJSONを読ませない。
                    var bytes = JsonSerializer.SerializeToUtf8Bytes(next, JsonOptions);
                    File.WriteAllBytes(temporary, bytes);
                    File.Move(temporary, filePath, true);
                    annotation = next;
                    expectedFileHash = Convert.ToHexString(SHA256.HashData(bytes));
                }
                finally { if (File.Exists(temporary)) File.Delete(temporary); }
            }
        }

        private static CmSegAnnotation Clone(CmSegAnnotation value) =>
            JsonSerializer.Deserialize<CmSegAnnotation>(JsonSerializer.Serialize(value, JsonOptions), JsonOptions);
    }

    public sealed class CmSegAnnotationConflictException : InvalidOperationException
    {
        public CmSegAnnotationConflictException(string message) : base(message) { }
    }
}
