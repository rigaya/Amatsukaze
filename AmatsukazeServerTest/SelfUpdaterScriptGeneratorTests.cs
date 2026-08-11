using System.Diagnostics;
using Amatsukaze.Server.Update;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class SelfUpdaterScriptGeneratorTests
{
    [Fact]
    public async Task Linux更新スクリプトは許可規則どおりに配置する()
    {
        using var fixture = new UpdaterFixture("通常 系 空白");
        fixture.CreateStaging();
        fixture.CreateInstalledFiles();
        var generated = fixture.Generate(UpdateOSKind.Linux);

        var result = await RunAsync(generated.ScriptPath);

        Assert.True(result.ExitCode == 0,
            $"exit={result.ExitCode} stdout={result.Stdout} stderr={result.Stderr}");
        Assert.Equal("new-tool", fixture.Read("exe_files/tool"));
        Assert.Equal("new-root", fixture.Read("AmatsukazeServer.sh"));
        Assert.Equal("new-base", fixture.Read("JL/base.txt"));
        Assert.Equal("mine", fixture.Read("JL/user/mine.lua"));
        Assert.Equal("mine-avs", fixture.Read("avs/mine.avs"));
        Assert.Equal("user-profile", fixture.Read("profile/sample.profile"));
        Assert.Equal("sample-avs", fixture.Read("avs/sample.avs"));
        Assert.Equal("mine-logo", fixture.Read("logo/x.lgd"));
        Assert.Equal("mine-config", fixture.Read("config/settings.xml"));
        Assert.Equal("mine-data", fixture.Read("data/state.dat"));
        Assert.Equal("keep", fixture.Read("exe_files/mytool"));
        Assert.False(fixture.Exists("exe_files/wwwroot/_framework/OLD.HASH.wasm"));
        Assert.Equal("new-index", fixture.Read("exe_files/wwwroot/index.html"));
        Assert.Contains("status=success", File.ReadAllText(generated.ResultPath));
        Assert.False(Directory.Exists(fixture.Staging));
        Assert.False(Directory.Exists(generated.BackupDirectory));
        Assert.True(File.Exists(generated.ReadyPath));
    }

    [Fact]
    public async Task 配置失敗時は全変更をバックアップから復元する()
    {
        using var fixture = new UpdaterFixture("rollback");
        fixture.CreateStaging();
        fixture.CreateInstalledFiles();
        var before = fixture.Snapshot();
        var generated = fixture.Generate(UpdateOSKind.Linux,
            new SelfUpdaterScriptOptions(FailAfterPlacedItems: 7));

        var result = await RunAsync(generated.ScriptPath);

        Assert.NotEqual(0, result.ExitCode);
        Assert.Equal(before, fixture.Snapshot());
        Assert.Contains("status=rolled_back", File.ReadAllText(generated.ResultPath));
        Assert.True(Directory.Exists(fixture.Staging));
        Assert.True(Directory.Exists(generated.BackupDirectory));
        var created = File.ReadAllText(Path.Combine(generated.BackupDirectory,
            "created_paths.txt"));
        Assert.Contains("JL/user/archive.lua", created);
        Assert.Contains("avs/sample.avs", created);
    }

    [Fact]
    public async Task 待機タイムアウトでは既存ファイルに触れない()
    {
        using var fixture = new UpdaterFixture("timeout");
        fixture.CreateStaging();
        fixture.CreateInstalledFiles();
        var before = fixture.Snapshot();
        var generated = fixture.Generate(UpdateOSKind.Linux,
            new SelfUpdaterScriptOptions(WaitTimeoutSeconds: 1, PollIntervalSeconds: 1),
            Environment.ProcessId);

        var result = await RunAsync(generated.ScriptPath);

        Assert.Equal(2, result.ExitCode);
        Assert.Equal(before, fixture.Snapshot());
        Assert.Contains("status=timeout", File.ReadAllText(generated.ResultPath));
        Assert.True(Directory.Exists(fixture.Staging));
    }

    [Fact]
    public void 生成スクリプトは許可リストを二重定義しない()
    {
        using var fixture = new UpdaterFixture("policy");
        fixture.CreateStaging();

        var generated = fixture.Generate(UpdateOSKind.Linux);
        var script = File.ReadAllText(generated.ScriptPath);

        Assert.Contains("place_file 'exe_files/tool'", script);
        Assert.Contains("place_addition 'profile/sample.profile'", script);
        Assert.Contains("place_wwwroot", script);
        Assert.DoesNotContain("logo/archive.lgd", script);
        Assert.DoesNotContain("data/archive.dat", script);
    }

    [Fact]
    public void Windows更新スクリプトはCRLFと安全な定数を使う()
    {
        using var fixture = new UpdaterFixture("batch");
        fixture.CreateWindowsStaging();

        var generated = fixture.Generate(UpdateOSKind.Windows);
        var bytes = File.ReadAllBytes(generated.ScriptPath);
        var script = File.ReadAllText(generated.ScriptPath);

        Assert.DoesNotContain("\n", script.Replace("\r\n", string.Empty));
        Assert.Contains("ROBOCOPY_SUCCESS_MAX=7", script);
        Assert.Contains("ROBOCOPY_FAILURE_MIN=8", script);
        Assert.Contains("if not exist \"%RESTART_EXE%\"", script);
        Assert.Contains("call :place_file \"exe_files\\tool.exe\"", script);
        Assert.DoesNotContain("data\\archive.dat", script);
        Assert.True(bytes.Length > 2);
    }

    [Fact]
    public void 引用符を安全に埋め込めない場合は生成を拒否する()
    {
        using var fixture = new UpdaterFixture("quote");
        fixture.CreateStaging();
        var prepared = fixture.Prepared with
        {
            RestartCommandLine = new RestartCommandLine("/tmp/has'quote", []),
        };

        var exception = Assert.Throws<UpdatePreparationException>(() =>
            SelfUpdaterScriptGenerator.Generate(fixture.AppRoot, 999999, prepared,
                "1234abcd", UpdateOSKind.Linux, fixture.GeneratedAt));

        Assert.Equal("UNSAFE_SCRIPT_ARGUMENT", exception.Code);
    }

    [Fact]
    public async Task 再起動実行ファイルが無い場合は配置後に失敗を記録する()
    {
        using var fixture = new UpdaterFixture("restart missing");
        fixture.CreateStaging();
        fixture.CreateInstalledFiles();
        var prepared = fixture.Prepared with
        {
            RestartCommandLine = new RestartCommandLine(
                Path.Combine(fixture.Root, "存在しないサーバー"), []),
        };
        var generated = SelfUpdaterScriptGenerator.Generate(fixture.AppRoot, 999999,
            prepared, "1234abcd", UpdateOSKind.Linux, fixture.GeneratedAt);

        var result = await RunAsync(generated.ScriptPath);

        Assert.NotEqual(0, result.ExitCode);
        Assert.Equal("new-tool", fixture.Read("exe_files/tool"));
        var resultText = File.ReadAllText(generated.ResultPath);
        Assert.Contains("status=failed", resultText);
        Assert.Contains("error_code=RESTART_FAILED", resultText);
        Assert.Contains("reason=not_executable", File.ReadAllText(generated.LogPath));
    }

    private static async Task<ProcessResult> RunAsync(string scriptPath)
    {
        using var process = new Process
        {
            StartInfo = new ProcessStartInfo
            {
                FileName = scriptPath,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
            },
        };
        process.Start();
        var stdout = process.StandardOutput.ReadToEndAsync();
        var stderr = process.StandardError.ReadToEndAsync();
        await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(15));
        return new ProcessResult(process.ExitCode, await stdout, await stderr);
    }

    private sealed record ProcessResult(int ExitCode, string Stdout, string Stderr);

    private sealed class UpdaterFixture : IDisposable
    {
        public string Root { get; }
        public string AppRoot { get; }
        public string Staging { get; }
        public DateTime GeneratedAt { get; } = new DateTime(2026, 8, 12, 1, 23, 45,
            DateTimeKind.Utc);
        public PreparedSelfUpdate Prepared => new(
            Path.Combine(AppRoot, ".amatsukaze_update"), Staging, "1.0.8.8",
            new RestartCommandLine("/bin/sleep", ["5"]));

        public UpdaterFixture(string name)
        {
            Root = Path.Combine(Path.GetTempPath(), "amatsukaze-p4b-" + name + "-" +
                Guid.NewGuid().ToString("N"));
            AppRoot = Path.Combine(Root, "app root");
            Staging = Path.Combine(AppRoot, ".amatsukaze_update", "staging");
            Directory.CreateDirectory(Staging);
        }

        public void CreateStaging()
        {
            WriteStaging("AmatsukazeServer.sh", "new-root");
            WriteStaging("exe_files/tool", "new-tool");
            WriteStaging("exe_files/wwwroot/index.html", "new-index");
            WriteStaging("JL/base.txt", "new-base");
            WriteStaging("JL/user/archive.lua", "archive-user");
            WriteStaging("avs/sample.avs", "sample-avs");
            WriteStaging("profile/sample.profile", "sample-profile");
            WriteStaging("scripts/install.sh", "new-script");
            WriteStaging("logo/archive.lgd", "ignored-logo");
            WriteStaging("data/archive.dat", "ignored-data");
        }

        public void CreateWindowsStaging()
        {
            WriteStaging("AmatsukazeServer.bat", "new-root");
            WriteStaging("exe_files/tool.exe", "new-tool");
            WriteStaging("exe_files/wwwroot/index.html", "new-index");
            WriteStaging("profile/sample.profile", "sample-profile");
            WriteStaging("data/archive.dat", "ignored-data");
        }

        public void CreateInstalledFiles()
        {
            Write("AmatsukazeServer.sh", "old-root");
            Write("exe_files/tool", "old-tool");
            Write("exe_files/mytool", "keep");
            Write("exe_files/wwwroot/index.html", "old-index");
            Write("exe_files/wwwroot/_framework/OLD.HASH.wasm", "old-wasm");
            Write("JL/base.txt", "old-base");
            Write("JL/user/mine.lua", "mine");
            Write("avs/mine.avs", "mine-avs");
            Write("profile/sample.profile", "user-profile");
            Write("logo/x.lgd", "mine-logo");
            Write("config/settings.xml", "mine-config");
            Write("data/state.dat", "mine-data");
        }

        public GeneratedSelfUpdater Generate(UpdateOSKind os,
            SelfUpdaterScriptOptions? options = null, long serverPid = 999999) =>
            SelfUpdaterScriptGenerator.Generate(AppRoot, serverPid, Prepared, "1234abcd",
                os, GeneratedAt, options);

        public string Snapshot() => string.Join("\n", Directory.EnumerateFiles(AppRoot, "*",
                SearchOption.AllDirectories)
            .Where(path => !path.Contains(Path.DirectorySeparatorChar + ".amatsukaze_update" +
                Path.DirectorySeparatorChar, StringComparison.Ordinal))
            .OrderBy(path => path, StringComparer.Ordinal)
            .Select(path => Path.GetRelativePath(AppRoot, path).Replace('\\', '/') + "=" +
                Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(
                    File.ReadAllBytes(path)))));

        public bool Exists(string relative) => File.Exists(Path.Combine(AppRoot,
            relative.Replace('/', Path.DirectorySeparatorChar)));

        public string Read(string relative) => File.ReadAllText(Path.Combine(AppRoot,
            relative.Replace('/', Path.DirectorySeparatorChar)));

        private void Write(string relative, string content)
        {
            var path = Path.Combine(AppRoot, relative.Replace('/', Path.DirectorySeparatorChar));
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path, content);
        }

        private void WriteStaging(string relative, string content)
        {
            var path = Path.Combine(Staging, relative.Replace('/', Path.DirectorySeparatorChar));
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path, content);
        }

        public void Dispose()
        {
            if (Directory.Exists(Root)) Directory.Delete(Root, true);
        }
    }
}
