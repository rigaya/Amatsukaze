using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading.Channels;
using Amatsukaze.Server.Update;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class ReleaseClientTests
{
    private static Task<IPAddress[]> ResolveLoopbackAsync(string host, CancellationToken cancellationToken) =>
        Task.FromResult(new[] { IPAddress.Loopback });

    private static UpdateDiagnostics.NetworkProbes Probes(List<string>? queriedHosts = null) =>
        new(
            host =>
            {
                queriedHosts?.Add(host);
                return Task.FromResult("127.0.0.1");
            },
            _ => Task.FromResult("ok"),
            _ => Task.FromResult(("TLS", "test", "ok")));

    [Fact]
    public async Task T01_DNS解決失敗は指定ホストだけを診断する()
    {
        const string host = "amatsukaze-update.invalid";
        using var fixture = new ReleaseFixture();
        var diagnosedHosts = new List<string>();
        var resolvedHosts = new List<string>();
        using var http = UpdateHttpTestSupport.CreateClient(new UpdateResponseHandler(
            _ => throw new InvalidOperationException("名前解決に失敗したリクエストを送信しました")));
        using var client = new ReleaseClient(http, $"http://{host}/", UpdateHttpTestSupport.Unbounded,
            (name, _) =>
            {
                resolvedHosts.Add(name);
                return Task.FromException<IPAddress[]>(new SocketException((int)SocketError.HostNotFound));
            }, Probes(diagnosedHosts));

        var releases = await client.GetReleasesAsync("owner/repository", "test",
            ReleaseSelectMode.Latest, fixture.Log, CancellationToken.None);
        fixture.CloseLog();

        Assert.Null(releases);
        Assert.Equal(new[] { host, host, host }, resolvedHosts);
        Assert.Equal(new[] { host }, diagnosedHosts);
        Assert.Contains("[S03_CONNECT] NG code=DNS_FAILED", fixture.LogText);
        Assert.Contains("api_dns=", fixture.LogText);
        Assert.DoesNotContain("objects_dns=", fixture.LogText);
    }

    [Fact]
    public async Task T02_接続タイムアウトは三回試行して時間を記録する()
    {
        using var fixture = new ReleaseFixture();
        var clock = new FakeTimeProvider();
        var timeout = TimeSpan.FromSeconds(8);
        var timing = new UpdateHttpTiming(clock, timeout, TimeSpan.Zero);
        var requests = Channel.CreateUnbounded<CancellationToken>();
        using var http = UpdateHttpTestSupport.CreateClient(new UpdateResponseHandler(async cancellationToken =>
        {
            requests.Writer.TryWrite(cancellationToken);
            await UpdateHttpTestSupport.WaitForCancellationAsync(cancellationToken);
            throw new InvalidOperationException("期限切れの要求が応答しました");
        }));
        using var client = new ReleaseClient(http, "http://127.0.0.1/", timing, ResolveLoopbackAsync, Probes());
        using var cancellation = new CancellationTokenSource();
        var releases = client.GetReleasesAsync("owner/repository", "test",
            ReleaseSelectMode.Latest, fixture.Log, cancellation.Token);
        try
        {
            for (var attempt = 1; attempt <= 3; attempt++)
            {
                var request = requests.Reader.ReadAsync(cancellation.Token).AsTask();
                Assert.Same(request, await Task.WhenAny(request, releases));
                var token = await request;
                Assert.False(token.IsCancellationRequested);
                clock.Advance(timeout);
                Assert.True(token.IsCancellationRequested);
            }

            Assert.Null(await releases);
            Assert.False(requests.Reader.TryRead(out _));
        }
        finally
        {
            cancellation.Cancel();
        }

        fixture.CloseLog();
        Assert.Contains("[S03_CONNECT] NG code=CONNECT_TIMEOUT", fixture.LogText);
        Assert.Contains("timeout=8s", fixture.LogText);
        Assert.Contains("elapsed=8000ms", fixture.LogText);
        Assert.Contains("attempt=3/3", fixture.LogText);
    }

    [Fact]
    public async Task T03_DNSの期限切れで三回試行して通信前に失敗する()
    {
        using var fixture = new ReleaseFixture();
        var clock = new FakeTimeProvider();
        var timeout = TimeSpan.FromSeconds(8);
        var retryInterval = TimeSpan.FromMilliseconds(250);
        var timing = new UpdateHttpTiming(clock, timeout, retryInterval);
        var timers = clock.ObserveTimers();
        var lookups = Channel.CreateUnbounded<CancellationToken>();
        using var http = UpdateHttpTestSupport.CreateClient(new UpdateResponseHandler(
            _ => throw new InvalidOperationException("名前解決に失敗したリクエストを送信しました")));
        using var client = new ReleaseClient(http, "http://amatsukaze-update.invalid/", timing,
            async (_, cancellationToken) =>
            {
                lookups.Writer.TryWrite(cancellationToken);
                await UpdateHttpTestSupport.WaitForCancellationAsync(cancellationToken);
                throw new InvalidOperationException("期限切れの名前解決が応答しました");
            }, Probes());
        using var cancellation = new CancellationTokenSource();
        var releases = client.GetReleasesAsync("owner/repository", "test",
            ReleaseSelectMode.Latest, fixture.Log, cancellation.Token);
        try
        {
            for (var attempt = 1; attempt <= 3; attempt++)
            {
                var lookup = lookups.Reader.ReadAsync(cancellation.Token).AsTask();
                Assert.Same(lookup, await Task.WhenAny(lookup, releases));
                var token = await lookup;
                Assert.False(token.IsCancellationRequested);
                Assert.Equal(timeout, await timers.ReadAsync(cancellation.Token));
                clock.Advance(timeout);
                Assert.True(token.IsCancellationRequested);
                if (attempt < 3)
                {
                    var delay = retryInterval * attempt;
                    Assert.Equal(delay, await timers.ReadAsync(cancellation.Token));
                    clock.Advance(delay);
                }
            }

            Assert.Null(await releases);
            Assert.False(lookups.Reader.TryRead(out _));
        }
        finally
        {
            cancellation.Cancel();
        }

        fixture.CloseLog();
        Assert.Contains("[S03_CONNECT] NG code=DNS_FAILED", fixture.LogText);
        Assert.Contains("attempt=3/3", fixture.LogText);
    }

    [Fact]
    public async Task T04_レートリミット403は再試行せずヘッダを記録する()
    {
        using var fixture = new ReleaseFixture();
        await using var server = new LoopbackUpdateHttpServer(
            Encoding.UTF8.GetBytes("{\"message\":\"rate limit\"}"), statusCode: 403,
            extraHeaders: "X-RateLimit-Remaining: 0\r\nX-RateLimit-Reset: 1786406400\r\n");
        using var http = UpdateHttpTestSupport.CreateClient();
        using var client = new ReleaseClient(http, server.Url, UpdateHttpTestSupport.Unbounded,
            ResolveLoopbackAsync, Probes());

        var releases = await client.GetReleasesAsync("owner/repository", "test",
            ReleaseSelectMode.Latest, fixture.Log, CancellationToken.None);
        fixture.CloseLog();

        Assert.Null(releases);
        Assert.Equal(1, server.RequestCount);
        Assert.Contains("status=403", fixture.LogText);
        Assert.Contains("ratelimit=0", fixture.LogText);
        Assert.Contains("x_ratelimit_remaining=0", fixture.LogText);
        Assert.Contains("x_ratelimit_reset=1786406400", fixture.LogText);
    }

    private sealed class ReleaseFixture : IDisposable
    {
        private readonly string root = Path.Combine(Path.GetTempPath(),
            "amatsukaze-release-test-" + Guid.NewGuid());
        private bool closed;

        public ReleaseFixture()
        {
            Directory.CreateDirectory(root);
            Log = new UpdateLog(root);
        }

        public UpdateLog Log { get; }
        public string LogText { get; private set; } = string.Empty;

        public void CloseLog()
        {
            if (closed) return;
            closed = true;
            Log.Dispose();
            LogText = File.ReadAllText(Log.FilePath);
        }

        public void Dispose()
        {
            CloseLog();
            Directory.Delete(root, true);
        }
    }

}
