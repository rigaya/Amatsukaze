using System.Diagnostics;
using System.Security.Cryptography;
using Amatsukaze.Server.Update;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class UpdateTransactionTests
{
    [LinuxFact]
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
            using var cancellation = new CancellationTokenSource();
            var ready = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            var draining = new TaskCompletionSource<Task>(TaskCreationOptions.RunContinuationsAsynchronously);
            var extractor = new ArchiveExtractor(fakeExtractor, limits: null, start =>
            {
                var extracting = start.ArgumentList.Count != 0 && start.ArgumentList[0] == "x";
                start.RedirectStandardInput = extracting;
                var process = Process.Start(start)!;
                if (extracting)
                {
                    try
                    {
                        // 子が出力端を継承し、入力待ちに入ったことを確かめる。
                        Assert.Equal("ready", process.StandardError.ReadLine());
                        ready.SetResult();
                    }
                    catch
                    {
                        process.StandardInput.Close();
                        process.WaitForExit();
                        process.Dispose();
                        throw;
                    }
                }
                return process;
            }, (process, drain) =>
            {
                if (process.StartInfo.RedirectStandardInput) draining.SetResult(drain);
            });

            var extraction = extractor.ExtractAsync(result,
                transaction.GetTargetExtractDirectory("test"), "test", log, cancellation.Token);
            var readyToCancel = Task.WhenAll(ready.Task, draining.Task);
            if (await Task.WhenAny(readyToCancel, extraction) == extraction)
            {
                await extraction;
                Assert.Fail("子プロセスの起動通知前に展開処理が終了した");
            }

            await readyToCancel;
            var drain = await draining.Task;
            await cancellation.CancelAsync();
            await Assert.ThrowsAnyAsync<OperationCanceledException>(() => extraction);
            // 展開の取消が返った時点で、子が継承した出力端までEOFになっている。
            Assert.True(drain.IsCompletedSuccessfully);
        }

        Assert.False(Directory.Exists(rootDir));
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
                "  exit 0\nfi\nexec 3<&0\n" +
                "(printf 'ready\\n' >&2; IFS= read -r line <&3) &\n" +
                "exec 1>&- 2>&-\nwait\n");
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
