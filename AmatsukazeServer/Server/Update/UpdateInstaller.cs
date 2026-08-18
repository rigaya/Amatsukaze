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
        string BackupPath, string Version, string Timestamp,
        InstallLayout Layout = InstallLayout.ExeFilesFlat, string InstalledRootPath = null);

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

    // 実行ファイルまたは対象ディレクトリのバックアップ、置換、動作確認を担当する。
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
        private static readonly Regex DirectoryResidueRegex = new Regex(
            @"^(?<name>.+)\.(?<kind>new|old)\.(?<ts>\d{8}-\d{6})$",
            RegexOptions.Compiled | RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));
        private readonly string appRoot;
        private readonly string executableRoot;
        private readonly Func<DateTime> clock;
        private readonly UpdateOSKind os;

        public UpdateInstaller(string appRoot, Func<DateTime> clock = null,
            UpdateOSKind? os = null)
        {
            this.appRoot = Path.GetFullPath(appRoot);
            executableRoot = Path.GetFullPath(Path.Combine(this.appRoot, "exe_files"));
            this.clock = clock;
            this.os = os ?? (OperatingSystem.IsWindows()
                ? UpdateOSKind.Windows : UpdateOSKind.Linux);
        }

        public Task<InstalledUpdate> InstallAsync(UpdateTargetDef target,
            PreparedUpdate prepared, string expectedVersion, UpdateLog log,
            CancellationToken cancellationToken)
        {
            var layout = target?.GetInstallLayout(os) ?? InstallLayout.AppRootPartial;
            ValidateInputs(target, prepared, expectedVersion, log, layout);
            cancellationToken.ThrowIfCancellationRequested();
            return layout == InstallLayout.ExeFilesSubDir
                ? InstallDirectoryAsync(target, prepared, expectedVersion, log, cancellationToken)
                : InstallFileAsync(target, prepared, expectedVersion, log, cancellationToken);
        }

        private async Task<InstalledUpdate> InstallFileAsync(UpdateTargetDef target,
            PreparedUpdate prepared, string expectedVersion, UpdateLog log,
            CancellationToken cancellationToken)
        {
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
                return new InstalledUpdate(target.Id, destination, backup, actualVersion, timestamp,
                    InstallLayout.ExeFilesFlat, destination);
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

        private async Task<InstalledUpdate> InstallDirectoryAsync(UpdateTargetDef target,
            PreparedUpdate prepared, string expectedVersion, UpdateLog log,
            CancellationToken cancellationToken)
        {
            if (Path.GetFileName(target.Id) != target.Id)
            {
                throw new UpdateInstallException("INVALID_INSTALL_PATH", "S10_INSTALL",
                    "更新対象IDを配置先ディレクトリ名として使用できません");
            }
            var timestamp = GetTimestamp(log);
            var destination = ValidateDirectChild(Path.Combine(executableRoot, target.Id));
            var pending = ValidateDirectChild(destination + ".new." + timestamp);
            var old = ValidateDirectChild(destination + ".old." + timestamp);
            var sourceDirectory = Path.GetFullPath(prepared.SourceDirectory);
            var executable = ValidateDirectoryExecutable(destination, prepared.DestName);
            var oldMoved = false;
            var installed = false;
            try
            {
                if (Directory.Exists(destination))
                {
                    // 前回の適用済み旧版は、次の差し替えを始める前にだけ回収する。
                    RemoveCompletedOldResidues(target.Id, log);
                }

                cancellationToken.ThrowIfCancellationRequested();
                Directory.Move(sourceDirectory, pending);
                cancellationToken.ThrowIfCancellationRequested();
                if (Directory.Exists(destination))
                {
                    Directory.Move(destination, old);
                    oldMoved = true;
                }
                Directory.Move(pending, destination);
                installed = true;

                log.Write(target.Id, "S10_INSTALL", "OK",
                    ("source", sourceDirectory), ("pending", pending),
                    ("old", oldMoved ? old : "(none)"),
                    ("backup", oldMoved ? old : "(none)"),
                    ("replaced", destination), ("operation", "directory_rename"));

                var probe = await UpdateExecutableProbe.RunAsync(executable,
                    target.VersionArgument, cancellationToken).ConfigureAwait(false);
                var match = target.VersionRegex?.Match(probe.Output ?? string.Empty);
                var actualVersion = match?.Success == true
                    ? (match.Groups["ver"].Success ? match.Groups["ver"].Value : match.Value)
                    : null;
                if (probe.LaunchFailed || !string.Equals(actualVersion, expectedVersion,
                    StringComparison.OrdinalIgnoreCase))
                {
                    log.Write(target.Id, "S11_POSTCHECK", "NG",
                        ("code", "VERIFY_FAILED"), ("path", executable),
                        ("exit", probe.ExitCode), ("out", probe.Output),
                        ("version", actualVersion ?? "Unknown"), ("expected", expectedVersion));
                    RollbackDirectory(target.Id, destination, old, timestamp, log);
                    oldMoved = false;
                    installed = false;
                    throw new UpdateInstallException("VERIFY_FAILED", "S11_POSTCHECK",
                        "設置後の実行ファイルを検証できなかったため旧版へ戻しました");
                }

                log.Write(target.Id, "S11_POSTCHECK", "OK", ("path", executable),
                    ("exit", probe.ExitCode), ("out", probe.Output),
                    ("version", actualVersion), ("expected", expectedVersion));
                return new InstalledUpdate(target.Id, executable, oldMoved ? old : null,
                    actualVersion, timestamp,
                    InstallLayout.ExeFilesSubDir, destination);
            }
            catch (UpdateInstallException)
            {
                throw;
            }
            catch (OperationCanceledException)
            {
                if (installed)
                {
                    RollbackDirectory(target.Id, destination, old, timestamp, log);
                }
                else if (oldMoved)
                {
                    RestoreOldDirectory(destination, old);
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
                try
                {
                    if (installed)
                    {
                        RollbackDirectory(target.Id, destination, old, timestamp, log);
                    }
                    else if (oldMoved)
                    {
                        RestoreOldDirectory(destination, old);
                    }
                }
                catch (UpdateInstallException)
                {
                    throw;
                }
                throw new UpdateInstallException(installed ? "VERIFY_FAILED" : "INSTALL_FAILED",
                    installed ? "S11_POSTCHECK" : "S10_INSTALL", ex.Message, ex);
            }
            finally
            {
                TryDeleteOwnedDirectory(pending);
            }
        }

        public Task RollbackInstalledAsync(InstalledUpdate installed, UpdateLog log)
        {
            if (installed == null || log == null)
            {
                throw new ArgumentNullException(installed == null ? nameof(installed) : nameof(log));
            }
            if (installed.Layout == InstallLayout.ExeFilesSubDir)
            {
                var destination = installed.InstalledRootPath ??
                    Path.GetDirectoryName(installed.DestinationPath);
                RollbackDirectory(installed.TargetId, destination,
                    installed.BackupPath, installed.Timestamp, log);
            }
            else
            {
                Rollback(installed.TargetId, installed.DestinationPath,
                    installed.BackupPath, installed.Timestamp, log);
            }
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

        private void RollbackDirectory(string targetId, string destination, string old,
            string timestamp, UpdateLog log)
        {
            var rollbackPending = ValidateDirectChild(destination + ".new." + timestamp);
            try
            {
                TryDeleteOwnedDirectory(rollbackPending);
                if (!string.IsNullOrEmpty(old))
                {
                    if (!Directory.Exists(old))
                    {
                        throw new DirectoryNotFoundException(
                            "旧版ディレクトリが消失しているため、安全に復元できません: " + old);
                    }
                    TryDeleteOwnedDirectory(destination);
                    Directory.Move(old, destination);
                    log.Write(targetId, "S13_ROLLBACK", "OK", ("restored", destination),
                        ("backup", old), ("method", "directory_old_rename"));
                }
                else
                {
                    TryDeleteOwnedDirectory(destination);
                    log.Write(targetId, "S13_ROLLBACK", "OK", ("removed", destination),
                        ("reason", "new_install"));
                }
            }
            catch (Exception ex)
            {
                log.Write(targetId, "S13_ROLLBACK", "NG", ("code", "ROLLBACK_FAILED"),
                    ("destination", destination),
                    ("destination_exists", Directory.Exists(destination)),
                    ("old", old), ("old_exists", Directory.Exists(old)),
                    ("pending", rollbackPending),
                    ("pending_exists", Directory.Exists(rollbackPending)),
                    ("error", ex.GetType().Name), ("message", ex.Message));
                throw new UpdateInstallException("ROLLBACK_FAILED", "S13_ROLLBACK",
                    "旧版ディレクトリへの復元に失敗しました。ログに記録した現物の状態を確認してください", ex);
            }
            finally
            {
                TryDeleteOwnedDirectory(rollbackPending);
            }
        }

        private static void RestoreOldDirectory(string destination, string old)
        {
            if (Directory.Exists(destination) || !Directory.Exists(old))
            {
                throw new UpdateInstallException("ROLLBACK_FAILED", "S13_ROLLBACK",
                    "旧版ディレクトリを安全に復元できる状態ではありません");
            }
            Directory.Move(old, destination);
        }

        private void RemoveCompletedOldResidues(string targetId, UpdateLog log)
        {
            var comparison = os == UpdateOSKind.Windows
                ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
            foreach (var residue in FindDirectoryResidues(targetId, "old", comparison))
            {
                DeleteOwnedDirectory(new DirectoryInfo(residue.Path));
                log.Write(targetId, "S13_ROLLBACK", "OK",
                    ("action", "remove_applied_old"), ("path", residue.Path),
                    ("result", "new_version_kept"));
            }
        }

        private (string Path, string Timestamp)[] FindDirectoryResidues(string targetId,
            string kind, StringComparison comparison)
        {
            return Directory.EnumerateDirectories(executableRoot, "*",
                    SearchOption.TopDirectoryOnly)
                .Select(path => (Path: path, Match: DirectoryResidueRegex.Match(
                    Path.GetFileName(path))))
                .Where(item => item.Match.Success && string.Equals(
                    item.Match.Groups["name"].Value, targetId, comparison) &&
                    item.Match.Groups["kind"].Value == kind)
                .Select(item => (item.Path,
                    Timestamp: item.Match.Groups["ts"].Value))
                .OrderByDescending(item => item.Timestamp, StringComparer.Ordinal)
                .ToArray();
        }

        private string ValidateDirectoryExecutable(string directory, string name)
        {
            if (Path.GetFileName(name) != name)
            {
                throw new UpdateInstallException("INVALID_INSTALL_PATH", "S10_INSTALL",
                    "実行ファイル名にディレクトリを含めることはできません");
            }
            var path = ValidateChild(directory, Path.Combine(directory, name));
            if (!string.Equals(Path.GetDirectoryName(path), directory, PathComparison))
            {
                throw new UpdateInstallException("INVALID_INSTALL_PATH", "S10_INSTALL",
                    "実行ファイルが対象ディレクトリ直下にありません");
            }
            return path;
        }

        private void ValidateInputs(UpdateTargetDef target, PreparedUpdate prepared,
            string expectedVersion, UpdateLog log, InstallLayout layout)
        {
            if (target == null || prepared == null || log == null ||
                !string.Equals(target.Id, prepared.TargetId, StringComparison.OrdinalIgnoreCase))
            {
                throw new UpdateInstallException("INVALID_INSTALL_INPUT", "S10_INSTALL",
                    "設置対象とステージング結果が一致しません");
            }
            if (layout == InstallLayout.AppRootPartial)
            {
                throw new UpdateInstallException("UNSUPPORTED_LAYOUT", "S10_INSTALL",
                    "アプリケーションルートの配置方式はこの段階では適用できません");
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
            if (layout == InstallLayout.ExeFilesSubDir)
            {
                if (string.IsNullOrWhiteSpace(prepared.SourceDirectory))
                {
                    throw new UpdateInstallException("INVALID_STAGING_PATH", "S10_INSTALL",
                        "ディレクトリ配置元が指定されていません");
                }
                var sourceDirectory = Path.GetFullPath(prepared.SourceDirectory);
                if (!IsChildPath(stagingRoot, sourceDirectory) ||
                    !IsChildPath(sourceDirectory, source) || !Directory.Exists(sourceDirectory))
                {
                    throw new UpdateInstallException("INVALID_STAGING_PATH", "S10_INSTALL",
                        "ディレクトリ配置元が更新一時ディレクトリの外を指しています");
                }
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

        public static int CleanupStartupResidues(string appRoot, UpdateOSKind? os = null)
        {
            try
            {
                if (!UpdateCatalog.TryInitialize(out _)) return 0;
                var effectiveOS = os ?? (OperatingSystem.IsWindows()
                    ? UpdateOSKind.Windows : UpdateOSKind.Linux);
                var installer = new UpdateInstaller(appRoot, os: effectiveOS);
                if (!Directory.Exists(installer.executableRoot)) return 0;
                var recovered = new List<(string Target, string Path, string Action, string Result)>();
                foreach (var path in Directory.EnumerateFiles(installer.executableRoot,
                    "*", SearchOption.TopDirectoryOnly))
                {
                    var match = NewResidueRegex.Match(Path.GetFileName(path));
                    if (!match.Success || !IsCurrentPlatformName(match.Groups["name"].Value,
                            effectiveOS) ||
                        !IsKnownPayloadName(match.Groups["name"].Value))
                    {
                        continue;
                    }
                    var target = FindPayloadTarget(match.Groups["name"].Value);
                    File.Delete(path);
                    recovered.Add((target, path, "remove_incomplete_new",
                        "old_version_unchanged"));
                }
                foreach (var target in UpdateCatalog.Targets.Where(item =>
                    item.GetInstallLayout(effectiveOS) == InstallLayout.ExeFilesSubDir &&
                    item.Payload?.Length > 0))
                {
                    installer.RecoverDirectoryResidues(target, effectiveOS, recovered);
                }
                installer.TryTrimBackups();
                if (recovered.Count > 0)
                {
                    using var log = new UpdateLog(installer.appRoot);
                    foreach (var item in recovered)
                    {
                        log.Write(item.Target, "S13_ROLLBACK", "OK",
                            ("action", item.Action), ("path", item.Path),
                            ("result", item.Result));
                    }
                }
                return recovered.Count;
            }
            catch (Exception ex)
            {
                UpdateLog.WriteFallbackError("S13_ROLLBACK", "STARTUP_RECOVERY_FAILED", ex);
                return 0;
            }
        }

        private void RecoverDirectoryResidues(UpdateTargetDef target, UpdateOSKind os,
            List<(string Target, string Path, string Action, string Result)> recovered)
        {
            var comparison = os == UpdateOSKind.Windows
                ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
            var destination = ValidateDirectChild(Path.Combine(executableRoot, target.Id));
            var newDirectories = FindDirectoryResidues(target.Id, "new", comparison);
            var oldDirectories = FindDirectoryResidues(target.Id, "old", comparison);

            if (Directory.Exists(destination))
            {
                foreach (var residue in newDirectories)
                {
                    DeleteOwnedDirectory(new DirectoryInfo(residue.Path));
                    recovered.Add((target.Id, residue.Path, "remove_incomplete_new",
                        "old_version_unchanged"));
                }
                foreach (var residue in oldDirectories)
                {
                    DeleteOwnedDirectory(new DirectoryInfo(residue.Path));
                    recovered.Add((target.Id, residue.Path, "remove_applied_old",
                        "new_version_kept"));
                }
                return;
            }

            if (newDirectories.Length > 0)
            {
                var selected = newDirectories[0];
                Directory.Move(selected.Path, destination);
                recovered.Add((target.Id, selected.Path, "complete_incomplete_new",
                    "new_version_activated"));
                foreach (var residue in newDirectories.Skip(1).Concat(oldDirectories))
                {
                    DeleteOwnedDirectory(new DirectoryInfo(residue.Path));
                    recovered.Add((target.Id, residue.Path, "remove_superseded_residue",
                        "complete_version_kept"));
                }
                return;
            }

            if (oldDirectories.Length > 0)
            {
                var selected = oldDirectories[0];
                Directory.Move(selected.Path, destination);
                recovered.Add((target.Id, selected.Path, "restore_incomplete_old",
                    "old_version_activated"));
                foreach (var residue in oldDirectories.Skip(1))
                {
                    DeleteOwnedDirectory(new DirectoryInfo(residue.Path));
                    recovered.Add((target.Id, residue.Path, "remove_superseded_residue",
                        "complete_version_kept"));
                }
            }
        }

        private static bool IsCurrentPlatformName(string name, UpdateOSKind os) =>
            os == UpdateOSKind.Windows
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

        private static void TryDeleteOwnedDirectory(string path)
        {
            try
            {
                if (!Directory.Exists(path)) return;
                DeleteOwnedDirectory(new DirectoryInfo(path));
            }
            catch
            {
                // 起動時に厳密な名前で再回収できるため、残骸の削除失敗は本来の結果を隠さない。
            }
        }
    }
}
