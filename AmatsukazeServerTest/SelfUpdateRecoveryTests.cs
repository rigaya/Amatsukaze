using Amatsukaze.Server.Update;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class SelfUpdateRecoveryTests
{
    [Fact]
    public void 現在のWwwRootがあればNewとOld残骸を削除する()
    {
        using var fixture = new RecoveryFixture();
        fixture.CreateWwwRoot("現行");
        fixture.CreateResidue("new", "11111111", "新版");
        fixture.CreateResidue("old", "22222222", "旧版");

        Assert.Equal(2, SelfUpdateRecovery.RecoverWwwRootWithoutThrow(fixture.AppRoot));

        Assert.Equal("現行", fixture.ReadWwwRoot());
        Assert.Empty(fixture.GetResidues());
    }

    [Fact]
    public void WwwRootが無ければNew残骸を昇格する()
    {
        using var fixture = new RecoveryFixture();
        fixture.CreateResidue("new", "11111111", "新版");

        Assert.Equal(1, SelfUpdateRecovery.RecoverWwwRootWithoutThrow(fixture.AppRoot));

        Assert.Equal("新版", fixture.ReadWwwRoot());
        Assert.Empty(fixture.GetResidues());
    }

    [Fact]
    public void WwwRootが無ければOld残骸を復元する()
    {
        using var fixture = new RecoveryFixture();
        fixture.CreateResidue("old", "11111111", "旧版");

        Assert.Equal(1, SelfUpdateRecovery.RecoverWwwRootWithoutThrow(fixture.AppRoot));

        Assert.Equal("旧版", fixture.ReadWwwRoot());
        Assert.Empty(fixture.GetResidues());
    }

    [Fact]
    public void 複数のNew残骸から最新だけを昇格する()
    {
        using var fixture = new RecoveryFixture();
        var older = fixture.CreateResidue("new", "11111111", "古い新版");
        var newer = fixture.CreateResidue("new", "22222222", "新しい新版");
        Directory.SetLastWriteTimeUtc(older, DateTime.UtcNow.AddMinutes(-2));
        Directory.SetLastWriteTimeUtc(newer, DateTime.UtcNow.AddMinutes(-1));

        Assert.Equal(2, SelfUpdateRecovery.RecoverWwwRootWithoutThrow(fixture.AppRoot));

        Assert.Equal("新しい新版", fixture.ReadWwwRoot());
        Assert.Empty(fixture.GetResidues());
    }

    [Fact]
    public void 成功結果はUpdaterログと結果を取り込み成果物を片付ける()
    {
        using var fixture = new RecoveryFixture();
        var updaterLine = "[Update][f9018168][main][S22_PLACE] OK items=10";
        fixture.WriteResult("success", string.Empty, updaterLine);

        Assert.Equal(1, SelfUpdateRecovery.ImportResultWithoutThrow(fixture.AppRoot));

        var log = fixture.ReadServerUpdateLogs();
        Assert.Contains(updaterLine, log);
        Assert.Contains("[f9018168][main][S29_RESULT] OK", log);
        Assert.Contains("status=success", log);
        Assert.Contains("elapsed=4s", log);
        Assert.False(File.Exists(fixture.ResultPath));
        Assert.False(File.Exists(fixture.UpdaterLogPath));
        Assert.False(File.Exists(Path.Combine(fixture.WorkRoot, "updater_ready")));
        Assert.False(File.Exists(Path.Combine(fixture.WorkRoot, "updater.sh")));
    }

    [Fact]
    public void Rollback結果はNgで取り込みStagingを残す()
    {
        using var fixture = new RecoveryFixture();
        Directory.CreateDirectory(fixture.Staging);
        File.WriteAllText(Path.Combine(fixture.Staging, "sentinel"), "保持");
        fixture.WriteResult("rolled_back", "PLACE_FAILED",
            "[Update][f9018168][main][S23_ROLLBACK] OK restored=10");

        Assert.Equal(1, SelfUpdateRecovery.ImportResultWithoutThrow(fixture.AppRoot));

        var log = fixture.ReadServerUpdateLogs();
        Assert.Contains("[f9018168][main][S29_RESULT] NG", log);
        Assert.Contains("error_code=PLACE_FAILED", log);
        Assert.Equal("保持", File.ReadAllText(Path.Combine(fixture.Staging, "sentinel")));
    }

    [Fact]
    public void Updaterログが無くてもSkipを記録して結果取り込みを続ける()
    {
        using var fixture = new RecoveryFixture();
        fixture.WriteResult("success", string.Empty, null);

        Assert.Equal(1, SelfUpdateRecovery.ImportResultWithoutThrow(fixture.AppRoot));

        var log = fixture.ReadServerUpdateLogs();
        Assert.Contains("[main][S20_IMPORT] SKIP reason=updater_log_not_found", log);
        Assert.Contains("[main][S29_RESULT] OK", log);
    }

    [Fact]
    public void 結果が無い通常起動では取り込みを行わない()
    {
        using var fixture = new RecoveryFixture();

        Assert.Equal(0, SelfUpdateRecovery.ImportResultWithoutThrow(fixture.AppRoot));
        Assert.False(Directory.Exists(Path.Combine(fixture.AppRoot, "log", "update")));
    }

    [Fact]
    public void Stagingの有無を保留状態として記録する()
    {
        using var pendingFixture = new RecoveryFixture();
        Directory.CreateDirectory(pendingFixture.Staging);
        Assert.True(SelfUpdateRecovery.DetectPendingWithoutThrow(pendingFixture.AppRoot));
        Assert.Contains("pending=yes", pendingFixture.ReadServerUpdateLogs());

        using var cleanFixture = new RecoveryFixture();
        Assert.False(SelfUpdateRecovery.DetectPendingWithoutThrow(cleanFixture.AppRoot));
        Assert.Empty(cleanFixture.GetServerUpdateLogPaths());
    }

    [Fact]
    public void 更新が絡まない通常起動を繰り返してもログを作らない()
    {
        using var fixture = new RecoveryFixture();

        for (var count = 0; count < 5; count++)
        {
            Assert.False(SelfUpdateRecovery.RunStartupRecovery(fixture.AppRoot).HasPending);
        }

        Assert.Empty(fixture.GetServerUpdateLogPaths());
    }

    [Fact]
    public void 結果と残骸を一つの結果Txidログへ取り込む()
    {
        using var fixture = new RecoveryFixture();
        fixture.CreateResidue("new", "11111111", "新版");
        fixture.WriteResult("success", string.Empty,
            "[Update][f9018168][main][S22_PLACE] OK items=10");

        var state = SelfUpdateRecovery.RunStartupRecovery(fixture.AppRoot);
        Assert.False(state.HasPending);
        Assert.NotNull(state.LastResult);
        Assert.Equal("success", state.LastResult.Status);
        Assert.Equal("1.0.8.8", state.LastResult.Version);
        Assert.Equal(string.Empty, state.LastResult.ErrorCode);

        var paths = fixture.GetServerUpdateLogPaths();
        Assert.Single(paths);
        Assert.EndsWith("_f9018168.log", paths[0], StringComparison.Ordinal);
        var content = File.ReadAllText(paths[0]);
        Assert.Contains("[f9018168][main][S13_ROLLBACK] OK", content);
        Assert.Contains("[f9018168][main][S22_PLACE] OK", content);
        Assert.Contains("[f9018168][main][S29_RESULT] OK", content);
    }

    [Fact]
    public void 結果が無い残骸復旧は残骸Txidの一ログへ記録する()
    {
        using var fixture = new RecoveryFixture();
        fixture.CreateResidue("old", "1234abcd", "旧版");

        Assert.False(SelfUpdateRecovery.RunStartupRecovery(fixture.AppRoot).HasPending);

        var paths = fixture.GetServerUpdateLogPaths();
        Assert.Single(paths);
        Assert.EndsWith("_1234abcd.log", paths[0], StringComparison.Ordinal);
        Assert.Contains("[1234abcd][main][S13_ROLLBACK] OK",
            File.ReadAllText(paths[0]));
    }

    [Fact]
    public void 保留Stagingだけを破棄しBackupと外部を保持する()
    {
        using var fixture = new RecoveryFixture();
        Directory.CreateDirectory(fixture.Staging);
        var backup = Path.Combine(fixture.WorkRoot, "backup", "20260812-120000");
        Directory.CreateDirectory(backup);
        File.WriteAllText(Path.Combine(backup, "sentinel"), "保持");
        var outside = Path.Combine(fixture.AppRoot, "outside");
        Directory.CreateDirectory(outside);
        File.WriteAllText(Path.Combine(outside, "sentinel"), "保持");

        SelfUpdateRecovery.DiscardPendingStaging(fixture.AppRoot);

        Assert.False(Directory.Exists(fixture.Staging));
        Assert.Equal("保持", File.ReadAllText(Path.Combine(backup, "sentinel")));
        Assert.Equal("保持", File.ReadAllText(Path.Combine(outside, "sentinel")));
    }

    [Fact]
    public void 適用失敗後の再評価で保留状態をFalseからTrueへ更新する()
    {
        using var fixture = new RecoveryFixture();
        var state = new SelfUpdatePendingState(false);
        Directory.CreateDirectory(fixture.Staging);

        state.Refresh(fixture.AppRoot);

        Assert.True(state.Value);
    }

    private sealed class RecoveryFixture : IDisposable
    {
        public string Root { get; } = Path.Combine(Path.GetTempPath(),
            "amatsukaze-self-recovery-" + Guid.NewGuid().ToString("N"));
        public string AppRoot { get; }
        public string ExecutableRoot { get; }
        public string WorkRoot { get; }
        public string Staging { get; }
        public string ResultPath { get; }
        public string UpdaterLogPath { get; }

        public RecoveryFixture()
        {
            AppRoot = Path.Combine(Root, "app");
            ExecutableRoot = Path.Combine(AppRoot, "exe_files");
            WorkRoot = Path.Combine(AppRoot, ".amatsukaze_update");
            Staging = Path.Combine(WorkRoot, "staging");
            ResultPath = Path.Combine(WorkRoot, "result.txt");
            UpdaterLogPath = Path.Combine(WorkRoot, "update_20260812-012345.log");
            Directory.CreateDirectory(ExecutableRoot);
            Directory.CreateDirectory(WorkRoot);
        }

        public void CreateWwwRoot(string content)
        {
            var path = Path.Combine(ExecutableRoot, "wwwroot");
            Directory.CreateDirectory(path);
            File.WriteAllText(Path.Combine(path, "sentinel"), content);
        }

        public string CreateResidue(string kind, string txid, string content)
        {
            var path = Path.Combine(ExecutableRoot, $"wwwroot.{kind}.{txid}");
            Directory.CreateDirectory(path);
            File.WriteAllText(Path.Combine(path, "sentinel"), content);
            return path;
        }

        public string ReadWwwRoot() => File.ReadAllText(Path.Combine(ExecutableRoot,
            "wwwroot", "sentinel"));

        public string[] GetResidues() => Directory.GetDirectories(ExecutableRoot)
            .Where(path => Path.GetFileName(path).StartsWith("wwwroot.",
                StringComparison.Ordinal)).ToArray();

        public void WriteResult(string status, string errorCode, string? updaterLine)
        {
            if (updaterLine != null) File.WriteAllText(UpdaterLogPath, updaterLine + "\n");
            File.WriteAllText(Path.Combine(WorkRoot, "updater_ready"), string.Empty);
            File.WriteAllText(Path.Combine(WorkRoot, "updater.sh"), "#!/bin/sh\n");
            File.WriteAllLines(ResultPath, new[]
            {
                "txid=f9018168",
                "status=" + status,
                "version=1.0.8.8",
                "started=2026-08-11T23:26:29Z",
                "finished=2026-08-11T23:26:33Z",
                "error_code=" + errorCode,
                "log=" + UpdaterLogPath,
            });
        }

        public string ReadServerUpdateLogs()
        {
            return string.Join("\n", GetServerUpdateLogPaths()
                .OrderBy(path => path, StringComparer.Ordinal).Select(File.ReadAllText));
        }

        public string[] GetServerUpdateLogPaths()
        {
            var directory = Path.Combine(AppRoot, "log", "update");
            return Directory.Exists(directory) ? Directory.GetFiles(directory, "*.log") :
                Array.Empty<string>();
        }

        public void Dispose()
        {
            if (Directory.Exists(Root)) Directory.Delete(Root, true);
        }
    }
}
