using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using Amatsukaze.Shared;

namespace Amatsukaze.Server.Rest
{
    internal sealed class TrimAdjustTempDirRegistry
    {
        internal sealed class Entry
        {
            public string SourcePath { get; set; }
            public string TempDir { get; set; }
            public bool IsRestored { get; set; }
            public bool DeletePending { get; set; }
        }

        private readonly object sync = new object();
        private readonly string workRoot;
        private readonly string indexPath;
        private readonly StringComparer pathComparer;
        private readonly List<Entry> entries = new List<Entry>();
        private bool loadFailed;
        private static readonly JsonSerializerOptions JsonOptions = new JsonSerializerOptions { WriteIndented = true };

        public TrimAdjustTempDirRegistry(string workRoot)
        {
            this.workRoot = Path.GetFullPath(workRoot);
            indexPath = Path.Combine(this.workRoot, "trim-adjust-tempdirs.json");
            pathComparer = OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal;
            Directory.CreateDirectory(this.workRoot);
            var rootInfo = new DirectoryInfo(this.workRoot);
            if ((rootInfo.Attributes & FileAttributes.ReparsePoint) != 0)
                throw new InvalidOperationException("TrimAdjust workRootが再解析ポイントです");
            Load();
        }

        public string GetRestoredPath(string sourcePath)
        {
            var identity = NormalizeSource(sourcePath);
            lock (sync)
            {
                return entries.LastOrDefault(entry => entry.IsRestored && !entry.DeletePending
                    && pathComparer.Equals(NormalizeSource(entry.SourcePath), identity))?.TempDir;
            }
        }

        public List<string> GetPathsForSource(string sourcePath)
        {
            var identity = NormalizeSource(sourcePath);
            lock (sync)
            {
                return entries.Where(entry => pathComparer.Equals(NormalizeSource(entry.SourcePath), identity))
                    .Select(entry => entry.TempDir).Distinct(pathComparer).ToList();
            }
        }

        public void RegisterRestored(string sourcePath, string tempDir)
        {
            var identity = NormalizeSource(sourcePath);
            if (!TryNormalizeOwnedPath(tempDir, out var normalized, out var reason) || !Path.GetFileName(normalized).StartsWith("amt-trim-", StringComparison.Ordinal))
                throw new InvalidOperationException("復元フォルダーを登録できません: " + (reason ?? normalized));
            lock (sync)
            {
                // 古い復元フォルダーも削除成功まで所有記録を保ち、後のキュー削除で回収する。
                entries.Add(new Entry { SourcePath = identity, TempDir = normalized, IsRestored = true, DeletePending = false });
                Save();
            }
        }

        public void MarkDeletePending(string sourcePath, string tempDir)
        {
            var identity = NormalizeSource(sourcePath);
            if (!TryNormalizeOwnedPath(tempDir, out var normalized, out var reason))
                throw new InvalidOperationException("削除待ちフォルダーを登録できません: " + (reason ?? tempDir));
            lock (sync)
            {
                var entry = entries.FirstOrDefault(item => pathComparer.Equals(NormalizeSource(item.SourcePath), identity)
                    && pathComparer.Equals(item.TempDir, normalized));
                if (entry == null)
                {
                    entry = new Entry { SourcePath = identity, TempDir = normalized, IsRestored = Path.GetFileName(normalized).StartsWith("amt-trim-", StringComparison.Ordinal) };
                    entries.Add(entry);
                }
                entry.DeletePending = true;
                Save();
            }
        }

        public void CompleteDelete(string sourcePath, string tempDir)
        {
            var identity = NormalizeSource(sourcePath);
            if (!TryNormalizeOwnedPath(tempDir, out var normalized, out _)) return;
            lock (sync)
            {
                entries.RemoveAll(item => pathComparer.Equals(NormalizeSource(item.SourcePath), identity)
                    && pathComparer.Equals(item.TempDir, normalized));
                Save();
            }
        }

