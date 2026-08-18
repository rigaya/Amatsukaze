using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;

namespace Amatsukaze.Server.Update
{
    internal sealed record SelfUpdateResult(string Status, string Version, string ErrorCode);
    internal sealed record SelfUpdateStartupState(bool HasPending, SelfUpdateResult LastResult);

    internal sealed class SelfUpdatePendingState
    {
        public bool Value { get; private set; }

        public SelfUpdatePendingState(bool initialValue) => Value = initialValue;

        public void Refresh(string appRoot) => Value = SelfUpdateRecovery.HasPending(appRoot);

        public void Clear() => Value = false;
    }

    // 本体 updater の異常終了残骸を復旧し、再起動後の結果をサーバーログへ取り込む。
    internal static class SelfUpdateRecovery
    {
        private const string WorkDirectoryName = ".amatsukaze_update";
        private const string StagingDirectoryName = "staging";
        private const string ResultFileName = "result.txt";
        private static readonly Regex WwwRootResidueRegex = new Regex(
            @"^wwwroot\.(?<kind>new|old)\.(?<txid>[0-9a-f]{8})$",
            RegexOptions.Compiled | RegexOptions.CultureInvariant | RegexOptions.IgnoreCase,
            TimeSpan.FromSeconds(1));
        private static readonly Regex TransactionIdRegex = new Regex("^[0-9a-f]{8}$",
            RegexOptions.Compiled | RegexOptions.CultureInvariant | RegexOptions.IgnoreCase,
            TimeSpan.FromSeconds(1));

        public static SelfUpdateStartupState RunStartupRecovery(string appRoot)
        {
            UpdateLog log = null;
            try
            {
                var result = ReadResultWithoutThrow(appRoot);
                var transactionId = FindStartupTransactionId(appRoot);
                UpdateLog GetLog() => log ??= new UpdateLog(appRoot,
                    transactionId: transactionId);
                RecoverWwwRootWithoutThrow(appRoot, GetLog);
                ImportResultWithoutThrow(appRoot, GetLog);
                var pending = DetectPendingWithoutThrow(appRoot, GetLog);
                return new SelfUpdateStartupState(pending, result);
            }
            finally
            {
                log?.Dispose();
            }
        }

        internal static bool HasPending(string appRoot)
        {
            try
            {
                return Directory.Exists(GetStagingPath(appRoot));
            }
            catch (Exception ex)
            {
                UpdateLog.WriteFallbackError("S20_PENDING",
                    "SELF_UPDATE_PENDING_CHECK_FAILED", ex);
                return false;
            }
        }

        internal static void DiscardPendingStaging(string appRoot)
        {
            var staging = GetStagingPath(appRoot);
            if (!Directory.Exists(staging)) return;
            var info = new DirectoryInfo(staging);
            var reparse = (info.Attributes & FileAttributes.ReparsePoint) != 0;
            info.Delete(recursive: !reparse);
        }

        internal static int RecoverWwwRootWithoutThrow(string appRoot)
        {
            UpdateLog log = null;
            try
            {
                var transactionId = FindResidueTransactionId(appRoot) ??
                    UpdateLog.UnknownTransactionId;
                return RecoverWwwRootWithoutThrow(appRoot, () => log ??=
                    new UpdateLog(appRoot, transactionId: transactionId));
            }
            finally
            {
                log?.Dispose();
            }
        }

        private static int RecoverWwwRootWithoutThrow(string appRoot, Func<UpdateLog> getLog)
        {
            try
            {
                var fullAppRoot = Path.GetFullPath(appRoot);
                var executableRoot = SelfUpdateWorkspace.ValidateChild(fullAppRoot,
                    Path.Combine(fullAppRoot, "exe_files"));
                if (!Directory.Exists(executableRoot)) return 0;
                var destination = SelfUpdateWorkspace.ValidateChild(executableRoot,
                    Path.Combine(executableRoot, "wwwroot"));
                var residues = new DirectoryInfo(executableRoot).EnumerateDirectories()
                    .Where(item => WwwRootResidueRegex.IsMatch(item.Name))
                    .OrderByDescending(item => item.LastWriteTimeUtc)
                    .ThenByDescending(item => item.Name, StringComparer.Ordinal)
                    .ToArray();
                if (residues.Length == 0) return 0;

                var recovered = new List<(string Action, string Path, string Result)>();
                if (Directory.Exists(destination))
                {
                    foreach (var residue in residues)
                    {
                        DeleteOwnedDirectory(residue);
                        recovered.Add(("remove_completed_residue", residue.FullName,
                            "current_wwwroot_kept"));
                    }
                }
                else
                {
                    var selected = residues.FirstOrDefault(item =>
                        string.Equals(WwwRootResidueRegex.Match(item.Name).Groups["kind"].Value,
                            "new", StringComparison.OrdinalIgnoreCase)) ?? residues[0];
                    Directory.Move(selected.FullName, destination);
                    recovered.Add((selected.Name.Contains(".new.", StringComparison.OrdinalIgnoreCase)
                            ? "complete_incomplete_new" : "restore_incomplete_old",
                        selected.FullName, selected.Name.Contains(".new.",
                            StringComparison.OrdinalIgnoreCase)
                            ? "new_wwwroot_activated" : "old_wwwroot_activated"));
                    foreach (var residue in residues.Where(item =>
                        !string.Equals(item.FullName, selected.FullName, StringComparison.Ordinal)))
                    {
                        DeleteOwnedDirectory(residue);
                        recovered.Add(("remove_superseded_residue", residue.FullName,
                            "complete_wwwroot_kept"));
                    }
                }

                var log = getLog();
                foreach (var item in recovered)
                {
                    log.Write("main", "S13_ROLLBACK", "OK", ("action", item.Action),
                        ("path", item.Path), ("result", item.Result));
                }
                return recovered.Count;
            }
            catch (Exception ex)
            {
                UpdateLog.WriteFallbackError("S13_ROLLBACK", "SELF_UPDATE_RECOVERY_FAILED", ex);
                return 0;
            }
        }

