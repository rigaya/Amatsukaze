using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using Amatsukaze.Server.Update;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class UpdateDownloaderTests
{
    private static readonly byte[] ZipLikeData =
        [0x50, 0x4b, 0x03, 0x04, 1, 2, 3, 4, 5, 6];
    private static readonly byte[] DebLikeData =
        [0x21, 0x3c, 0x61, 0x72, 0x63, 0x68, 0x3e, 0x0a];
    private static readonly byte[] SevenZipLikeData =
        [0x37, 0x7a, 0xbc, 0xaf, 0x27, 0x1c, 0x00, 0x04];

    [Fact]
    public async Task 正常ダウンロードでサイズとハッシュが一致する()
    {
        using var fixture = new DownloadFixture();
        await using var server = await MockServer.StartAsync((_, response) =>
            WriteResponseAsync(response, ZipLikeData));
        var asset = Asset(server.Url, ZipLikeData);

        var result = await fixture.Downloader.DownloadAsync(asset, fixture.DownloadDirectory,
            "test", fixture.Log, null, CancellationToken.None);

        Assert.Equal(ZipLikeData.Length, result.Bytes);
        Assert.Equal(Sha256(ZipLikeData), result.Sha256);
        Assert.Equal(ZipLikeData, await File.ReadAllBytesAsync(result.FilePath));
    }

    [Fact]
    public async Task 接続中断は三回再試行して部分ファイルを残さない()
    {
        using var fixture = new DownloadFixture();
        await using var server = await MockServer.StartAsync(async (_, response) =>
        {
            response.ContentLength64 = 1024;
            await response.OutputStream.WriteAsync(ZipLikeData.AsMemory(0, 5));
            response.Abort();
        });
        var asset = Asset(server.Url, new byte[1024]);

        var exception = await Assert.ThrowsAsync<UpdatePreparationException>(() =>
            fixture.Downloader.DownloadAsync(asset, fixture.DownloadDirectory, "test",
                fixture.Log, null, CancellationToken.None));

        Assert.Equal("DOWNLOAD_FAILED", exception.Code);
        Assert.Equal(3, server.RequestCount);
        Assert.Empty(Directory.EnumerateFiles(fixture.DownloadDirectory, "*.part"));
    }

    [Fact]
    public async Task サイズ不一致は検証失敗になる()
    {
        using var fixture = new DownloadFixture();
        await using var server = await MockServer.StartAsync((_, response) =>
            WriteResponseAsync(response, ZipLikeData));
        var original = Asset(server.Url, ZipLikeData);
        var asset = WithSize(original, ZipLikeData.Length + 1);

        var exception = await Assert.ThrowsAsync<UpdatePreparationException>(() =>
            fixture.Downloader.DownloadAsync(asset, fixture.DownloadDirectory, "test",
                fixture.Log, null, CancellationToken.None));

        Assert.Equal("VERIFY_FAILED", exception.Code);
    }

    [Fact]
    public async Task Digest不一致は検証失敗になる()
    {
        using var fixture = new DownloadFixture();
        await using var server = await MockServer.StartAsync((_, response) =>
            WriteResponseAsync(response, ZipLikeData));
        var asset = new ReleaseAssetInfo
        {
            Name = "test.zip", BrowserDownloadUrl = server.Url,
            Size = ZipLikeData.Length, Digest = "sha256:" + new string('0', 64),
        };

        var exception = await Assert.ThrowsAsync<UpdatePreparationException>(() =>
            fixture.Downloader.DownloadAsync(asset, fixture.DownloadDirectory, "test",
                fixture.Log, null, CancellationToken.None));
        Assert.Equal("VERIFY_FAILED", exception.Code);
    }

    [Fact]
    public async Task Digestなしは続行しログへ記録する()
    {
        using var fixture = new DownloadFixture();
        await using var server = await MockServer.StartAsync((_, response) =>
            WriteResponseAsync(response, ZipLikeData));
        var asset = new ReleaseAssetInfo
        {
            Name = "test.zip", BrowserDownloadUrl = server.Url,
            Size = ZipLikeData.Length, Digest = null,
        };

        _ = await fixture.Downloader.DownloadAsync(asset, fixture.DownloadDirectory, "test",
            fixture.Log, null, CancellationToken.None);
        fixture.Log.Dispose();

        Assert.Contains("match=unavailable", await File.ReadAllTextAsync(fixture.Log.FilePath));
    }

    [Fact]
    public async Task 拡張子とマジック不一致は検証失敗になる()
    {
        using var fixture = new DownloadFixture();
        var invalid = new byte[] { 1, 2, 3, 4, 5, 6 };
        await using var server = await MockServer.StartAsync((_, response) =>
            WriteResponseAsync(response, invalid));
        var asset = Asset(server.Url, invalid);

        var exception = await Assert.ThrowsAsync<UpdatePreparationException>(() =>
            fixture.Downloader.DownloadAsync(asset, fixture.DownloadDirectory, "test",
                fixture.Log, null, CancellationToken.None));
        Assert.Equal("VERIFY_FAILED", exception.Code);
    }

    [Fact]
    public async Task Debは拡張子とArマジックが一致すれば受け入れる()
    {
        using var fixture = new DownloadFixture();
        await using var server = await MockServer.StartAsync((_, response) =>
            WriteResponseAsync(response, DebLikeData));
        var asset = Asset(server.Url, DebLikeData, "test.deb");

        var result = await fixture.Downloader.DownloadAsync(asset, fixture.DownloadDirectory,
            "test", fixture.Log, null, CancellationToken.None);

        Assert.Equal("deb", result.Format);
    }

    [Fact]
    public async Task Deb拡張子でもArマジックでなければ検証失敗になる()
    {
        using var fixture = new DownloadFixture();
        await using var server = await MockServer.StartAsync((_, response) =>
            WriteResponseAsync(response, ZipLikeData));
        var asset = Asset(server.Url, ZipLikeData, "test.deb");

        var exception = await Assert.ThrowsAsync<UpdatePreparationException>(() =>
            fixture.Downloader.DownloadAsync(asset, fixture.DownloadDirectory, "test",
                fixture.Log, null, CancellationToken.None));

        Assert.Equal("VERIFY_FAILED", exception.Code);
    }

    [Fact]
    public async Task SevenZipは拡張子とマジックが一致すれば受け入れる()
    {
        using var fixture = new DownloadFixture();
        await using var server = await MockServer.StartAsync((_, response) =>
            WriteResponseAsync(response, SevenZipLikeData));
        var asset = Asset(server.Url, SevenZipLikeData, "test.7z");

        var result = await fixture.Downloader.DownloadAsync(asset, fixture.DownloadDirectory,
            "test", fixture.Log, null, CancellationToken.None);

        Assert.Equal("7z", result.Format);
    }

    [Fact]
    public async Task SevenZip拡張子でもマジックが違えば検証失敗になる()
    {
        using var fixture = new DownloadFixture();
        await using var server = await MockServer.StartAsync((_, response) =>
            WriteResponseAsync(response, ZipLikeData));
        var asset = Asset(server.Url, ZipLikeData, "test.7z");

        var exception = await Assert.ThrowsAsync<UpdatePreparationException>(() =>
            fixture.Downloader.DownloadAsync(asset, fixture.DownloadDirectory, "test",
                fixture.Log, null, CancellationToken.None));

        Assert.Equal("VERIFY_FAILED", exception.Code);
    }

    [Fact]
    public async Task ダウンロード中のキャンセルでトランザクションを残さない()
    {
        using var fixture = new DownloadFixture();
        await using var server = await MockServer.StartAsync(async (_, response) =>
        {
            response.ContentLength64 = 100_000;
            try
            {
                for (var index = 0; index < 1000; index++)
                {
                    await response.OutputStream.WriteAsync(ZipLikeData);
                    await response.OutputStream.FlushAsync();
                    await Task.Delay(20);
                }
            }
            catch
            {
                // クライアントのキャンセルによる切断を受け入れる。
            }
        });
        string rootDir;
        using (var transaction = UpdateTransaction.Create(fixture.Root, "1357abcd"))
        {
            rootDir = transaction.RootDir;
            var asset = new ReleaseAssetInfo
            {
                Name = "test.zip", BrowserDownloadUrl = server.Url,
                Size = 100_000, Digest = null,
            };
            using var cancellation = new CancellationTokenSource(TimeSpan.FromMilliseconds(150));
            await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
                fixture.Downloader.DownloadAsync(asset,
                    transaction.GetTargetDownloadDirectory("test"), "test", fixture.Log,
                    null, cancellation.Token));
        }
        Assert.False(Directory.Exists(rootDir));
    }

    [Fact]
    public async Task 必要容量を確保できないサイズは通信前に拒否する()
    {
        using var fixture = new DownloadFixture();
        var asset = new ReleaseAssetInfo
        {
            Name = "test.zip", BrowserDownloadUrl = "http://127.0.0.1:1/unused",
            Size = long.MaxValue, Digest = null,
        };

        var exception = await Assert.ThrowsAsync<UpdatePreparationException>(() =>
            fixture.Downloader.DownloadAsync(asset, fixture.DownloadDirectory, "test",
                fixture.Log, null, CancellationToken.None));
        Assert.Equal("NO_SPACE", exception.Code);
    }

    [Fact]
    public void 空き容量確認は対象を含む最長マウントポイントを選ぶ()
    {
        using var fixture = new DownloadFixture();
        var targetPath = Environment.GetEnvironmentVariable("AMT_DRIVE_TEST_PATH") ??
            fixture.DownloadDirectory;
        var selected = UpdateDownloader.SelectDriveForPath(targetPath);
        var drives = DriveInfo.GetDrives().Where(drive =>
        {
            try { return drive.IsReady; } catch { return false; }
        }).Select(drive => drive.RootDirectory.FullName).ToArray();

        Console.WriteLine("target=" + Path.GetFullPath(targetPath));
        Console.WriteLine("drives=" + string.Join(",", drives));
        Console.WriteLine("selected=" + selected.RootDirectory.FullName);
        Assert.True(selected.IsReady);
        var comparison = OperatingSystem.IsWindows()
            ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
        var fullTarget = Path.GetFullPath(targetPath);
        var expected = drives.Where(root =>
        {
            var normalized = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar,
                Path.AltDirectorySeparatorChar);
            var prefix = normalized.Length == 0 ? Path.DirectorySeparatorChar.ToString() :
                normalized + Path.DirectorySeparatorChar;
            return string.Equals(fullTarget, normalized, comparison) ||
                fullTarget.StartsWith(prefix, comparison);
        }).OrderByDescending(root => Path.GetFullPath(root).Length).First();
        Assert.True(string.Equals(Path.GetFullPath(expected),
            Path.GetFullPath(selected.RootDirectory.FullName), comparison));
        Assert.StartsWith(Path.GetFullPath(selected.RootDirectory.FullName),
            Path.GetFullPath(targetPath),
            comparison);
    }

    private static ReleaseAssetInfo Asset(string url, byte[] content,
        string name = "test.zip") => new ReleaseAssetInfo
    {
        Name = name, BrowserDownloadUrl = url, Size = content.Length,
        Digest = "sha256:" + Sha256(content),
    };

    private static ReleaseAssetInfo WithSize(ReleaseAssetInfo source, long size) =>
        new ReleaseAssetInfo
        {
            Name = source.Name, BrowserDownloadUrl = source.BrowserDownloadUrl,
            Size = size, Digest = source.Digest,
        };

    private static string Sha256(byte[] content) =>
        Convert.ToHexString(SHA256.HashData(content)).ToLowerInvariant();

    private static async Task WriteResponseAsync(HttpListenerResponse response, byte[] content)
    {
        response.ContentLength64 = content.Length;
        await response.OutputStream.WriteAsync(content);
        response.Close();
    }

    private sealed class DownloadFixture : IDisposable
    {
        private readonly UpdateTransaction transaction;
        private readonly string[] before;

        public DownloadFixture()
        {
            Root = Path.Combine(Path.GetTempPath(), "amatsukaze-update-test-" + Guid.NewGuid());
            var exeFiles = Path.Combine(Root, "exe_files");
            Directory.CreateDirectory(Path.Combine(exeFiles, "existing"));
            File.WriteAllText(Path.Combine(exeFiles, "existing", "encoder"), "変更禁止");
            before = Snapshot(exeFiles);
            transaction = UpdateTransaction.Create(Root, "1122aabb");
            DownloadDirectory = transaction.GetTargetDownloadDirectory("test");
            Log = new UpdateLog(Root);
            Downloader = new UpdateDownloader(string.Empty);
        }

        public string Root { get; }
        public string DownloadDirectory { get; }
        public UpdateLog Log { get; }
        public UpdateDownloader Downloader { get; }

        public void Dispose()
        {
            Downloader.Dispose();
            Log.Dispose();
            transaction.Dispose();
            Assert.Equal(before, Snapshot(Path.Combine(Root, "exe_files")));
            if (Directory.Exists(Root)) Directory.Delete(Root, true);
        }

        private static string[] Snapshot(string root) => Directory.EnumerateFiles(root, "*",
                SearchOption.AllDirectories)
            .Where(path => !Path.GetRelativePath(root, path).Replace('\\', '/')
                .StartsWith(".update_tmp/", StringComparison.Ordinal))
            .Select(path => Path.GetRelativePath(root, path) + ":" +
                Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path))))
            .OrderBy(value => value, StringComparer.Ordinal).ToArray();
    }

    private sealed class MockServer : IAsyncDisposable
    {
        private readonly HttpListener listener;
        private readonly Func<HttpListenerRequest, HttpListenerResponse, Task> handler;
        private readonly CancellationTokenSource cancellation = new();
        private readonly Task loop;
        private int requestCount;

        private MockServer(HttpListener listener, string url,
            Func<HttpListenerRequest, HttpListenerResponse, Task> handler)
        {
            this.listener = listener;
            this.handler = handler;
            Url = url;
            loop = Task.Run(RunAsync);
        }

        public string Url { get; }
        public int RequestCount => Volatile.Read(ref requestCount);

        public static Task<MockServer> StartAsync(
            Func<HttpListenerRequest, HttpListenerResponse, Task> handler)
        {
            using var reservation = new TcpListener(IPAddress.Loopback, 0);
            reservation.Start();
            var port = ((IPEndPoint)reservation.LocalEndpoint).Port;
            reservation.Stop();
            var url = $"http://127.0.0.1:{port}/asset";
            var listener = new HttpListener();
            listener.Prefixes.Add($"http://127.0.0.1:{port}/");
            listener.Start();
            return Task.FromResult(new MockServer(listener, url, handler));
        }

        private async Task RunAsync()
        {
            while (!cancellation.IsCancellationRequested)
            {
                try
                {
                    var context = await listener.GetContextAsync();
                    Interlocked.Increment(ref requestCount);
                    await handler(context.Request, context.Response);
                }
                catch when (cancellation.IsCancellationRequested || !listener.IsListening)
                {
                    return;
                }
            }
        }

        public async ValueTask DisposeAsync()
        {
            cancellation.Cancel();
            listener.Close();
            try { await loop; } catch { }
            cancellation.Dispose();
        }
    }
}