        public bool TryDeletePaths(string sourcePath, IEnumerable<string> paths, IEnumerable<QueueItem> remainingOwners, Func<string, bool> releaseSessions, out bool handled, out string error)
        {
            handled = false;
            error = null;
            var identity = NormalizeSource(sourcePath);
            var owners = (remainingOwners ?? Enumerable.Empty<QueueItem>()).Where(item => item != null).ToArray();
            var targets = new List<string>();
            foreach (var path in paths ?? Enumerable.Empty<string>())
            {
                if (!TryNormalizeOwnedPath(path, out var normalized, out var reason))
                {
                    error = "削除対象を検証できません: " + reason;
                    return false;
                }
                if (!targets.Contains(normalized, pathComparer)) targets.Add(normalized);
            }
            if (targets.Count == 0) return true;
            handled = true;
            foreach (var target in targets)
            {
                var shared = owners.Any(item => pathComparer.Equals(NormalizeSource(item.SrcPath), identity)
                    || (!string.IsNullOrWhiteSpace(item.ResumeDir) && pathComparer.Equals(Path.GetFullPath(item.ResumeDir), target)));
                if (shared) continue;
                try
                {
                    MarkDeletePending(identity, target);
                    if (releaseSessions != null && !releaseSessions(target))
                    {
                        error = "使用中のTrimAdjustセッションを終了できません: " + target;
                        return false;
                    }
                    if (Directory.Exists(target)) Directory.Delete(target, recursive: true);
                    CompleteDelete(identity, target);
                }
                catch (Exception ex)
                {
                    error = "一時フォルダーの削除に失敗しました。削除待ち記録を保持します: " + target + ": " + ex.Message;
                    return false;
                }
            }
            return true;
        }
        public HashSet<string> PrepareStartupCleanup(IEnumerable<QueueItem> queueItems)
        {
            var queuedSources = new HashSet<string>((queueItems ?? Enumerable.Empty<QueueItem>())
                .Where(item => item != null && !string.IsNullOrWhiteSpace(item.SrcPath))
                .Select(item => NormalizeSource(item.SrcPath)), pathComparer);
            var preserve = new HashSet<string>(pathComparer);
            if (loadFailed)
            {
                foreach (var dir in Directory.GetDirectories(workRoot, "amt-trim-*")) preserve.Add(Path.GetFullPath(dir));
                Util.AddLog("[TrimAdjust] 管理情報を読み込めないため復元フォルダーを保護しました", null);
                return preserve;
            }
            lock (sync)
            {
                foreach (var entry in entries.ToArray())
                {
                    if (!TryNormalizeOwnedPath(entry.TempDir, out var normalized, out var reason))
                    {
                        Util.AddLog("[TrimAdjust] 所有フォルダー記録が不正なので保持します: " + reason, null);
                        preserve.Add(entry.TempDir);
                        continue;
                    }
                    if (entry.IsRestored && !entry.DeletePending && queuedSources.Contains(NormalizeSource(entry.SourcePath)))
                    {
                        preserve.Add(normalized);
                        continue;
                    }
                    entry.DeletePending = true;
                    Save();
                    try
                    {
                        if (Directory.Exists(normalized)) Directory.Delete(normalized, recursive: true);
                        entries.Remove(entry);
                        Save();
                    }
                    catch (Exception ex)
                    {
                        Util.AddLog("[TrimAdjust] 起動時の復元フォルダー削除に失敗しました。次回起動まで記録を保持します: " + normalized, ex);
                        preserve.Add(normalized);
                    }
                }
            }
            return preserve;
        }

        public bool TryNormalizeOwnedPath(string path, out string normalized, out string reason)
        {
            normalized = null;
            reason = null;
            if (string.IsNullOrWhiteSpace(path) || !Path.IsPathFullyQualified(path))
            {
                reason = "絶対パスではありません";
                return false;
            }
            try { normalized = Path.GetFullPath(path).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar); }
            catch (Exception ex) { reason = "パスを正規化できません: " + ex.Message; return false; }
            var comparison = OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
            var prefix = Path.TrimEndingDirectorySeparator(workRoot) + Path.DirectorySeparatorChar;
            if (!normalized.StartsWith(prefix, comparison)) { reason = "workRootの外です"; return false; }
            if (!string.Equals(Path.GetDirectoryName(normalized), Path.TrimEndingDirectorySeparator(workRoot), comparison)) { reason = "workRoot直下ではありません"; return false; }
            var name = Path.GetFileName(normalized);
            if (!System.Text.RegularExpressions.Regex.IsMatch(name ?? "", @"^amt(?:[0-9]+|-trim-[0-9a-f]{32})$", System.Text.RegularExpressions.RegexOptions.IgnoreCase | System.Text.RegularExpressions.RegexOptions.CultureInvariant))
            { reason = "一時フォルダー名が許可形式ではありません"; return false; }
            var info = new DirectoryInfo(normalized);
            if (info.Exists && (info.Attributes & FileAttributes.ReparsePoint) != 0) { reason = "再解析ポイントです"; return false; }
            return true;
        }

        private string NormalizeSource(string path)
        {
            if (string.IsNullOrWhiteSpace(path)) throw new ArgumentException("録画元パスが空です", nameof(path));
            return Path.GetFullPath(path).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        }

        private void Load()
        {
            if (!File.Exists(indexPath)) return;
            try
            {
                var loaded = JsonSerializer.Deserialize<List<Entry>>(File.ReadAllText(indexPath), JsonOptions);
                if (loaded == null) return;
                foreach (var entry in loaded)
                {
                    if (entry == null || string.IsNullOrWhiteSpace(entry.SourcePath)
                        || !TryNormalizeOwnedPath(entry.TempDir, out var path, out _)) continue;
                    entry.SourcePath = NormalizeSource(entry.SourcePath);
                    entry.TempDir = path;
                    entries.Add(entry);
                }
            }
            catch (Exception ex) { loadFailed = true; Util.AddLog("[TrimAdjust] 復元フォルダー管理情報を読み込めません。既存復元フォルダーは起動時に保護します", ex); }
        }

        private void Save()
        {
            var writing = indexPath + ".writing";
            try
            {
                var bytes = JsonSerializer.SerializeToUtf8Bytes(entries, JsonOptions);
                using (var stream = new FileStream(writing, FileMode.Create, FileAccess.Write, FileShare.None, 4096, FileOptions.WriteThrough))
                {
                    stream.Write(bytes, 0, bytes.Length);
                    stream.Flush(true);
                }
                File.Move(writing, indexPath, overwrite: true);
            }
            catch
            {
                try { if (File.Exists(writing)) File.Delete(writing); } catch { }
                throw;
            }
        }
    }
}