        internal static int ImportResultWithoutThrow(string appRoot)
        {
            UpdateLog log = null;
            try
            {
                var transactionId = FindResultTransactionId(appRoot) ??
                    UpdateLog.UnknownTransactionId;
                return ImportResultWithoutThrow(appRoot, () => log ??=
                    new UpdateLog(appRoot, transactionId: transactionId));
            }
            finally
            {
                log?.Dispose();
            }
        }

        private static int ImportResultWithoutThrow(string appRoot, Func<UpdateLog> getLog)
        {
            try
            {
                var workRoot = GetWorkRoot(appRoot);
                var resultPath = GetResultPath(appRoot);
                if (!File.Exists(resultPath)) return 0;

                var values = ParseResult(File.ReadAllLines(resultPath));
                var log = getLog();
                string importedLogPath = null;
                if (values.TryGetValue("log", out var candidateLogPath) &&
                    !string.IsNullOrWhiteSpace(candidateLogPath))
                {
                    try
                    {
                        importedLogPath = SelfUpdateWorkspace.ValidateChild(workRoot,
                            candidateLogPath);
                        if (File.Exists(importedLogPath))
                        {
                            foreach (var line in File.ReadLines(importedLogPath)) log.WriteRaw(line);
                        }
                        else
                        {
                            log.Write("main", "S20_IMPORT", "SKIP",
                                ("reason", "updater_log_not_found"), ("path", importedLogPath));
                        }
                    }
                    catch (Exception ex)
                    {
                        log.Write("main", "S20_IMPORT", "SKIP",
                            ("reason", "updater_log_unreadable"),
                            ("error", ex.GetType().Name), ("message", ex.Message));
                        importedLogPath = null;
                    }
                }
                else
                {
                    log.Write("main", "S20_IMPORT", "SKIP",
                        ("reason", "updater_log_not_recorded"));
                }

                var status = GetValue(values, "status", "unknown");
                var errorCode = GetValue(values, "error_code", string.Empty);
                log.Write("main", "S29_RESULT",
                    string.Equals(status, "success", StringComparison.OrdinalIgnoreCase)
                        ? "OK" : "NG",
                    ("status", status), ("version", GetValue(values, "version", "?")),
                    ("error_code", string.IsNullOrEmpty(errorCode) ? "(none)" : errorCode),
                    ("elapsed", CalculateElapsed(values)));

                DeleteFileWithoutThrow(resultPath);
                DeleteFileWithoutThrow(SelfUpdateWorkspace.ValidateChild(workRoot,
                    Path.Combine(workRoot, "updater_ready")));
                DeleteFileWithoutThrow(SelfUpdateWorkspace.ValidateChild(workRoot,
                    Path.Combine(workRoot, "updater.sh")));
                DeleteFileWithoutThrow(SelfUpdateWorkspace.ValidateChild(workRoot,
                    Path.Combine(workRoot, "updater.bat")));
                if (!string.IsNullOrEmpty(importedLogPath)) DeleteFileWithoutThrow(importedLogPath);
                return 1;
            }
            catch (Exception ex)
            {
                UpdateLog.WriteFallbackError("S20_IMPORT", "SELF_UPDATE_RESULT_IMPORT_FAILED", ex);
                return 0;
            }
        }

        internal static bool DetectPendingWithoutThrow(string appRoot)
        {
            UpdateLog log = null;
            try
            {
                return DetectPendingWithoutThrow(appRoot, () => log ??=
                    new UpdateLog(appRoot, transactionId: UpdateLog.UnknownTransactionId));
            }
            finally
            {
                log?.Dispose();
            }
        }

