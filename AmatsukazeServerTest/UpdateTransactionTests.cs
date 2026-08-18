using System.Diagnostics;
using System.Security.Cryptography;
using Amatsukaze.Server.Update;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class UpdateTransactionTests
{
    [Fact]
    public async Task 展開キャンセルでトランザクションとプロセスツリーを残さない()
    {
        using var fixture = new TransactionFixture();
        var fakeExtractor = fixture.CreateHangingExtractor();
        string rootDir;
        using (var transaction = UpdateTransaction.Create(fixture.AppRoot, "abcdef12"))
        {
            rootDir = transaction.RootDir;
            var archive = Path.Combine(transaction.GetTargetDownloadDirectory("test"), "test.zip");
            await File.WriteAllBytesAsync(archive, [0x50, 0x4b, 0x03, 0x04, 1, 2]);
            var result = new DownloadResult(archive, new FileInfo(archive).Length, "-",
                TimeSpan.Zero, "zip");
            using var log = new UpdateLog(fixture.AppRoot);
            using var cancellation = new CancellationTokenSource(TimeSpan.FromMilliseconds(500));
            await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
                new ArchiveExtractor(fakeExtractor).ExtractAsync(result,
                    transaction.GetTargetExtractDirectory("test"), "test", log,
                    cancellation.Token));
        }

        Assert.False(Directory.Exists(rootDir));
        await Task.Delay(200);
        Assert.DoesNotContain(EnumerateCommandLines(), command => command.Contains(fakeExtractor,
            StringComparison.Ordinal));
        fixture.AssertExeFilesUnchanged();
    }

    [Fact]
    public void 起動時の残骸削除はUpdateTmpの外を変更しない()
    {
        using var fixture = new TransactionFixture();
        var stale = Path.Combine(fixture.ExeFiles, ".update_tmp", "deadbeef");
        Directory.CreateDirectory(stale);
        File.WriteAllText(Path.Combine(stale, "partial"), "途中");

        UpdateTransaction.CleanupStale(fixture.AppRoot);

        Assert.False(Directory.Exists(stale));
        fixture.AssertExeFilesUnchanged();
    }

    [Fact]
    public void UpdateTmpがリンクなら境界外へ書かず拒否する()
    {
        if (OperatingSystem.IsWindows()) return;
        using var fixture = new TransactionFixture();
        var external = Path.Combine(fixture.AppRoot, "external");
        Directory.CreateDirectory(external);
        var sentinel = Path.Combine(external, "sentinel");
        File.WriteAllText(sentinel, "変更禁止");
        var temporaryRoot = Path.Combine(fixture.ExeFiles, ".update_tmp");
        Directory.CreateSymbolicLink(temporaryRoot, external);
        try
        {
            var exception = Assert.Throws<UpdatePreparationException>(() =>
                UpdateTransaction.Create(fixture.AppRoot, "2468abcd"));
            Assert.Equal("INVALID_STAGING_PATH", exception.Code);
            Assert.Equal("変更禁止", File.ReadAllText(sentinel));
            Assert.Equal(new[] { sentinel }, Directory.EnumerateFileSystemEntries(external)
                .OrderBy(path => path, StringComparer.Ordinal));
        }
        finally
        {
            Directory.Delete(temporaryRoot);
        }
        fixture.AssertExeFilesUnchanged();
    }

    private static IEnumerable<string> EnumerateCommandLines()
    {
        if (!Directory.Exists("/proc")) yield break;
        foreach (var directory in Directory.EnumerateDirectories("/proc"))
        {
            var file = Path.Combine(directory, "cmdline");
            string command;
            try { command = File.ReadAllText(file).Replace('\0', ' '); }
            catch { continue; }
            yield return command;
        }
    }

    private sealed class TransactionFixture : IDisposable
    {
        private readonly IReadOnlyDictionary<string, string> before;

        public TransactionFixture()
        {
            AppRoot = Path.Combine(Path.GetTempPath(), "amatsukaze-transaction-test-" + Guid.NewGuid());
            ExeFiles = Path.Combine(AppRoot, "exe_files");
            Directory.CreateDirectory(Path.Combine(ExeFiles, "existing"));
            File.WriteAllText(Path.Combine(ExeFiles, "existing", "encoder"), "変更禁止");
            before = Snapshot();
        }

        public string AppRoot { get; }
        public string ExeFiles { get; }

        public string CreateHangingExtractor()
        {
            if (OperatingSystem.IsWindows())
            {
                throw new PlatformNotSupportedException("このテストは Linux 用です");
            }
            var path = Path.Combine(AppRoot, "fake-7z.sh");
            File.WriteAllText(path, "#!/bin/sh\nif [ $# -eq 0 ]; then\n" +
                "  echo '7-Zip 26.02'\n  exit 0\nfi\nif [ \"$1\" = \"l\" ]; then\n" +
                "  printf '%s\\n' '----------' 'Path = tool' 'Size = 1' ''\n" +
                "  exit 0\nfi\nsleep 60\n");
            File.SetUnixFileMode(path, File.GetUnixFileMode(path) | UnixFileMode.UserExecute);
            return path;
        }

        public void AssertExeFilesUnchanged() => Assert.Equal(before, Snapshot());

        private IReadOnlyDictionary<string, string> Snapshot()
        {
            return Directory.EnumerateFiles(ExeFiles, "*", SearchOption.AllDirectories)
                .Where(path => !Path.GetRelativePath(ExeFiles, path).Replace('\\', '/')
                    .StartsWith(".update_tmp/", StringComparison.Ordinal))
                .ToDictionary(path => Path.GetRelativePath(ExeFiles, path), path =>
                {
                    var bytes = File.ReadAllBytes(path);
                    return bytes.Length + ":" + Convert.ToHexString(SHA256.HashData(bytes));
                }, StringComparer.Ordinal);
        }

        public void Dispose()
        {
            AssertExeFilesUnchanged();
            Directory.Delete(AppRoot, true);
        }
    }
}
