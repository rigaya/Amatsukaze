using System.Security.Cryptography;
using Amatsukaze.Server.Update;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class UpdateInstallerTests
{
    [Fact]
    public async Task 原子的置換後に新版が動作し残骸を残さない()
    {
        if (OperatingSystem.IsWindows()) return;
        using var fixture = new InstallerFixture();
        var oldPath = fixture.WriteInstalled("tool", "1.0.0");
        var oldHash = Hash(oldPath);
        var prepared = fixture.CreatePrepared("tool", "2.0.0", exitCode: 7);

        InstalledUpdate installed;
        using (var log = new UpdateLog(fixture.Root))
        {
            installed = await fixture.Installer.InstallAsync(fixture.Target, prepared,
                "2.0.0", log, CancellationToken.None);
        }

        Assert.Equal("2.0.0", installed.Version);
        Assert.Contains("version 2.0.0", await RunAsync(oldPath));
        Assert.Equal(oldHash, Hash(installed.BackupPath));
        Assert.Empty(Directory.EnumerateFiles(fixture.ExeFiles, "*.new.*"));
    }

    [Fact]
    public async Task 設置後検証の失敗時は旧版をハッシュ一致で復元する()
    {
        if (OperatingSystem.IsWindows()) return;
        using var fixture = new InstallerFixture();
        var oldPath = fixture.WriteInstalled("tool", "1.0.0");
        var oldHash = Hash(oldPath);
        // PreparedUpdate の検証済み版と現物を故意に食い違わせて S11 を失敗させる。
        var prepared = fixture.CreatePrepared("tool", "2.0.0", actualVersion: "9.9.9");

        using var log = new UpdateLog(fixture.Root);
        var exception = await Assert.ThrowsAsync<UpdateInstallException>(() =>
            fixture.Installer.InstallAsync(fixture.Target, prepared, "2.0.0", log,
                CancellationToken.None));

        Assert.Equal("VERIFY_FAILED", exception.Code);
        Assert.Equal(oldHash, Hash(oldPath));
        Assert.Contains("version 1.0.0", await RunAsync(oldPath));
        Assert.Empty(Directory.EnumerateFiles(fixture.ExeFiles, "*.new.*"));
    }

    [Fact]
    public async Task バックアップ消失時は新版を消さず復元失敗とする()
    {
        if (OperatingSystem.IsWindows()) return;
        using var fixture = new InstallerFixture();
        var destination = fixture.WriteInstalled("tool", "1.0.0");
        InstalledUpdate installed;
        using (var installLog = new UpdateLog(fixture.Root))
        {
            installed = await fixture.Installer.InstallAsync(fixture.Target,
                fixture.CreatePrepared("tool", "2.0.0"), "2.0.0", installLog,
                CancellationToken.None);
        }
        var installedHash = Hash(destination);
        File.Delete(installed.BackupPath);

        using var rollbackLog = new UpdateLog(fixture.Root);
        var exception = await Assert.ThrowsAsync<UpdateInstallException>(() =>
            fixture.Installer.RollbackInstalledAsync(installed, rollbackLog));

        Assert.Equal("ROLLBACK_FAILED", exception.Code);
        Assert.True(File.Exists(destination));
        Assert.Equal(installedHash, Hash(destination));
        Assert.Contains("version 2.0.0", await RunAsync(destination));
    }

    [Fact]
    public void 起動時は厳密一致する既知対象のNew残骸だけ削除する()
    {
        if (OperatingSystem.IsWindows()) return;
        using var fixture = new InstallerFixture();
        var oldPath = fixture.WriteInstalled("x264", "1.0.0");
        var oldHash = Hash(oldPath);
        var residue = Path.Combine(fixture.ExeFiles, "x264.new.20260811-123456");
        var misleading = new[]
        {
            "x264.new.txt", "x264.new.2026", "x264.new",
            "unknown.new.20260811-123456",
        };
        File.WriteAllText(residue, "更新残骸");
        foreach (var name in misleading) File.WriteAllText(Path.Combine(fixture.ExeFiles, name), name);

        var removed = UpdateInstaller.CleanupStartupResidues(fixture.Root);

        Assert.Equal(1, removed);
        Assert.False(File.Exists(residue));
        Assert.All(misleading, name => Assert.True(File.Exists(Path.Combine(fixture.ExeFiles, name))));
        Assert.Equal(oldHash, Hash(oldPath));
    }

    [Fact]
    public void T18_起動時掃除後も対象実行ファイルは旧版として残る()
    {
        if (OperatingSystem.IsWindows()) return;
        using var fixture = new InstallerFixture();
        var installed = fixture.WriteInstalled("x264", "1.0.0");
        var installedHash = Hash(installed);
        var residue = Path.Combine(fixture.ExeFiles, "x264.new.20260811-123456");
        File.WriteAllText(residue, "未完了の新版");
        var stale = Path.Combine(fixture.ExeFiles, ".update_tmp", "deadbeef",
            "extract", "x264");
        Directory.CreateDirectory(stale);
        File.WriteAllText(Path.Combine(stale, "x264"), "一時ファイル");

        UpdateTransaction.CleanupStale(fixture.Root);
        UpdateInstaller.CleanupStartupResidues(fixture.Root);

        Assert.True(File.Exists(installed));
        Assert.True(new FileInfo(installed).Length > 0);
        Assert.Equal(installedHash, Hash(installed));
        Assert.False(File.Exists(residue));
        Assert.Empty(Directory.EnumerateFileSystemEntries(
            Path.Combine(fixture.ExeFiles, ".update_tmp")));
    }

    [NonRootLinuxFact]
    public void T14_配置先が書き込み不可ならS01で事前拒否する()
    {
        if (OperatingSystem.IsWindows()) return;
        var root = Path.Combine(Path.GetTempPath(),
            "amatsukaze-readonly-test-" + Guid.NewGuid());
        var executableRoot = Directory.CreateDirectory(Path.Combine(root, "exe_files")).FullName;
        var readOnlyMode = UnixFileMode.UserRead | UnixFileMode.UserExecute |
            UnixFileMode.GroupRead | UnixFileMode.GroupExecute |
            UnixFileMode.OtherRead | UnixFileMode.OtherExecute;
        try
        {
            File.SetUnixFileMode(executableRoot, readOnlyMode);
            using var log = new UpdateLog(root);

            var exception = Assert.Throws<UpdatePreparationException>(() =>
                UpdateManager.EnsureWritableUpdateDirectories(root, log));
            log.Dispose();
            var text = File.ReadAllText(log.FilePath);

            Assert.Equal("WRITE_ACCESS_DENIED", exception.Code);
            Assert.Equal("S01_PRECHECK", exception.Stage);
            Assert.Contains("area=install", text);
            Assert.Contains("path=", text);
        }
        finally
        {
            if (Directory.Exists(executableRoot))
            {
                File.SetUnixFileMode(executableRoot, UnixFileMode.UserRead |
                    UnixFileMode.UserWrite | UnixFileMode.UserExecute);
            }
            if (Directory.Exists(root)) Directory.Delete(root, true);
        }
    }

    [Fact]
    public async Task バックアップは新しい二世代だけ保持する()
    {
        if (OperatingSystem.IsWindows()) return;
        using var fixture = new InstallerFixture();
        fixture.WriteInstalled("tool", "1.0.0");
        foreach (var item in new[]
        {
            ("20260811-010101", "2.0.0"),
            ("20260811-020202", "3.0.0"),
            ("20260811-030303", "4.0.0"),
        })
        {
            var instant = DateTime.ParseExact(item.Item1, "yyyyMMdd-HHmmss",
                System.Globalization.CultureInfo.InvariantCulture,
                System.Globalization.DateTimeStyles.None);
            var installer = new UpdateInstaller(fixture.Root, () => instant);
            using var log = new UpdateLog(fixture.Root);
            await installer.InstallAsync(fixture.Target,
                fixture.CreatePrepared("tool", item.Item2), item.Item2, log,
                CancellationToken.None);
        }

        var generations = Directory.EnumerateDirectories(Path.Combine(fixture.ExeFiles,
            ".update_backup")).Select(Path.GetFileName).OrderBy(name => name).ToArray();
        Assert.Equal(new[] { "20260811-020202", "20260811-030303" }, generations);
    }

    [Fact]
    public async Task バックアップルートがリンクなら境界外へ書かず拒否する()
    {
        if (OperatingSystem.IsWindows()) return;
        using var fixture = new InstallerFixture();
        fixture.WriteInstalled("tool", "1.0.0");
        var external = Directory.CreateDirectory(Path.Combine(fixture.Root, "external")).FullName;
        var sentinel = Path.Combine(external, "sentinel");
        File.WriteAllText(sentinel, "変更禁止");
        Directory.CreateSymbolicLink(Path.Combine(fixture.ExeFiles, ".update_backup"), external);
        using var log = new UpdateLog(fixture.Root);

        var exception = await Assert.ThrowsAsync<UpdateInstallException>(() =>
            fixture.Installer.InstallAsync(fixture.Target,
                fixture.CreatePrepared("tool", "2.0.0"), "2.0.0", log,
                CancellationToken.None));

        Assert.Equal("INVALID_BACKUP_PATH", exception.Code);
        Assert.Equal("変更禁止", File.ReadAllText(sentinel));
        Assert.Single(Directory.EnumerateFileSystemEntries(external));
    }

    [Fact]
    public async Task MaintenanceLeaseは実行中を拒否して必ず停止を解除する()
    {
        var pauses = new List<bool>();
        var exception = await Assert.ThrowsAsync<UpdateInstallException>(() =>
            UpdateMaintenanceLease.AcquireAsync(() => true, pause =>
            {
                pauses.Add(pause);
                return Task.CompletedTask;
            }, CancellationToken.None));

        Assert.Equal("ENCODING_ACTIVE", exception.Code);
        Assert.Equal(new[] { true, false }, pauses);
    }

    [Fact]
    public async Task MaintenanceLease解放でユーザー停止を上書きしない()
    {
        var maintenance = false;
        using (await UpdateMaintenanceLease.AcquireAsync(() => false, pause =>
        {
            maintenance = pause;
            return Task.CompletedTask;
        }, CancellationToken.None))
        {
            Assert.True(maintenance);
        }
        Assert.False(maintenance);

        var pool = new Amatsukaze.Server.WorkerPool();
        pool.SetPause(true, scheduled: false);
        pool.SetMaintenancePause(true);
        Assert.Throws<InvalidOperationException>(() => pool.ForceStart(null!));
        pool.SetMaintenancePause(false);
        Assert.True(pool.UserPaused);
        Assert.True(pool.IsPaused);
    }

    [Fact]
    public async Task MaintenanceLeaseの復旧通知失敗は元の取得例外を上書きしない()
    {
        var original = new InvalidOperationException("取得失敗");
        var exception = await Assert.ThrowsAsync<InvalidOperationException>(() =>
            UpdateMaintenanceLease.AcquireAsync(() => false, pause =>
            {
                if (pause) throw original;
                throw new ApplicationException("復旧通知失敗");
            }, CancellationToken.None));

        Assert.Same(original, exception);
    }

    [Fact]
    public async Task MaintenanceLeaseの解放通知失敗はDisposeから送出しない()
    {
        var lease = await UpdateMaintenanceLease.AcquireAsync(() => false, pause =>
            pause ? Task.CompletedTask : Task.FromException(
                new ApplicationException("解放通知失敗")), CancellationToken.None);

        var exception = Record.Exception(lease.Dispose);

        Assert.Null(exception);
    }

    [Fact]
    public async Task 単一Writerは多重適用を区別して拒否し解放後に再取得できる()
    {
        var semaphore = new SemaphoreSlim(1, 1);
        using (await UpdateWriterLease.AcquireAsync(semaphore, CancellationToken.None))
        {
            var exception = await Assert.ThrowsAsync<UpdateInstallException>(() =>
                UpdateWriterLease.AcquireAsync(semaphore, CancellationToken.None));
            Assert.Equal("UPDATE_BUSY", exception.Code);
        }
        using var reacquired = await UpdateWriterLease.AcquireAsync(semaphore,
            CancellationToken.None);
    }

    [Fact]
    public async Task 単一Writerの解放失敗はDisposeから送出しない()
    {
        var semaphore = new SemaphoreSlim(1, 1);
        var lease = await UpdateWriterLease.AcquireAsync(semaphore, CancellationToken.None);
        semaphore.Dispose();

        var exception = Record.Exception(lease.Dispose);

        Assert.Null(exception);
    }

    [Fact]
    public void 更新ジョブの中止はリンクしたTokenへ伝播する()
    {
        using var managerCancellation = new CancellationTokenSource();
        var job = new UpdateManager.UpdateJobRecord("job", managerCancellation.Token,
            isApply: true);

        Assert.True(job.Cancel());
        Assert.True(job.Token.IsCancellationRequested);
        Assert.False(job.Cancel());
        job.Dispose();
    }

    [Fact]
    public void 更新ジョブは対象別結果をスナップショットで返す()
    {
        var job = new UpdateManager.UpdateJobRecord("job", CancellationToken.None,
            isApply: true);
        job.AddTargetResult("x264", true, null, "更新しました");
        job.AddTargetResult("x265", false, "VERIFY_FAILED", "検証失敗");

        var view = job.ToView();
        view.TargetResults[0].Message = "書き換え";

        Assert.True(job.IsApply);
        Assert.Equal(2, job.ToView().TargetResults.Count);
        Assert.Equal("更新しました", job.ToView().TargetResults[0].Message);
        job.Dispose();
    }

    [Fact]
    public void Windows設定パスはExeFiles直下の同系列だけ更新する()
    {
        Assert.True(UpdateCatalog.TryInitialize(out var error), error);
        using var fixture = new InstallerFixture();
        var target = Assert.Single(UpdateCatalog.Targets, item => item.Id == "x264");
        var oldPath = Path.Combine(fixture.ExeFiles, "x264_3223_x64.exe");
        var newPath = Path.Combine(fixture.ExeFiles, "x264_3333_x64.exe");

        Assert.True(UpdateManager.ShouldUpdateWindowsSettingPath(target, oldPath,
            newPath, fixture.Root));
        Assert.False(UpdateManager.ShouldUpdateWindowsSettingPath(target,
            Path.Combine(fixture.Root, "custom", "x264_3223_x64.exe"), newPath,
            fixture.Root));
        Assert.False(UpdateManager.ShouldUpdateWindowsSettingPath(target,
            Path.Combine(fixture.ExeFiles, "unrelated.exe"), newPath, fixture.Root));
        Assert.False(UpdateManager.ShouldUpdateWindowsSettingPath(target, string.Empty,
            newPath, fixture.Root));
    }

    [Fact]
    public void P2cで適用できる対象は三対象だけ()
    {
        Assert.True(UpdateCatalog.TryInitialize(out var error), error);
        var environment = UpdateRuntimeEnvironment.Detect();

        var applicable = UpdateCatalog.Targets.Where(target =>
            UpdateManager.GetCannotApplyReason(target, environment.OS) == null)
            .Select(target => target.Id).OrderBy(id => id).ToArray();

        Assert.Equal(new[] { "SVT-AV1", "x264", "x265" }, applicable);

        var qsvenc = UpdateCatalog.Targets.Single(target => target.Id == "QSVEnc");
        Assert.Equal(InstallLayout.ExeFilesFlat,
            qsvenc.GetInstallLayout(UpdateOSKind.Linux));
        Assert.Equal("payload_not_defined_yet",
            UpdateManager.GetCannotApplyReason(qsvenc, UpdateOSKind.Linux));
        Assert.Equal("layout_not_supported_yet",
            UpdateManager.GetCannotApplyReason(qsvenc, UpdateOSKind.Windows));
    }

    private static string Hash(string path) => Convert.ToHexString(
        SHA256.HashData(File.ReadAllBytes(path)));

    private static async Task<string> RunAsync(string path)
    {
        var result = await UpdateExecutableProbe.RunAsync(path, "--version",
            CancellationToken.None);
        return result.Output;
    }

    private sealed class InstallerFixture : IDisposable
    {
        private readonly List<UpdateTransaction> transactions = new();

        public InstallerFixture()
        {
            Root = Path.Combine(Path.GetTempPath(), "amatsukaze-installer-test-" + Guid.NewGuid());
            ExeFiles = Path.Combine(Root, "exe_files");
            Directory.CreateDirectory(ExeFiles);
            Installer = new UpdateInstaller(Root,
                () => new DateTime(2026, 8, 11, 12, 34, 56, DateTimeKind.Utc));
            Target = new UpdateTargetDef
            {
                Id = "test",
                DisplayName = "test",
                Repository = "test/test",
                AssetRules = Array.Empty<AssetRule>(),
                WindowsLayout = InstallLayout.ExeFilesFlat,
                LinuxLayout = InstallLayout.ExeFilesFlat,
                VersionArgument = "--version",
                VersionPattern = @"version (?<ver>\d+\.\d+\.\d+)",
                Payload = [new PayloadEntry { Pattern = "^tool$" }],
            };
            Assert.True(Target.TryCompileRegexes(out var error), error);
        }

        public string Root { get; }
        public string ExeFiles { get; }
        public UpdateInstaller Installer { get; }
        public UpdateTargetDef Target { get; }

        public string WriteInstalled(string name, string version)
        {
            var path = Path.Combine(ExeFiles, name);
            WriteScript(path, version, 0);
            return path;
        }

        public PreparedUpdate CreatePrepared(string name, string version, int exitCode = 0,
            string? actualVersion = null)
        {
            var transaction = UpdateTransaction.Create(Root,
                (transactions.Count + 1).ToString("x8"));
            transactions.Add(transaction);
            var path = Path.Combine(transaction.GetTargetExtractDirectory("test"), name);
            WriteScript(path, actualVersion ?? version, exitCode);
            return new PreparedUpdate("test", path, name, version);
        }

        private static void WriteScript(string path, string version, int exitCode)
        {
            File.WriteAllText(path, $"#!/bin/sh\necho version {version}\nexit {exitCode}\n");
            if (!OperatingSystem.IsWindows())
            {
                File.SetUnixFileMode(path, File.GetUnixFileMode(path) |
                    UnixFileMode.UserExecute);
            }
        }

        public void Dispose()
        {
            foreach (var transaction in transactions) transaction.Dispose();
            Directory.Delete(Root, recursive: true);
        }
    }
}

internal sealed class NonRootLinuxFactAttribute : FactAttribute
{
    public NonRootLinuxFactAttribute()
    {
        if (OperatingSystem.IsWindows())
        {
            Skip = "Linux のアクセス権テストです";
        }
        else if (string.Equals(Environment.UserName, "root", StringComparison.Ordinal))
        {
            Skip = "root は配置先のアクセス権を無視するため検証できません";
        }
    }
}