        private static bool DetectPendingWithoutThrow(string appRoot, Func<UpdateLog> getLog)
        {
            try
            {
                var staging = GetStagingPath(appRoot);
                var pending = Directory.Exists(staging);
                if (pending)
                {
                    getLog().Write("main", "S20_PENDING", "OK", ("pending", "yes"),
                        ("staging", staging));
                }
                return pending;
            }
            catch (Exception ex)
            {
                UpdateLog.WriteFallbackError("S20_PENDING", "SELF_UPDATE_PENDING_CHECK_FAILED", ex);
                return false;
            }
        }

        private static Dictionary<string, string> ParseResult(IEnumerable<string> lines)
        {
            var values = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            foreach (var line in lines)
            {
                var separator = line.IndexOf('=');
                if (separator <= 0) continue;
                values[line.Substring(0, separator)] = line.Substring(separator + 1);
            }
            return values;
        }

        private static string FindStartupTransactionId(string appRoot) =>
            FindResultTransactionId(appRoot) ?? FindResidueTransactionId(appRoot) ??
            UpdateLog.UnknownTransactionId;

        private static SelfUpdateResult ReadResultWithoutThrow(string appRoot)
        {
            try
            {
                var resultPath = GetResultPath(appRoot);
                if (!File.Exists(resultPath)) return null;
                var values = ParseResult(File.ReadAllLines(resultPath));
                return new SelfUpdateResult(GetValue(values, "status", "unknown"),
                    GetValue(values, "version", "?"),
                    GetValue(values, "error_code", string.Empty));
            }
            catch
            {
                return null;
            }
        }

        private static string FindResultTransactionId(string appRoot)
        {
            try
            {
                var resultPath = GetResultPath(appRoot);
                if (!File.Exists(resultPath)) return null;
                var values = ParseResult(File.ReadAllLines(resultPath));
                return values.TryGetValue("txid", out var transactionId) &&
                    TransactionIdRegex.IsMatch(transactionId)
                    ? transactionId.ToLowerInvariant() : null;
            }
            catch
            {
                return null;
            }
        }

        private static string FindResidueTransactionId(string appRoot)
        {
            try
            {
                var fullAppRoot = Path.GetFullPath(appRoot);
                var executableRoot = SelfUpdateWorkspace.ValidateChild(fullAppRoot,
                    Path.Combine(fullAppRoot, "exe_files"));
                if (!Directory.Exists(executableRoot)) return null;
                var residue = new DirectoryInfo(executableRoot).EnumerateDirectories()
                    .Select(item => (Directory: item,
                        Match: WwwRootResidueRegex.Match(item.Name)))
                    .Where(item => item.Match.Success)
                    .OrderByDescending(item => item.Directory.LastWriteTimeUtc)
                    .ThenByDescending(item => item.Directory.Name, StringComparer.Ordinal)
                    .FirstOrDefault();
                return residue.Match?.Groups["txid"].Value.ToLowerInvariant();
            }
            catch
            {
                return null;
            }
        }

        private static string CalculateElapsed(IReadOnlyDictionary<string, string> values)
        {
            if (values.TryGetValue("started", out var startedText) &&
                values.TryGetValue("finished", out var finishedText) &&
                DateTimeOffset.TryParse(startedText, CultureInfo.InvariantCulture,
                    DateTimeStyles.RoundtripKind, out var started) &&
                DateTimeOffset.TryParse(finishedText, CultureInfo.InvariantCulture,
                    DateTimeStyles.RoundtripKind, out var finished))
            {
                return Math.Max(0, (finished - started).TotalSeconds)
                    .ToString("0.###", CultureInfo.InvariantCulture) + "s";
            }
            return "?";
        }

        private static string GetWorkRoot(string appRoot)
        {
            var fullAppRoot = Path.GetFullPath(appRoot);
            return SelfUpdateWorkspace.ValidateChild(fullAppRoot,
                Path.Combine(fullAppRoot, WorkDirectoryName));
        }

        private static string GetStagingPath(string appRoot)
        {
            var workRoot = GetWorkRoot(appRoot);
            return SelfUpdateWorkspace.ValidateChild(workRoot,
                Path.Combine(workRoot, StagingDirectoryName));
        }

        private static string GetResultPath(string appRoot)
        {
            var workRoot = GetWorkRoot(appRoot);
            return SelfUpdateWorkspace.ValidateChild(workRoot,
                Path.Combine(workRoot, ResultFileName));
        }

        private static string GetValue(IReadOnlyDictionary<string, string> values,
            string key, string fallback) => values.TryGetValue(key, out var value)
            ? value : fallback;

        private static void DeleteOwnedDirectory(DirectoryInfo directory)
        {
            var reparse = (directory.Attributes & FileAttributes.ReparsePoint) != 0;
            directory.Delete(recursive: !reparse);
        }

        private static void DeleteFileWithoutThrow(string path)
        {
            try { if (File.Exists(path)) File.Delete(path); }
            catch (Exception ex)
            {
                UpdateLog.WriteFallbackError("S20_IMPORT", "SELF_UPDATE_ARTIFACT_DELETE_FAILED", ex);
            }
        }
    }
}
