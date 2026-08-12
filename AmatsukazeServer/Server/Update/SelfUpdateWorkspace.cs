using System;
using System.IO;
using System.Text.RegularExpressions;

namespace Amatsukaze.Server.Update
{
    // サーバー終了後も保持する本体更新専用の作業領域を管理する。
    internal sealed class SelfUpdateWorkspace : IDisposable
    {
        private static readonly Regex TransactionIdRegex = new Regex("^[0-9a-f]{8}$",
            RegexOptions.Compiled | RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));
        private static readonly Regex TemporaryDirectoryRegex = new Regex(
            @"^\.(?:download|staging)\.[0-9a-f]{8}$",
            RegexOptions.Compiled | RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));
        private readonly string downloadDirectory;
        private readonly string candidateDirectory;
        private readonly string transactionId;
        private bool committed;
        private bool disposed;

        private SelfUpdateWorkspace(string rootDirectory, string stagingDirectory,
            string downloadDirectory, string candidateDirectory, string transactionId)
        {
            RootDirectory = rootDirectory;
            StagingDirectory = stagingDirectory;
            this.downloadDirectory = downloadDirectory;
            this.candidateDirectory = candidateDirectory;
            this.transactionId = transactionId;
        }

        public string RootDirectory { get; }
        public string StagingDirectory { get; }
        internal string DownloadDirectory => downloadDirectory;
        internal string CandidateDirectory => candidateDirectory;

        // 前回失敗した staging を、新しい準備処理が上書きしないようにする。
        public static void EnsureNoPendingUpdate(string appRoot)
        {
            var fullAppRoot = Path.GetFullPath(appRoot);
            var root = ValidateChild(fullAppRoot,
                Path.Combine(fullAppRoot, ".amatsukaze_update"));
            var staging = ValidateChild(root, Path.Combine(root, "staging"));
            if (Directory.Exists(staging) || File.Exists(staging))
            {
                throw new UpdatePreparationException("SELF_UPDATE_PENDING", "S01_PRECHECK",
                    "前回の本体更新が完了していません。保留中の更新を再実行してください");
            }
        }

        public static SelfUpdateWorkspace Create(string appRoot, string transactionId)
        {
            if (!TransactionIdRegex.IsMatch(transactionId ?? string.Empty))
            {
                throw new UpdatePreparationException("INVALID_TRANSACTION_ID", "S01_PRECHECK",
                    "更新トランザクションIDが不正です");
            }
            var fullAppRoot = Path.GetFullPath(appRoot);
            var root = ValidateChild(fullAppRoot,
                Path.Combine(fullAppRoot, ".amatsukaze_update"));
            EnsureNotReparsePoint(root);
            Directory.CreateDirectory(root);
            EnsureNotReparsePoint(root);
            CleanupStaleWorkDirectories(root, transactionId);
            var staging = ValidateChild(root, Path.Combine(root, "staging"));
            var download = ValidateChild(root, Path.Combine(root, ".download." + transactionId));
            var candidate = ValidateChild(root, Path.Combine(root, ".staging." + transactionId));
            DeleteOwned(download);
            DeleteOwned(candidate);
            Directory.CreateDirectory(download);
            Directory.CreateDirectory(candidate);
            return new SelfUpdateWorkspace(root, staging, download, candidate, transactionId);
        }

        // 検証済み候補だけを永続 staging に昇格する。
        public void Commit()
        {
            if (committed) return;
            var staging = ValidateChild(RootDirectory, StagingDirectory);
            var candidate = ValidateChild(RootDirectory, candidateDirectory);
            DeleteOwned(staging);
            Directory.Move(candidate, staging);
            committed = true;
        }

        internal static string ValidateChild(string root, string candidate)
        {
            var fullRoot = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar,
                Path.AltDirectorySeparatorChar) + Path.DirectorySeparatorChar;
            var fullCandidate = Path.GetFullPath(candidate);
            var comparison = OperatingSystem.IsWindows()
                ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
            if (!fullCandidate.StartsWith(fullRoot, comparison))
            {
                throw new UpdatePreparationException("INVALID_STAGING_PATH", "S01_PRECHECK",
                    "本体更新の作業領域外を指すパスは使用できません");
            }
            return fullCandidate;
        }

        private static void EnsureNotReparsePoint(string path)
        {
            if ((Directory.Exists(path) || File.Exists(path)) &&
                (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
            {
                throw new UpdatePreparationException("INVALID_STAGING_PATH", "S01_PRECHECK",
                    "本体更新の作業領域がリンクまたはリパースポイントです");
            }
        }

        private static void DeleteOwned(string path)
        {
            if (Directory.Exists(path))
            {
                var reparse = (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0;
                Directory.Delete(path, recursive: !reparse);
            }
            else if (File.Exists(path))
            {
                File.Delete(path);
            }
        }

        private static void CleanupStaleWorkDirectories(string root, string transactionId)
        {
            try
            {
                foreach (var entry in Directory.EnumerateFileSystemEntries(root, "*",
                    SearchOption.TopDirectoryOnly))
                {
                    var name = Path.GetFileName(entry);
                    if (!TemporaryDirectoryRegex.IsMatch(name) ||
                        string.Equals(name, ".download." + transactionId,
                            StringComparison.Ordinal) ||
                        string.Equals(name, ".staging." + transactionId,
                            StringComparison.Ordinal)) continue;
                    try
                    {
                        DeleteOwned(ValidateChild(root, entry));
                    }
                    catch (Exception ex)
                    {
                        SafeLog(transactionId, "本体更新の古い作業領域を削除できませんでした", ex);
                    }
                }
            }
            catch (Exception ex)
            {
                SafeLog(transactionId, "本体更新の古い作業領域を列挙できませんでした", ex);
            }
        }

        private static void SafeLog(string transactionId, string message, Exception exception)
        {
            try
            {
                Util.AddLog($"[Update][{transactionId}][-][S99_CLEANUP] " +
                    $"NG code=SELF_UPDATE_CLEANUP_FAILED msg={UpdateLog.FormatValue(message)} " +
                    $"type={exception.GetType().Name} " +
                    $"error={UpdateLog.FormatValue(exception.Message)}", exception);
            }
            catch
            {
                // 後始末ログの失敗をサーバー本体へ伝播させない。
            }
        }

        public void Dispose()
        {
            if (disposed) return;
            disposed = true;
            try
            {
                DeleteOwned(ValidateChild(RootDirectory, downloadDirectory));
                if (!committed)
                {
                    DeleteOwned(ValidateChild(RootDirectory, candidateDirectory));
                }
            }
            catch (Exception ex)
            {
                SafeLog(transactionId, "本体更新の作業領域を削除できませんでした", ex);
            }
        }
    }
}
