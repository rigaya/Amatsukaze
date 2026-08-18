using Amatsukaze.Server.Update;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class SelfUpdatePreparationTests
{
    [Fact]
    public void 未知のトップレベル項目を拒否する()
    {
        using var fixture = new SelfUpdateFixture();
        Directory.CreateDirectory(Path.Combine(fixture.Staging, "exe_files"));
        File.WriteAllText(Path.Combine(fixture.Staging, "unknown.txt"), "不明");

        var exception = Assert.Throws<UpdatePreparationException>(() =>
            SelfUpdatePolicy.ValidateTopLevel(fixture.Staging, UpdateOSKind.Linux));

        Assert.Equal("UNEXPECTED_ARCHIVE_LAYOUT", exception.Code);
    }

    [Fact]
    public void 許可項目が欠けていてもトップレベル検証を通す()
    {
        using var fixture = new SelfUpdateFixture();
        Directory.CreateDirectory(Path.Combine(fixture.Staging, "exe_files"));
        Directory.CreateDirectory(Path.Combine(fixture.Staging, "profile"));
        File.WriteAllText(Path.Combine(fixture.Staging, "AmatsukazeServer.sh"), "起動");

        SelfUpdatePolicy.ValidateTopLevel(fixture.Staging, UpdateOSKind.Linux);
    }

    [Fact]
    public void ルート直下ファイルの許可リストは配布構成と一致する()
    {
        Assert.Equal(new[] { "AmatsukazeServer.sh" },
            SelfUpdatePolicy.GetRootFiles(UpdateOSKind.Linux).OrderBy(item => item,
                StringComparer.Ordinal));
        Assert.Equal(new[]
        {
            "Amatsukaze.bat",
            "AmatsukazeClient.bat",
            "AmatsukazeServer.bat",
            "AmatsukazeServerCLI.bat",
            "Readme.txt",
        }, SelfUpdatePolicy.GetRootFiles(UpdateOSKind.Windows).OrderBy(item => item,
            StringComparer.OrdinalIgnoreCase));
    }

    [Theory]
    [InlineData(0)]
    [InlineData(1)]
    public void 実測済みトップレベル構成を許可する(int osValue)
    {
        var os = (UpdateOSKind)osValue;
        using var fixture = new SelfUpdateFixture();
        var directories = os == UpdateOSKind.Windows
            ? new[] { "JL", "avs", "bat", "exe_files", "profile" }
            : new[] { "JL", "avs", "avscache", "bat", "drcs", "exe_files", "logo",
                "profile", "scripts" };
        foreach (var directory in directories)
        {
            Directory.CreateDirectory(Path.Combine(fixture.Staging, directory));
        }
        foreach (var file in SelfUpdatePolicy.GetRootFiles(os))
        {
            File.WriteAllText(Path.Combine(fixture.Staging, file), "配布物");
        }

        SelfUpdatePolicy.ValidateTopLevel(fixture.Staging, os);
    }

    [Fact]
    public void 許可リストは上書き規則を区別する()
    {
        Assert.Equal(SelfUpdatePathAction.OverwriteCopy,
            SelfUpdatePolicy.GetAction("exe_files/AmatsukazeServerCLI", UpdateOSKind.Linux));
        Assert.Equal(SelfUpdatePathAction.ReplaceDirectory,
            SelfUpdatePolicy.GetAction("exe_files/wwwroot/index.html", UpdateOSKind.Linux));
        Assert.Equal(SelfUpdatePathAction.AddOnly,
            SelfUpdatePolicy.GetAction("JL/user/custom.lua", UpdateOSKind.Linux));
        Assert.Equal(SelfUpdatePathAction.AddOnly,
            SelfUpdatePolicy.GetAction("profile/custom.profile", UpdateOSKind.Linux));
        Assert.Equal(SelfUpdatePathAction.Ignore,
            SelfUpdatePolicy.GetAction("data/Server.lock", UpdateOSKind.Linux));
        Assert.Equal(SelfUpdatePathAction.OverwriteCopy,
            SelfUpdatePolicy.GetAction("scripts/install.sh", UpdateOSKind.Linux));
        Assert.Equal(SelfUpdatePathAction.Reject,
            SelfUpdatePolicy.GetAction("scripts/install.sh", UpdateOSKind.Windows));
        Assert.Equal(SelfUpdatePathAction.Reject,
            SelfUpdatePolicy.GetAction("../outside", UpdateOSKind.Linux));
    }

    [Theory]
    [InlineData(0, true)]
    [InlineData(0, false)]
    [InlineData(1, true)]
    [InlineData(1, false)]
    public void 検証対象ファイルが無ければ拒否する(int osValue, bool executableMissing)
    {
        var os = (UpdateOSKind)osValue;
        using var fixture = new SelfUpdateFixture();
        var (executable, library) = GetProductNames(os);
        CreateProductFiles(fixture.Staging, executable, library);
        File.Delete(Path.Combine(fixture.Staging, "exe_files",
            executableMissing ? executable : library));

        var exception = Assert.Throws<UpdatePreparationException>(() =>
            SelfUpdatePreparation.ValidateProduct(fixture.Staging, os));

        Assert.Equal("VERIFY_FAILED", exception.Code);
    }

    [Theory]
    [InlineData(0, true)]
    [InlineData(0, false)]
    [InlineData(1, true)]
    [InlineData(1, false)]
    public void 検証対象ファイルが空なら拒否する(int osValue, bool executableEmpty)
    {
        var os = (UpdateOSKind)osValue;
        using var fixture = new SelfUpdateFixture();
        var (executable, library) = GetProductNames(os);
        CreateProductFiles(fixture.Staging, executable, library);
        File.WriteAllBytes(Path.Combine(fixture.Staging, "exe_files",
            executableEmpty ? executable : library), []);

        var exception = Assert.Throws<UpdatePreparationException>(() =>
            SelfUpdatePreparation.ValidateProduct(fixture.Staging, os));

        Assert.Equal("VERIFY_FAILED", exception.Code);
    }

    [Fact]
    public void 本体更新領域はUpdateTmpの起動時掃除で消えない()
    {
        using var fixture = new SelfUpdateFixture();
        var sentinel = Path.Combine(fixture.Staging, "sentinel");
        File.WriteAllText(sentinel, "保持");
        var stale = Path.Combine(fixture.AppRoot, "exe_files", ".update_tmp", "deadbeef");
        Directory.CreateDirectory(stale);
        File.WriteAllText(Path.Combine(stale, "partial"), "削除");

        UpdateTransaction.CleanupStale(fixture.AppRoot);

        Assert.False(Directory.Exists(stale));
        Assert.Equal("保持", File.ReadAllText(sentinel));
    }

    [Fact]
    public void 本体更新領域外のパスを拒否する()
    {
        using var fixture = new SelfUpdateFixture();
        var outside = Path.Combine(fixture.AppRoot, "outside");

        var exception = Assert.Throws<UpdatePreparationException>(() =>
            SelfUpdateWorkspace.ValidateChild(fixture.WorkRoot, outside));

        Assert.Equal("INVALID_STAGING_PATH", exception.Code);
    }

    [Fact]
    public void 検証済み候補だけを永続ステージングへ昇格する()
    {
        using var fixture = new SelfUpdateFixture();
        File.WriteAllText(Path.Combine(fixture.Staging, "old"), "旧候補");
        using var workspace = SelfUpdateWorkspace.Create(fixture.AppRoot, "1234abcd");
        File.WriteAllText(Path.Combine(workspace.CandidateDirectory, "new"), "新候補");

        workspace.Commit();

        Assert.False(File.Exists(Path.Combine(fixture.Staging, "old")));
        Assert.Equal("新候補", File.ReadAllText(Path.Combine(fixture.Staging, "new")));
        Assert.StartsWith(Path.Combine(fixture.AppRoot, ".amatsukaze_update"),
            workspace.StagingDirectory, StringComparison.Ordinal);
        Assert.DoesNotContain(Path.Combine("exe_files", ".update_tmp"),
            workspace.StagingDirectory, StringComparison.Ordinal);
    }

    [Fact]
    public void 再起動用コマンドラインから実行ファイル自身を除く()
    {
        var command = SelfUpdatePreparation.CaptureRestartCommandLine();

        Assert.Equal(Environment.ProcessPath, command.ProcessPath);
        Assert.DoesNotContain(command.Arguments.Take(1), argument =>
            IsSamePath(argument, command.ProcessPath) ||
            IsSamePath(argument, Path.ChangeExtension(command.ProcessPath, ".dll")));
    }

    [Fact]
    public void AppHost起動では実行アセンブリを再起動引数から除く()
    {
        var command = SelfUpdatePreparation.BuildRestartCommandLine("/opt/amt/server",
            new[] { "/opt/amt/server.dll", "-p", "32768" });

        Assert.Equal(new[] { "-p", "32768" }, command.Arguments);
    }

    [Fact]
    public void Dotnet起動では実行アセンブリを再起動引数に残す()
    {
        var command = SelfUpdatePreparation.BuildRestartCommandLine("/usr/bin/dotnet",
            new[] { "/opt/amt/server.dll", "-p", "32768" });

        Assert.Equal(new[] { "/opt/amt/server.dll", "-p", "32768" }, command.Arguments);
    }

    [Fact]
    public void 単一ファイル起動では実行ファイル自身を再起動引数から除く()
    {
        var command = SelfUpdatePreparation.BuildRestartCommandLine("/opt/amt/server",
            new[] { "/opt/amt/server", "-p", "32768" });

        Assert.Equal(new[] { "-p", "32768" }, command.Arguments);
    }

    [Fact]
    public void 起動時に旧トランザクションの作業領域だけを掃除する()
    {
        using var fixture = new SelfUpdateFixture();
        var oldDownload = Path.Combine(fixture.WorkRoot, ".download.deadbeef");
        var oldStaging = Path.Combine(fixture.WorkRoot, ".staging.0123abcd");
        var unrelated = Path.Combine(fixture.WorkRoot, ".download.user");
        Directory.CreateDirectory(oldDownload);
        Directory.CreateDirectory(oldStaging);
        Directory.CreateDirectory(unrelated);
        File.WriteAllText(Path.Combine(fixture.Staging, "keep"), "保持");

        using var workspace = SelfUpdateWorkspace.Create(fixture.AppRoot, "1234abcd");

        Assert.False(Directory.Exists(oldDownload));
        Assert.False(Directory.Exists(oldStaging));
        Assert.True(Directory.Exists(unrelated));
        Assert.Equal("保持", File.ReadAllText(Path.Combine(fixture.Staging, "keep")));
    }

    private static bool IsSamePath(string left, string right)
    {
        if (string.IsNullOrWhiteSpace(left) || string.IsNullOrWhiteSpace(right)) return false;
        var comparison = OperatingSystem.IsWindows()
            ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
        return string.Equals(Path.GetFullPath(left), Path.GetFullPath(right), comparison);
    }

    private static (string Executable, string Library) GetProductNames(UpdateOSKind os) =>
        os == UpdateOSKind.Windows
            ? ("AmatsukazeServerCLI.exe", "Amatsukaze.dll")
            : ("AmatsukazeServerCLI", "libAmatsukaze.so");

    private static void CreateProductFiles(string staging, string executable, string library)
    {
        var exeFiles = Path.Combine(staging, "exe_files");
        Directory.CreateDirectory(exeFiles);
        using (var stream = File.Create(Path.Combine(exeFiles, executable)))
        {
            stream.SetLength(2L * 1024 * 1024);
        }
        using (var stream = File.Create(Path.Combine(exeFiles, library)))
        {
            stream.SetLength(128L * 1024);
        }
    }

    private sealed class SelfUpdateFixture : IDisposable
    {
        public SelfUpdateFixture()
        {
            AppRoot = Path.Combine(Path.GetTempPath(), "amatsukaze-self-update-test-" +
                Guid.NewGuid());
            WorkRoot = Path.Combine(AppRoot, ".amatsukaze_update");
            Staging = Path.Combine(WorkRoot, "staging");
            Directory.CreateDirectory(Staging);
            Directory.CreateDirectory(Path.Combine(AppRoot, "exe_files"));
        }

        public string AppRoot { get; }
        public string WorkRoot { get; }
        public string Staging { get; }

        public void Dispose()
        {
            Directory.Delete(AppRoot, true);
        }
    }
}
