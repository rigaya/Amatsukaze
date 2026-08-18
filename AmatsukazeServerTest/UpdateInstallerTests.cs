using System.Security.Cryptography;
using Amatsukaze.Server;
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
    public async Task サブディレクトリは中身全部を差し替えロールバックできる()
    {
        if (OperatingSystem.IsWindows()) return;
        using var fixture = new InstallerFixture();
        var target = fixture.CreateSubDirectoryTarget("QSVEnc", "QSVEncC64.exe");
        var destination = Path.Combine(fixture.ExeFiles, target.Id);
        Directory.CreateDirectory(destination);
        fixture.WriteScript(Path.Combine(destination, "QSVEncC64.exe"), "1.0.0");
        File.WriteAllText(Path.Combine(destination, "old.dll"), "old");
        var prepared = fixture.CreateDirectoryPrepared(target, "QSVEncC64.exe", "2.0.0",
            ("new.dll", "new"));
        var installer = fixture.CreateInstaller(UpdateOSKind.Windows);

        InstalledUpdate installed;
        using (var log = new UpdateLog(fixture.Root))
        {
            installed = await installer.InstallAsync(target, prepared, "2.0.0", log,
                CancellationToken.None);
        }

        Assert.Equal(InstallLayout.ExeFilesSubDir, installed.Layout);
        Assert.Contains("version 2.0.0", await RunAsync(installed.DestinationPath));
        Assert.True(File.Exists(Path.Combine(destination, "new.dll")));
        Assert.False(File.Exists(Path.Combine(destination, "old.dll")));
        Assert.Empty(Directory.EnumerateDirectories(fixture.ExeFiles, "QSVEnc.new.*"));
        var old = Assert.Single(Directory.EnumerateDirectories(
            fixture.ExeFiles, "QSVEnc.old.*"));
        Assert.Equal(old, installed.BackupPath);
        Assert.False(Directory.Exists(Path.Combine(fixture.ExeFiles, ".update_backup")));

        using var rollbackLog = new UpdateLog(fixture.Root);
        await installer.RollbackInstalledAsync(installed, rollbackLog);

        Assert.Contains("version 1.0.0", await RunAsync(installed.DestinationPath));
        Assert.True(File.Exists(Path.Combine(destination, "old.dll")));
        Assert.False(File.Exists(Path.Combine(destination, "new.dll")));
        Assert.False(Directory.Exists(old));
    }

    [Fact]
    public async Task サブディレクトリはバックアップ領域へ複製しない()
    {
        if (OperatingSystem.IsWindows()) return;
        using var fixture = new InstallerFixture();
        var target = fixture.CreateSubDirectoryTarget("QSVEnc", "QSVEncC64.exe");
        var destination = Path.Combine(fixture.ExeFiles, target.Id);
        Directory.CreateDirectory(destination);
        fixture.WriteScript(Path.Combine(destination, "QSVEncC64.exe"), "1.0.0");
        var installer = fixture.CreateInstaller(UpdateOSKind.Windows);
        using var log = new UpdateLog(fixture.Root);

        await installer.InstallAsync(target,
            fixture.CreateDirectoryPrepared(target, "QSVEncC64.exe", "2.0.0"),
            "2.0.0", log, CancellationToken.None);

        Assert.False(Directory.Exists(Path.Combine(fixture.ExeFiles, ".update_backup")));
        Assert.Single(Directory.EnumerateDirectories(fixture.ExeFiles, "QSVEnc.old.*"));
    }

    [Fact]
    public async Task サブディレクトリを再起動せず二回更新してもOldは一個だけ残る()
    {
        if (OperatingSystem.IsWindows()) return;
        using var fixture = new InstallerFixture();
        var target = fixture.CreateSubDirectoryTarget("QSVEnc", "QSVEncC64.exe");
        var destination = Path.Combine(fixture.ExeFiles, target.Id);
        Directory.CreateDirectory(destination);
        fixture.WriteScript(Path.Combine(destination, "QSVEncC64.exe"), "1.0.0");

        using (var firstLog = new UpdateLog(fixture.Root))
        {
            await fixture.CreateInstaller(UpdateOSKind.Windows,
                    new DateTime(2026, 8, 11, 12, 34, 56, DateTimeKind.Utc))
                .InstallAsync(target,
                    fixture.CreateDirectoryPrepared(target, "QSVEncC64.exe", "2.0.0"),
                    "2.0.0", firstLog, CancellationToken.None);
        }
        using (var secondLog = new UpdateLog(fixture.Root))
        {
            await fixture.CreateInstaller(UpdateOSKind.Windows,
                    new DateTime(2026, 8, 11, 12, 35, 57, DateTimeKind.Utc))
                .InstallAsync(target,
                    fixture.CreateDirectoryPrepared(target, "QSVEncC64.exe", "3.0.0"),
                    "3.0.0", secondLog, CancellationToken.None);
        }

        var old = Assert.Single(Directory.EnumerateDirectories(
            fixture.ExeFiles, "QSVEnc.old.*"));
        Assert.EndsWith("QSVEnc.old.20260811-123557", old, StringComparison.Ordinal);
        Assert.Contains("version 3.0.0",
            await RunAsync(Path.Combine(destination, "QSVEncC64.exe")));
        Assert.False(Directory.Exists(Path.Combine(fixture.ExeFiles, ".update_backup")));
    }

    [Fact]
    public async Task サブディレクトリの設置後検証失敗は旧版全体を戻す()
    {
        if (OperatingSystem.IsWindows()) return;
        using var fixture = new InstallerFixture();
        var target = fixture.CreateSubDirectoryTarget("QSVEnc", "QSVEncC64.exe");
        var destination = Path.Combine(fixture.ExeFiles, target.Id);
        Directory.CreateDirectory(destination);
        fixture.WriteScript(Path.Combine(destination, "QSVEncC64.exe"), "1.0.0");
        File.WriteAllText(Path.Combine(destination, "old.dll"), "old");
        var prepared = fixture.CreateDirectoryPrepared(target, "QSVEncC64.exe", "9.9.9",
            ("new.dll", "new"));
        var installer = fixture.CreateInstaller(UpdateOSKind.Windows);
        using var log = new UpdateLog(fixture.Root);

        var exception = await Assert.ThrowsAsync<UpdateInstallException>(() =>
            installer.InstallAsync(target, prepared with { Version = "2.0.0" }, "2.0.0", log,
                CancellationToken.None));

        Assert.Equal("VERIFY_FAILED", exception.Code);
        Assert.Contains("version 1.0.0",
            await RunAsync(Path.Combine(destination, "QSVEncC64.exe")));
        Assert.True(File.Exists(Path.Combine(destination, "old.dll")));
        Assert.False(File.Exists(Path.Combine(destination, "new.dll")));
    }

    [Fact]
    public void 起動時復旧は対象ディレクトリだけなら何もしない()
    {
        using var fixture = new InstallerFixture();
        var destination = fixture.WriteDirectoryMarker("QSVEnc", "old");

        var recovered = UpdateInstaller.CleanupStartupResidues(fixture.Root,
            UpdateOSKind.Windows);

        Assert.Equal(0, recovered);
        Assert.Equal("old", File.ReadAllText(Path.Combine(destination, "marker")));
    }

    [Fact]
    public void 起動時復旧は対象とNewがあればNewだけを削除する()
    {
        using var fixture = new InstallerFixture();
        var destination = fixture.WriteDirectoryMarker("QSVEnc", "old");
        var pending = fixture.WriteDirectoryMarker("QSVEnc.new.20260811-123456", "new");

        var recovered = UpdateInstaller.CleanupStartupResidues(fixture.Root,
            UpdateOSKind.Windows);

        Assert.Equal(1, recovered);
        Assert.Equal("old", File.ReadAllText(Path.Combine(destination, "marker")));
        Assert.False(Directory.Exists(pending));
    }

    [Fact]
    public void 起動時復旧はOldとNewからNewを有効化する()
    {
        using var fixture = new InstallerFixture();
        var old = fixture.WriteDirectoryMarker("QSVEnc.old.20260811-123456", "old");
        var pending = fixture.WriteDirectoryMarker("QSVEnc.new.20260811-123456", "new");

        var recovered = UpdateInstaller.CleanupStartupResidues(fixture.Root,
            UpdateOSKind.Windows);
        var destination = Path.Combine(fixture.ExeFiles, "QSVEnc");

        Assert.Equal(2, recovered);
        Assert.Equal("new", File.ReadAllText(Path.Combine(destination, "marker")));
        Assert.False(Directory.Exists(old));
        Assert.False(Directory.Exists(pending));
    }

    [Fact]
    public void 起動時復旧は対象とOldがあればOldだけを削除する()
    {
        using var fixture = new InstallerFixture();
        var destination = fixture.WriteDirectoryMarker("QSVEnc", "new");
        var old = fixture.WriteDirectoryMarker("QSVEnc.old.20260811-123456", "old");

        var recovered = UpdateInstaller.CleanupStartupResidues(fixture.Root,
            UpdateOSKind.Windows);

        Assert.Equal(1, recovered);
        Assert.Equal("new", File.ReadAllText(Path.Combine(destination, "marker")));
        Assert.False(Directory.Exists(old));
    }

    [Fact]
    public void 起動時復旧はOldだけなら旧版を戻す()
    {
        using var fixture = new InstallerFixture();
        var old = fixture.WriteDirectoryMarker("QSVEnc.old.20260811-123456", "old");

        var recovered = UpdateInstaller.CleanupStartupResidues(fixture.Root,
            UpdateOSKind.Windows);
        var destination = Path.Combine(fixture.ExeFiles, "QSVEnc");

        Assert.Equal(1, recovered);
        Assert.Equal("old", File.ReadAllText(Path.Combine(destination, "marker")));
        Assert.False(Directory.Exists(old));
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
    public async Task MaintenanceLeaseは本体更新引き渡し後に停止を維持する()
    {
        var pauses = new List<bool>();
        var lease = await UpdateMaintenanceLease.AcquireAsync(() => false, pause =>
        {
            pauses.Add(pause);
            return Task.CompletedTask;
        }, CancellationToken.None);

        lease.KeepPaused();
        lease.Dispose();

        Assert.Equal(new[] { true }, pauses);
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

        Assert.True(UpdateManager.ShouldUpdateSettingPath(target, oldPath,
            newPath, fixture.Root, UpdateOSKind.Windows));
        Assert.False(UpdateManager.ShouldUpdateSettingPath(target,
            Path.Combine(fixture.Root, "custom", "x264_3223_x64.exe"), newPath,
            fixture.Root, UpdateOSKind.Windows));
        Assert.False(UpdateManager.ShouldUpdateSettingPath(target,
            Path.Combine(fixture.ExeFiles, "unrelated.exe"), newPath, fixture.Root,
            UpdateOSKind.Windows));
        Assert.False(UpdateManager.ShouldUpdateSettingPath(target, string.Empty,
            newPath, fixture.Root, UpdateOSKind.Windows));

        var qsvenc = Assert.Single(UpdateCatalog.Targets, item => item.Id == "QSVEnc");
        var oldQsvenc = Path.Combine(fixture.ExeFiles, "QSVEncC64.exe");
        var newQsvenc = Path.Combine(fixture.ExeFiles, "QSVEnc", "QSVEncC64.exe");
        Assert.True(UpdateManager.ShouldUpdateSettingPath(qsvenc, oldQsvenc,
            newQsvenc, fixture.Root, UpdateOSKind.Windows));
        Assert.True(UpdateManager.ShouldUpdateSettingPath(qsvenc, newQsvenc,
            newQsvenc, fixture.Root, UpdateOSKind.Windows));
        Assert.False(UpdateManager.ShouldUpdateSettingPath(qsvenc, oldQsvenc,
            Path.Combine(fixture.ExeFiles, "QSVEnc", "nested", "QSVEncC64.exe"),
            fixture.Root, UpdateOSKind.Windows));
    }

    [Fact]
    public void Linux設定パスは裸名だけを書き換え同じ絶対パスは維持する()
    {
        using var fixture = new InstallerFixture();
        var newPath = Path.Combine(fixture.ExeFiles, "tool");

        Assert.False(UpdateManager.ShouldUpdateSettingPath(fixture.Target, string.Empty,
            newPath, fixture.Root, UpdateOSKind.Linux));
        Assert.False(UpdateManager.ShouldUpdateSettingPath(fixture.Target, newPath,
            newPath, fixture.Root, UpdateOSKind.Linux));
        Assert.True(UpdateManager.ShouldUpdateSettingPath(fixture.Target, "tool",
            newPath, fixture.Root, UpdateOSKind.Linux));
        Assert.False(UpdateManager.ShouldUpdateSettingPath(fixture.Target,
            Path.Combine(fixture.Root, "custom", "tool"), newPath, fixture.Root,
            UpdateOSKind.Linux));

        Assert.True(UpdateCatalog.TryInitialize(out var error), error);
        var qsvenc = Assert.Single(UpdateCatalog.Targets, item => item.Id == "QSVEnc");
        Assert.False(UpdateManager.ShouldUpdateSettingPath(qsvenc, "qsvencc",
            Path.Combine(fixture.ExeFiles, "QSVEnc", "qsvencc"), fixture.Root,
            UpdateOSKind.Linux));
        Assert.False(UpdateManager.ShouldUpdateSettingPath(qsvenc, "QSVEncC64.exe",
            Path.Combine(fixture.ExeFiles, "QSVEncC64.exe"), fixture.Root,
            UpdateOSKind.Linux));
    }

    [Fact]
    public void Linuxの利用者指定パスは適用前に拒否する()
    {
        Assert.True(UpdateCatalog.TryInitialize(out var error), error);
        using var fixture = new InstallerFixture();
        var target = Assert.Single(UpdateCatalog.Targets, item => item.Id == "QSVEnc");
        var setting = new Setting { QSVEncPath = Path.Combine(fixture.Root, "custom", "qsvencc") };

        Assert.Equal("setting_path_outside_exe_files",
            UpdateManager.GetCannotApplyReason(target, UpdateOSKind.Linux,
                setting, fixture.Root));

        setting.QSVEncPath = Path.Combine("custom", "qsvencc");
        Assert.Equal("setting_path_outside_exe_files",
            UpdateManager.GetCannotApplyReason(target, UpdateOSKind.Linux,
                setting, fixture.Root));

        setting.QSVEncPath = "qsvencc";
        Assert.Null(UpdateManager.GetCannotApplyReason(target, UpdateOSKind.Linux,
            setting, fixture.Root));

        setting.QSVEncPath = Path.Combine(fixture.ExeFiles, "qsvencc");
        Assert.Null(UpdateManager.GetCannotApplyReason(target, UpdateOSKind.Linux,
            setting, fixture.Root));

        setting.QSVEncPath = string.Empty;
        Assert.Null(UpdateManager.GetCannotApplyReason(target, UpdateOSKind.Linux,
            setting, fixture.Root));
    }

    [Fact]
    public void Windowsの利用者指定パスは適用前に拒否する()
    {
        Assert.True(UpdateCatalog.TryInitialize(out var error), error);
        using var fixture = new InstallerFixture();
        var target = Assert.Single(UpdateCatalog.Targets, item => item.Id == "QSVEnc");
        var setting = new Setting
        {
            QSVEncPath = Path.Combine(fixture.Root, "custom", "QSVEncC64.exe"),
        };

        Assert.Equal("setting_path_outside_exe_files",
            UpdateManager.GetCannotApplyReason(target, UpdateOSKind.Windows,
                setting, fixture.Root));

        setting.QSVEncPath = Path.Combine(fixture.ExeFiles, "QSVEncC64.exe");
        Assert.Null(UpdateManager.GetCannotApplyReason(target, UpdateOSKind.Windows,
            setting, fixture.Root));

        setting.QSVEncPath = string.Empty;
        Assert.Null(UpdateManager.GetCannotApplyReason(target, UpdateOSKind.Windows,
            setting, fixture.Root));
    }

    [Fact]
    public void exe_files配下なら深さを問わず更新対象とする()
    {
        Assert.True(UpdateCatalog.TryInitialize(out var error), error);
        using var fixture = new InstallerFixture();
        var target = Assert.Single(UpdateCatalog.Targets, item => item.Id == "QSVEnc");
        var setting = new Setting();
        var installedPath = Path.Combine(fixture.ExeFiles, "QSVEnc", "QSVEncC64.exe");

        // 既定の設置場所
        setting.QSVEncPath = installedPath;
        Assert.Null(UpdateManager.GetCannotApplyReason(target, UpdateOSKind.Windows,
            setting, fixture.Root));

        // 配布書庫をそのまま展開したような、既定と異なるフォルダ名
        setting.QSVEncPath = Path.Combine(fixture.ExeFiles, "QSVEncC_8.26_x64", "QSVEncC64.exe");
        Assert.Null(UpdateManager.GetCannotApplyReason(target, UpdateOSKind.Windows,
            setting, fixture.Root));
        // 更新時は既定の設置場所へ移す
        Assert.True(UpdateManager.ShouldUpdateSettingPath(target, setting.QSVEncPath,
            installedPath, fixture.Root, UpdateOSKind.Windows));

        // さらに深い階層でも同じ
        setting.QSVEncPath = Path.Combine(fixture.ExeFiles, "QSVEnc", "x64", "QSVEncC64.exe");
        Assert.Null(UpdateManager.GetCannotApplyReason(target, UpdateOSKind.Windows,
            setting, fixture.Root));

        // exe_files と前方一致するだけの別ディレクトリは対象外
        setting.QSVEncPath = Path.Combine(fixture.Root, "exe_files_backup", "QSVEncC64.exe");
        Assert.Equal("setting_path_outside_exe_files",
            UpdateManager.GetCannotApplyReason(target, UpdateOSKind.Windows,
                setting, fixture.Root));
    }

    [Fact]
    public void ドライバスタックが必要な対象の新規インストールだけ拒否する()
    {
        Assert.True(UpdateCatalog.TryInitialize(out var error), error);
        var qsvenc = UpdateCatalog.Targets.Single(target => target.Id == "QSVEnc");
        var tsreplace = UpdateCatalog.Targets.Single(target => target.Id == "tsreplace");
        var debAsset = new ReleaseAssetInfo { Name = "qsvencc_8.26_amd64.deb" };
        var archiveAsset = new ReleaseAssetInfo { Name = "QSVEncC_8.26_x64.7z" };

        // ドライバスタックが必要な対象を deb から新規インストールする場合だけ拒否する
        Assert.Equal("fresh_install_requires_dependencies",
            UpdateManager.GetStateCannotApplyReason(qsvenc, new UpdateTargetState
            {
                Status = UpdateTargetStatus.NotInstalled,
                SelectedAsset = debAsset,
            }));
        // 既存インストールの更新は依存が解決済みなので通す
        Assert.Null(UpdateManager.GetStateCannotApplyReason(qsvenc, new UpdateTargetState
        {
            Status = UpdateTargetStatus.UpdateAvailable,
            SelectedAsset = debAsset,
        }));
        // Windows の書庫は依存の導入を伴わない
        Assert.Null(UpdateManager.GetStateCannotApplyReason(qsvenc, new UpdateTargetState
        {
            Status = UpdateTargetStatus.NotInstalled,
            SelectedAsset = archiveAsset,
        }));
        // tsreplace は deb でもベースシステムしか要求しないので新規インストールできる
        Assert.Null(UpdateManager.GetStateCannotApplyReason(tsreplace, new UpdateTargetState
        {
            Status = UpdateTargetStatus.NotInstalled,
            SelectedAsset = new ReleaseAssetInfo { Name = "tsreplace_0.19_amd64.deb" },
        }));
        Assert.Null(UpdateManager.GetStateCannotApplyReason(qsvenc, new UpdateTargetState
        {
            Status = UpdateTargetStatus.NotInstalled,
            SelectedAsset = null,
        }));
    }

    [Fact]
    public void ドライバスタックを必要とする対象はハードウェアエンコーダに限る()
    {
        Assert.True(UpdateCatalog.TryInitialize(out var error), error);

        var requiresDependencies = UpdateCatalog.Targets
            .Where(target => target.RequiresSystemDependencies)
            .Select(target => target.Id).OrderBy(id => id, StringComparer.Ordinal).ToArray();

        Assert.Equal(new[] { "NVEnc", "QSVEnc", "VCEEnc" }, requiresDependencies);
    }

    [Fact]
    public void 対応済みの配置形式とPayloadを対象ごとに判定する()
    {
        Assert.True(UpdateCatalog.TryInitialize(out var error), error);
        var environment = UpdateRuntimeEnvironment.Detect();

        var applicable = UpdateCatalog.Targets.Where(target =>
            UpdateManager.GetCannotApplyReason(target, environment.OS) == null)
            // 期待値は序数順で書いてあるので、並べ替えも序数で行う
            // (既定の比較子はカルチャ依存で "tsreplace" が "VCEEnc" より前に来る)
            .Select(target => target.Id).OrderBy(id => id, StringComparer.Ordinal).ToArray();

        var expected = new[]
        {
            "Amatsukaze", "NVEnc", "QSVEnc", "SVT-AV1", "VCEEnc", "tsreplace", "x264", "x265",
        };
        Assert.Equal(expected, applicable);

        var qsvenc = UpdateCatalog.Targets.Single(target => target.Id == "QSVEnc");
        Assert.Equal(InstallLayout.ExeFilesFlat,
            qsvenc.GetInstallLayout(UpdateOSKind.Linux));
        Assert.Null(UpdateManager.GetCannotApplyReason(qsvenc, UpdateOSKind.Linux));
        Assert.Null(UpdateManager.GetCannotApplyReason(qsvenc, UpdateOSKind.Windows));
        var application = UpdateCatalog.Targets.Single(target => target.Id == "Amatsukaze");
        Assert.Null(UpdateManager.GetCannotApplyReason(application, UpdateOSKind.Windows));
    }

    [Fact]
    public void 本体だけゲートを通しPayload未定義の通常対象は拒否する()
    {
        var application = UpdateCatalog.Targets.Single(target => target.IsApplication);
        Assert.Null(UpdateManager.GetCannotApplyReason(application, UpdateOSKind.Linux));
        Assert.Null(UpdateManager.GetCannotApplyReason(application, UpdateOSKind.Windows));

        var target = new UpdateTargetDef
        {
            Id = "payloadなし", DisplayName = "payloadなし",
            LinuxLayout = InstallLayout.ExeFilesFlat,
            WindowsLayout = InstallLayout.ExeFilesFlat,
        };
        Assert.Equal("payload_not_defined_yet",
            UpdateManager.GetCannotApplyReason(target, UpdateOSKind.Linux));
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

        public UpdateInstaller CreateInstaller(UpdateOSKind os, DateTime? instant = null) =>
            new UpdateInstaller(Root, () => instant ??
                new DateTime(2026, 8, 11, 12, 34, 56, DateTimeKind.Utc), os);

        public UpdateTargetDef CreateSubDirectoryTarget(string id, string executableName)
        {
            var target = new UpdateTargetDef
            {
                Id = id,
                DisplayName = id,
                Repository = "test/test",
                AssetRules = Array.Empty<AssetRule>(),
                WindowsLayout = InstallLayout.ExeFilesSubDir,
                LinuxLayout = InstallLayout.ExeFilesFlat,
                VersionArgument = "--version",
                VersionPattern = @"version (?<ver>\d+\.\d+\.\d+)",
                Payload = [new PayloadEntry
                {
                    Pattern = "^" + System.Text.RegularExpressions.Regex.Escape(executableName) + "$",
                }],
            };
            Assert.True(target.TryCompileRegexes(out var error), error);
            return target;
        }

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

        public PreparedUpdate CreateDirectoryPrepared(UpdateTargetDef target, string name,
            string version, params (string Name, string Content)[] additionalFiles)
        {
            var transaction = UpdateTransaction.Create(Root,
                (transactions.Count + 1).ToString("x8"));
            transactions.Add(transaction);
            var directory = transaction.GetTargetExtractDirectory(target.Id);
            var path = Path.Combine(directory, name);
            WriteScript(path, version);
            foreach (var item in additionalFiles)
            {
                File.WriteAllText(Path.Combine(directory, item.Name), item.Content);
            }
            return new PreparedUpdate(target.Id, path, name, version, directory);
        }

        public string WriteDirectoryMarker(string name, string value)
        {
            var directory = Directory.CreateDirectory(Path.Combine(ExeFiles, name)).FullName;
            File.WriteAllText(Path.Combine(directory, "marker"), value);
            return directory;
        }

        public void WriteScript(string path, string version, int exitCode = 0)
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
