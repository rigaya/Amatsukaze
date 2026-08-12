using Amatsukaze.Server.Update;
using System.Diagnostics;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class SelfUpdaterLauncherTests
{
    [Fact]
    public async Task UpdaterのReady通知を確認して処理を返す()
    {
        if (OperatingSystem.IsWindows()) return;
        using var fixture = new LauncherFixture();
        fixture.WriteScript("#!/bin/sh\nprintf ready > \"$1\"\nsleep 2\n", includeReadyArgument: true);

        await SelfUpdaterLauncher.StartAndWaitReadyAsync(fixture.Updater,
            UpdateOSKind.Linux, CancellationToken.None, TimeSpan.FromSeconds(2));

        Assert.True(File.Exists(fixture.ReadyPath));
    }

    [Fact]
    public async Task Updater起動失敗ではStagingを残す()
    {
        if (OperatingSystem.IsWindows()) return;
        using var fixture = new LauncherFixture();
        fixture.WriteScript("#!/bin/sh\nexit 1\n");
        File.WriteAllText(fixture.ReadyPath, "古い通知");

        var exception = await Assert.ThrowsAsync<UpdateInstallException>(() =>
            SelfUpdaterLauncher.StartAndWaitReadyAsync(fixture.Updater,
                UpdateOSKind.Linux, CancellationToken.None, TimeSpan.FromSeconds(1)));

        Assert.Equal("UPDATER_START_FAILED", exception.Code);
        Assert.False(File.Exists(fixture.ReadyPath));
        Assert.True(Directory.Exists(fixture.Staging));
        Assert.Equal("変更禁止", File.ReadAllText(Path.Combine(fixture.Staging, "sentinel")));
    }

    [Fact]
    public async Task Updater起動失敗ではMaintenancePauseを解除する()
    {
        if (OperatingSystem.IsWindows()) return;
        using var fixture = new LauncherFixture();
        fixture.WriteScript("#!/bin/sh\nexit 1\n");
        var pauses = new List<bool>();

        await Assert.ThrowsAsync<UpdateInstallException>(async () =>
        {
            using var lease = await UpdateMaintenanceLease.AcquireAsync(() => false, pause =>
            {
                pauses.Add(pause);
                return Task.CompletedTask;
            }, CancellationToken.None);
            await SelfUpdaterLauncher.StartAndWaitReadyAsync(fixture.Updater,
                UpdateOSKind.Linux, CancellationToken.None, TimeSpan.FromSeconds(1));
        });

        Assert.Equal(new[] { true, false }, pauses);
        Assert.True(Directory.Exists(fixture.Staging));
    }

    [Fact]
    public async Task Updater起動コマンドが無い場合は開始失敗にする()
    {
        if (OperatingSystem.IsWindows()) return;
        using var fixture = new LauncherFixture();
        fixture.WriteScript("#!/bin/sh\nprintf unexpected > \"$1\"\n", includeReadyArgument: true);

        var exception = await Assert.ThrowsAsync<UpdateInstallException>(() =>
            SelfUpdaterLauncher.StartAndWaitReadyAsync(fixture.Updater,
                UpdateOSKind.Linux, CancellationToken.None, TimeSpan.FromSeconds(1),
                Path.Combine(fixture.Root, "存在しないsetsid")));

        Assert.Equal("UPDATER_START_FAILED", exception.Code);
        Assert.False(File.Exists(fixture.ReadyPath));
        Assert.True(Directory.Exists(fixture.Staging));
    }

    [Fact]
    public async Task Readyを出さないUpdaterが生存中でもTimeoutで開始失敗にする()
    {
        if (OperatingSystem.IsWindows()) return;
        using var fixture = new LauncherFixture();
        var pidPath = Path.Combine(fixture.Root, "updater.pid");
        fixture.WriteScript("#!/bin/sh\nprintf '%s' \"$$\" > \"" + pidPath + "\"\nsleep 2\n");
        var elapsed = Stopwatch.StartNew();

        var exception = await Assert.ThrowsAsync<UpdateInstallException>(() =>
            SelfUpdaterLauncher.StartAndWaitReadyAsync(fixture.Updater,
                UpdateOSKind.Linux, CancellationToken.None, TimeSpan.FromMilliseconds(400)));

        Assert.Equal("UPDATER_START_FAILED", exception.Code);
        Assert.True(elapsed.Elapsed >= TimeSpan.FromMilliseconds(300));
        Assert.True(File.Exists(pidPath));
        using var updaterProcess = Process.GetProcessById(int.Parse(File.ReadAllText(pidPath)));
        Assert.False(updaterProcess.HasExited);
        Assert.True(Directory.Exists(fixture.Staging));
    }

    [Fact]
    public void 保留中のStagingがある場合は新しい準備を拒否する()
    {
        using var fixture = new LauncherFixture();

        var exception = Assert.Throws<UpdatePreparationException>(() =>
            SelfUpdateWorkspace.EnsureNoPendingUpdate(fixture.AppRoot));

        Assert.Equal("SELF_UPDATE_PENDING", exception.Code);
        Assert.Equal("変更禁止", File.ReadAllText(Path.Combine(fixture.Staging, "sentinel")));
    }

    [Fact]
    public void 本体と他対象の同時適用を拒否する()
    {
        var application = UpdateCatalog.Targets.Single(target => target.IsApplication);
        var encoder = UpdateCatalog.Targets.First(target => !target.IsApplication);
        var asset = new ReleaseAssetInfo
        {
            Name = "dummy", BrowserDownloadUrl = "https://example.invalid/dummy", Size = 1,
        };
        var targets = new[]
        {
            new UpdateManager.UpdateApplyTarget(application, asset, "1.0.0"),
            new UpdateManager.UpdateApplyTarget(encoder, asset, "1.0.0"),
        };

        var exception = Assert.Throws<UpdateInstallException>(() =>
            UpdateManager.ValidateSelfUpdateSelection(targets, null));

        Assert.Equal("SELF_UPDATE_NOT_ALONE", exception.Code);
    }

    private sealed class LauncherFixture : IDisposable
    {
        public string Root { get; } = Path.Combine(Path.GetTempPath(),
            "amatsukaze-launcher-" + Guid.NewGuid().ToString("N"));
        public string AppRoot { get; }
        public string Staging { get; }
        public string ScriptPath { get; }
        public string ReadyPath { get; }
        public GeneratedSelfUpdater Updater => new(ScriptPath,
            Path.Combine(Root, "update.log"), ReadyPath, Path.Combine(Root, "result.txt"),
            Path.Combine(Root, "backup"));

        public LauncherFixture()
        {
            AppRoot = Path.Combine(Root, "app");
            Staging = Path.Combine(AppRoot, ".amatsukaze_update", "staging");
            ScriptPath = Path.Combine(Root, "updater.sh");
            ReadyPath = Path.Combine(Root, "updater_ready");
            Directory.CreateDirectory(Staging);
            File.WriteAllText(Path.Combine(Staging, "sentinel"), "変更禁止");
        }

        public void WriteScript(string content, bool includeReadyArgument = false)
        {
            if (includeReadyArgument)
            {
                // テスト用スクリプトへ ready パスを埋め込み、製品側の引数仕様には依存しない。
                content = content.Replace("$1", ReadyPath.Replace("\\", "\\\\")
                    .Replace("\"", "\\\""));
            }
            File.WriteAllText(ScriptPath, content);
            if (!OperatingSystem.IsWindows())
            {
                File.SetUnixFileMode(ScriptPath, UnixFileMode.UserRead |
                    UnixFileMode.UserWrite | UnixFileMode.UserExecute);
            }
        }

        public void Dispose()
        {
            if (Directory.Exists(Root)) Directory.Delete(Root, true);
        }
    }
}
