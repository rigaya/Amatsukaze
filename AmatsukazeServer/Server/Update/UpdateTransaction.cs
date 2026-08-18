using System;
using System.IO;
using System.Text.RegularExpressions;
using System.Threading;

namespace Amatsukaze.Server.Update
{
    // 1回の更新操作で使う一時ファイル群の寿命を所有する。
    internal sealed class UpdateTransaction : IDisposable
    {
        private static readonly Regex TransactionIdRegex = new Regex("^[0-9a-f]{8}$",
            RegexOptions.Compiled | RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));
        private readonly string temporaryRoot;
        private bool disposed;

        private UpdateTransaction(string temporaryRoot, string rootDir, string txId)
        {
            this.temporaryRoot = temporaryRoot;
            TxId = txId;
            RootDir = rootDir;
            DownloadDir = Path.Combine(rootDir, "download");
            ExtractDir = Path.Combine(rootDir, "extract");
            Directory.CreateDirectory(DownloadDir);
            Directory.CreateDirectory(ExtractDir);
        }

        public string TxId { get; }
        public string RootDir { get; }
        public string DownloadDir { get; }
        public string ExtractDir { get; }

        public static UpdateTransaction Create(string appRoot, string txId)
        {
            if (!TransactionIdRegex.IsMatch(txId ?? string.Empty))
            {
                throw new UpdatePreparationException("INVALID_TRANSACTION_ID", "S01_PRECHECK",
                    "更新トランザクションIDが不正です");
            }
            var temporaryRoot = GetTemporaryRoot(appRoot);
            EnsureNotReparsePoint(temporaryRoot);
            Directory.CreateDirectory(temporaryRoot);
            EnsureNotReparsePoint(temporaryRoot);
            var rootDir = ValidateChildPath(temporaryRoot, Path.Combine(temporaryRoot, txId));
            DeleteWithRetry(rootDir, txId);
            if (Directory.Exists(rootDir) || File.Exists(rootDir))
            {
                throw new UpdatePreparationException("TEMP_DELETE_FAILED", "S01_PRECHECK",
                    "同じIDの更新一時ディレクトリを初期化できませんでした");
            }
            return new UpdateTransaction(temporaryRoot, rootDir, txId);
        }

        public string GetTargetDownloadDirectory(string targetId) =>
            CreateTargetDirectory(DownloadDir, targetId);

        public string GetTargetExtractDirectory(string targetId) =>
            CreateTargetDirectory(ExtractDir, targetId);

        private static string CreateTargetDirectory(string parent, string targetId)
        {
            if (string.IsNullOrWhiteSpace(targetId) || targetId.IndexOfAny(
                new[] { Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar }) >= 0)
            {
                throw new UpdatePreparationException("INVALID_TARGET_ID", "S01_PRECHECK",
                    "更新対象IDが不正です");
            }
            var directory = ValidateChildPath(parent, Path.Combine(parent, targetId));
            Directory.CreateDirectory(directory);
            return directory;
        }

        public static void CleanupStale(string appRoot)
        {
            string temporaryRoot;
            try
            {
                temporaryRoot = GetTemporaryRoot(appRoot);
                if (!Directory.Exists(temporaryRoot)) return;
                EnsureNotReparsePoint(temporaryRoot);
                foreach (var entry in Directory.EnumerateFileSystemEntries(temporaryRoot))
                {
                    var validated = ValidateChildPath(temporaryRoot, entry);
                    DeleteWithRetry(validated, "startup");
                }
            }
            catch (Exception ex)
            {
                SafeLog("startup", "一時ディレクトリの残骸を削除できませんでした", ex);
            }
        }

        private static string GetTemporaryRoot(string appRoot)
        {
            var executableRoot = Path.GetFullPath(Path.Combine(appRoot, "exe_files"));
            return ValidateChildPath(executableRoot,
                Path.Combine(executableRoot, ".update_tmp"));
        }

        private static string ValidateChildPath(string root, string candidate)
        {
            var fullRoot = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar,
                Path.AltDirectorySeparatorChar) + Path.DirectorySeparatorChar;
            var fullCandidate = Path.GetFullPath(candidate);
            var comparison = OperatingSystem.IsWindows()
                ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
            if (!fullCandidate.StartsWith(fullRoot, comparison))
            {
                throw new UpdatePreparationException("INVALID_STAGING_PATH", "S01_PRECHECK",
                    "削除対象が更新一時ディレクトリの外を指しています");
            }
            return fullCandidate;
        }

        private static void DeleteWithRetry(string path, string txId)
        {
            for (var attempt = 1; attempt <= 3; attempt++)
            {
                try
                {
                    if (Directory.Exists(path))
                    {
                        var reparse = (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0;
                        Directory.Delete(path, recursive: !reparse);
                    }
                    else if (File.Exists(path)) File.Delete(path);
                    return;
                }
                catch (Exception) when (attempt < 3)
                {
                    Thread.Sleep(100);
                }
                catch (Exception ex)
                {
                    SafeLog(txId, "一時ディレクトリを削除できませんでした", ex);
                }
            }
        }

        private static void EnsureNotReparsePoint(string path)
        {
            if ((Directory.Exists(path) || File.Exists(path)) &&
                (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
            {
                throw new UpdatePreparationException("INVALID_STAGING_PATH", "S01_PRECHECK",
                    "更新一時ディレクトリがリンクまたはリパースポイントです");
            }
        }

        private static void SafeLog(string txId, string message, Exception exception)
        {
            try
            {
                Util.AddLog($"[Update][{txId}][-][S99_CLEANUP] NG code=TEMP_DELETE_FAILED msg={UpdateLog.FormatValue(message)} path={UpdateLog.FormatValue(exception.Message)}", exception);
            }
            catch
            {
                // 後始末ログの失敗をサーバーへ伝播させない。
            }
        }

        public void Dispose()
        {
            if (disposed) return;
            disposed = true;
            try
            {
                var validated = ValidateChildPath(temporaryRoot, RootDir);
                DeleteWithRetry(validated, TxId);
            }
            catch (Exception ex)
            {
                SafeLog(TxId, "一時ディレクトリの検証または削除に失敗しました", ex);
            }
        }
    }
}
