using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;

namespace Amatsukaze.Server.Update
{
    internal sealed record InstalledUpdate(string TargetId, string DestinationPath,
        string BackupPath, string Version, string Timestamp);

    internal sealed class UpdateInstallException : Exception
    {
        public UpdateInstallException(string code, string stage, string message,
            Exception innerException = null) : base(message, innerException)
        {
            Code = code;
            Stage = stage;
        }

        public string Code { get; }
        public string Stage { get; }
    }

    // 単一実行ファイルのバックアップ、原子的置換、動作確認を担当する。
    internal sealed class UpdateInstaller
    {
        private const int BackupGenerations = 2;
        private const int ErrorSharingViolation = 32;
        private const int ErrorLockViolation = 33;
        private static readonly int TimestampLength = UpdateLog.TimestampFormat.Length;
        private static readonly Regex TimestampRegex = new Regex(@"^\d{8}-\d{6}$",
            RegexOptions.Compiled | RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));
        private static readonly Regex NewResidueRegex = new Regex(
            @"^(?<name>.+)\.new\.(?<ts>\d{8}-\d{6})$",
            RegexOptions.Compiled | RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));
        private readonly string appRoot;
        private readonly string executableRoot;
        private readonly Func<DateTime> clock;

        public UpdateInstaller(string appRoot, Func<DateTime> clock = null)
        {
            this.appRoot = Path.GetFullPath(appRoot);
            executableRoot = Path.GetFullPath(Path.Combine(this.appRoot, "exe_files"));
            this.clock = clock;
        }

        public async Task<InstalledUpdate> InstallAsync(UpdateTargetDef target,
            PreparedUpdate prepared, string expectedVersion, UpdateLog log,
            CancellationToken cancellationToken)
        {
            ValidateInputs(target, prepared, expectedVersion, log);
            cancellationToken.ThrowIfCancellationRequested();

            var timestamp = GetTimestamp(log);
            var destination = ValidateDirectChild(Path.Combine(executableRoot, prepared.DestName));
            var pending = ValidateDirectChild(destination + ".new." + timestamp);
            var backupDirectory = GetBackupDirectory(timestamp);
            var backup = File.Exists(destination)
                ? Path.Combine(backupDirectory, prepared.DestName) : null;
            var installed = false;
            try
            {
                if (backup != null)
                {
                    Directory.CreateDirectory(backupDirectory);
                    EnsureNotReparsePoint(backupDirectory);
                    File.Copy(destination, backup, overwrite: false);
                }

                cancellationToken.ThrowIfCancellationRequested();
                File.Move(prepared.FilePath, pending, overwrite: false);
                cancellationToken.ThrowIfCancellationRequested();
                try
                {
                    // 配置先が消える時間を作らないため、既存に触れる操作はこの rename だけにする。
                    File.Move(pending, destination, overwrite: true);
                    installed = true;
                }
                catch (IOException ex) when (IsSharingViolation(ex))
                {
                    log.Write(target.Id, "S10_INSTALL", "NG", ("code", "FILE_IN_USE"),
                        ("path", destination), ("error", ex.GetType().Name),
                        ("message", ex.Message));
                    throw new UpdateInstallException("FILE_IN_USE", "S10_INSTALL",
                        "実行中のプロセスが更新対象ファイルを使用しているため置換できません", ex);
                }

                log.Write(target.Id, "S10_INSTALL", "OK",
                    ("source", prepared.FilePath), ("pending", pending),
                    ("backup", backup ?? "(none)"), ("replaced", destination),
                    ("operation", "atomic_rename"));

                var probe = await UpdateExecutableProbe.RunAsync(destination,
                    target.VersionArgument, cancellationToken).ConfigureAwait(false);
                var match = target.VersionRegex?.Match(probe.Output ?? string.Empty);
                var actualVersion = match?.Success == true
                    ? (match.Groups["ver"].Success ? match.Groups["ver"].Value : match.Value)
                    : null;
                if (probe.LaunchFailed || !string.Equals(actualVersion, expectedVersion,
                    StringComparison.OrdinalIgnoreCase))
                {
                    log.Write(target.Id, "S11_POSTCHECK", "NG",
                        ("code", "VERIFY_FAILED"), ("path", destination),
                        ("exit", probe.ExitCode), ("out", probe.Output),
                        ("version", actualVersion ?? "Unknown"), ("expected", expectedVersion));
                    Rollback(target.Id, destination, backup, timestamp, log);
                    throw new UpdateInstallException("VERIFY_FAILED", "S11_POSTCHECK",
                        "設置後の実行ファイルを検証できなかったため旧版へ戻しました");
                }

                log.Write(target.Id, "S11_POSTCHECK", "OK", ("path", destination),
                    ("exit", probe.ExitCode), ("out", probe.Output),
                    ("version", actualVersion), ("expected", expectedVersion));
                return new InstalledUpdate(target.Id, destination, backup, actualVersion, timestamp);
            }
            catch (UpdateInstallException)
            {
                throw;
            }
            catch (OperationCanceledException)
            {
                if (installed)
                {
                    Rollback(target.Id, destination, backup, timestamp, log);
                }
                else
                {
                    log.Write(target.Id, "S13_ROLLBACK", "SKIP",
                        ("reason", "no_files_modified"));
                }
                throw;
            }
            catch (Exception ex)
            {
                log.Write(target.Id, installed ? "S11_POSTCHECK" : "S10_INSTALL", "NG",
                    ("code", installed ? "VERIFY_FAILED" : "INSTALL_FAILED"),
                    ("path", destination), ("error", ex.GetType().Name),
                    ("message", ex.Message));
                if (installed)
                {
                    Rollback(target.Id, destination, backup, timestamp, log);
                }
                throw new UpdateInstallException(installed ? "VERIFY_FAILED" : "INSTALL_FAILED",
                    installed ? "S11_POSTCHECK" : "S10_INSTALL", ex.Message, ex);
            }
            finally
            {
                TryDeleteFile(pending);
                TryTrimBackups();
            }
        }

        public Task RollbackInstalledAsync(InstalledUpdate installed, UpdateLog log)
        {
            if (installed == null || log == null)
            {
                throw new ArgumentNullException(installed == null ? nameof(installed) : nameof(log));
            }
            Rollback(installed.TargetId, installed.DestinationPath,
                installed.BackupPath, installed.Timestamp, log);
            return Task.CompletedTask;
        }

        private void Rollback(string targetId, string destination, string backup,
            string timestamp, UpdateLog log)
        {
            var rollbackPending = ValidateDirectChild(destination + ".new." + timestamp);
            try
            {
                TryDeleteFile(rollbackPending);
                if (!string.IsNullOrEmpty(backup))
                {
                    if (!File.Exists(backup))
                    {
                        throw new FileNotFoundException(
                            "旧版バックアップが消失しているため、安全に復元できません", backup);
                    }
                    File.Copy(backup, rollbackPending, overwrite: false);
                    File.Move(rollbackPending, destination, overwrite: true);
                    log.Write(targetId, "S13_ROLLBACK", "OK", ("restored", destination),
                        ("backup", backup), ("method", "atomic_rename"));
                }
                else
                {
                    File.Delete(destination);
                    log.Write(targetId, "S13_ROLLBACK", "OK", ("removed", destination),
                        ("reason", "new_install"));
                }
            }
            catch (Exception ex)
            {
                log.Write(targetId, "S13_ROLLBACK", "NG", ("code", "ROLLBACK_FAILED"),
                    ("destination", destination), ("destination_exists", File.Exists(destination)),
                    ("backup", backup ?? "(none)"),
                    ("backup_exists", !string.IsNullOrEmpty(backup) && File.Exists(backup)),
                    ("pending", rollbackPending), ("pending_exists", File.Exists(rollbackPending)),
                    ("error", ex.GetType().Name), ("message", ex.Message));
                throw new UpdateInstallException("ROLLBACK_FAILED", "S13_ROLLBACK",
                    "旧版への復元に失敗しました。ログに記録した現物の状態を確認してください", ex);
            }
            finally
            {
                TryDeleteFile(rollbackPending);
            }
        }

        private void ValidateInputs(UpdateTargetDef target, PreparedUpdate prepared,
            string expectedVersion, UpdateLog log)
        {
            if (target == null || prepared == null || log == null ||
                !string.Equals(target.Id, prepared.TargetId, StringComparison.OrdinalIgnoreCase))
            {
                throw new UpdateInstallException("INVALID_INSTALL_INPUT", "S10_INSTALL",
                    "設置対象とステージング結果が一致しません");
            }
            if (target.GetInstallLayout(UpdateRuntimeEnvironment.Detect().OS) !=
                InstallLayout.ExeFilesFlat)
            {
                throw new UpdateInstallException("UNSUPPORTED_LAYOUT", "S10_INSTALL",
                    "単一ファイル以外の配置方式はこの段階では適用できません");
            }
            if (!target.TryCompileRegexes(out var catalogError))
            {
                throw new UpdateInstallException("INVALID_CATALOG", "S10_INSTALL",
                    "更新対象の定義が不正です: " + catalogError);
            }
            if (string.IsNullOrWhiteSpace(expectedVersion) ||
                !string.Equals(prepared.Version, expectedVersion,
                    StringComparison.OrdinalIgnoreCase) ||
                string.IsNullOrWhiteSpace(prepared.DestName) ||
                Path.GetFileName(prepared.DestName) != prepared.DestName)
            {
                throw new UpdateInstallException("INVALID_INSTALL_INPUT", "S10_INSTALL",
                    "検証済みバージョンまたは配置先名が不正です");
            }
            var stagingRoot = Path.GetFullPath(Path.Combine(executableRoot, ".update_tmp"));
            var source = Path.GetFullPath(prepared.FilePath);
            if (!IsChildPath(stagingRoot, source) || !File.Exists(source))
            {
                throw new UpdateInstallException("INVALID_STAGING_PATH", "S10_INSTALL",
                    "配置元が更新一時ディレクトリの外を指しています");
            }
        }

        private string GetBackupDirectory(string timestamp)
        {
            var backupRoot = Path.GetFullPath(Path.Combine(executableRoot, ".update_backup"));
            EnsureNotReparsePoint(backupRoot);
            var directory = ValidateChild(backupRoot, Path.Combine(backupRoot, timestamp));
            EnsureNotReparsePoint(directory);
            return directory;
        }

        private string GetTimestamp(UpdateLog log)
        {
            if (clock != null)
            {
                return clock().ToString(UpdateLog.TimestampFormat, CultureInfo.InvariantCulture);
            }
            var fileName = Path.GetFileName(log.FilePath);
            if (!string.IsNullOrEmpty(fileName) && fileName.Length >= TimestampLength)
            {
                var value = fileName.Substring(0, TimestampLength);
                if (TimestampRegex.IsMatch(value)) return value;
            }
            return DateTime.Now.ToString(UpdateLog.TimestampFormat, CultureInfo.InvariantCulture);
        }

        private string ValidateDirectChild(string candidate)
        {
            var validated = ValidateChild(executableRoot, candidate);
            if (!string.Equals(Path.GetDirectoryName(validated), executableRoot,
                PathComparison))
            {
                throw new UpdateInstallException("INVALID_INSTALL_PATH", "S10_INSTALL",
                    "配置先が exe_files 直下ではありません");
            }
            return validated;
        }

        private static string ValidateChild(string root, string candidate)
        {
            var fullRoot = Path.GetFullPath(root);
            var fullCandidate = Path.GetFullPath(candidate);
            if (!IsChildPath(fullRoot, fullCandidate))
            {
                throw new UpdateInstallException("INVALID_INSTALL_PATH", "S10_INSTALL",
                    "更新対象パスが管理ディレクトリの外を指しています");
            }
            return fullCandidate;
        }

        private static bool IsChildPath(string root, string candidate)
        {
            var prefix = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar,
                Path.AltDirectorySeparatorChar) + Path.DirectorySeparatorChar;
            return Path.GetFullPath(candidate).StartsWith(prefix, PathComparison);
        }

        private static StringComparison PathComparison => OperatingSystem.IsWindows()
            ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;

        private static bool IsSharingViolation(IOException exception)
        {
            var error = exception.HResult & 0xffff;
            return OperatingSystem.IsWindows() &&
                (error == ErrorSharingViolation || error == ErrorLockViolation);
        }

        private void TrimBackups()
        {
            var backupRoot = Path.Combine(executableRoot, ".update_backup");
            if (!Directory.Exists(backupRoot)) return;
            EnsureNotReparsePoint(backupRoot);
            var generations = new DirectoryInfo(backupRoot).EnumerateDirectories()
                .Where(directory => TimestampRegex.IsMatch(directory.Name) &&
                    (directory.Attributes & FileAttributes.ReparsePoint) == 0)
                .OrderByDescending(directory => directory.Name, StringComparer.Ordinal)
                .Skip(BackupGenerations);
            foreach (var directory in generations)
            {
                DeleteOwnedDirectory(directory);
            }
        }

        private static void EnsureNotReparsePoint(string path)
        {
            if ((Directory.Exists(path) || File.Exists(path)) &&
                (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
            {
                throw new UpdateInstallException("INVALID_BACKUP_PATH", "S10_INSTALL",
                    "更新バックアップ先がリンクまたはリパースポイントです");
            }
        }

        private void TryTrimBackups()
        {
            try
            {
                TrimBackups();
            }
            catch (Exception ex)
            {
                UpdateLog.WriteFallbackError("S10_INSTALL", "BACKUP_RETENTION_FAILED", ex);
            }
        }

        public static int CleanupStartupResidues(string appRoot)
        {
            try
            {
                if (!UpdateCatalog.TryInitialize(out _)) return 0;
                var installer = new UpdateInstaller(appRoot);
                if (!Directory.Exists(installer.executableRoot)) return 0;
                var removed = new List<(string Target, string Path)>();
                foreach (var path in Directory.EnumerateFiles(installer.executableRoot,
                    "*", SearchOption.TopDirectoryOnly))
                {
                    var match = NewResidueRegex.Match(Path.GetFileName(path));
                    if (!match.Success || !IsCurrentPlatformName(match.Groups["name"].Value) ||
                        !IsKnownPayloadName(match.Groups["name"].Value))
                    {
                        continue;
                    }
                    var target = FindPayloadTarget(match.Groups["name"].Value);
                    File.Delete(path);
                    removed.Add((target, path));
                }
                installer.TryTrimBackups();
                if (removed.Count > 0)
                {
                    using var log = new UpdateLog(installer.appRoot);
                    foreach (var item in removed)
                    {
                        log.Write(item.Target, "S13_ROLLBACK", "OK",
                            ("action", "remove_incomplete_new"), ("path", item.Path),
                            ("result", "old_version_unchanged"));
                    }
                }
                return removed.Count;
            }
            catch (Exception ex)
            {
                UpdateLog.WriteFallbackError("S13_ROLLBACK", "STARTUP_RECOVERY_FAILED", ex);
                return 0;
            }
        }

        private static bool IsCurrentPlatformName(string name) => OperatingSystem.IsWindows()
            ? name.EndsWith(".exe", StringComparison.OrdinalIgnoreCase)
            : !name.EndsWith(".exe", StringComparison.OrdinalIgnoreCase);

        private static bool IsKnownPayloadName(string name) =>
            UpdateCatalog.Targets.Any(target => target.Payload?.Any(entry => entry.IsMatch(name)) == true);

        private static string FindPayloadTarget(string name) => UpdateCatalog.Targets
            .First(target => target.Payload?.Any(entry => entry.IsMatch(name)) == true).Id;

        private static void DeleteOwnedDirectory(DirectoryInfo directory)
        {
            var reparse = (directory.Attributes & FileAttributes.ReparsePoint) != 0;
            directory.Delete(recursive: !reparse);
        }

        private static void TryDeleteFile(string path)
        {
            try
            {
                if (File.Exists(path)) File.Delete(path);
            }
            catch
            {
                // 起動時に厳密な名前で再回収できるため、残骸の削除失敗は本来の結果を隠さない。
            }
        }
    }
}
