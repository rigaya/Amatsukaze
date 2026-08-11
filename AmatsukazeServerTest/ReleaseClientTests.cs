using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;
using System.Text;
using Amatsukaze.Server.Update;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class ReleaseClientTests
{
    private static readonly TimeSpan ShortTimeout = TimeSpan.FromMilliseconds(300);

    [Fact]
    public async Task T01_DNS解決失敗は指定ホストだけを診断する()
    {
        using var fixture = new ReleaseFixture();
        using var client = new ReleaseClient(string.Empty,
            "http://amatsukaze-update.invalid/", ShortTimeout);

        var releases = await client.GetReleasesAsync("owner/repository", "test",
            ReleaseSelectMode.Latest, fixture.Log, CancellationToken.None);
        fixture.CloseLog();

        Assert.Null(releases);
        Assert.Contains("[S03_CONNECT] NG code=DNS_FAILED", fixture.LogText);
        Assert.Contains("api_dns=", fixture.LogText);
        Assert.DoesNotContain("objects_dns=", fixture.LogText);
    }

    [Fact]
    public async Task T02_接続タイムアウトは三回試行して時間を記録する()
    {
        using var fixture = new ReleaseFixture();
        await using var server = await MockServer.StartAsync(async (_, _, cancellationToken) =>
            await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken));
        using var client = new ReleaseClient(string.Empty, server.Url, ShortTimeout);

        var releases = await client.GetReleasesAsync("owner/repository", "test",
            ReleaseSelectMode.Latest, fixture.Log, CancellationToken.None);
        fixture.CloseLog();

        Assert.Null(releases);
        Assert.Equal(3, server.RequestCount);
        Assert.Contains("[S03_CONNECT] NG code=CONNECT_TIMEOUT", fixture.LogText);
        Assert.Contains("timeout=", fixture.LogText);
        Assert.Contains("attempt=3/3", fixture.LogText);
    }

    [Fact]
    public async Task T04_レートリミット403は再試行せずヘッダを記録する()
    {
        using var fixture = new ReleaseFixture();
        await using var server = await MockServer.StartAsync((_, response, _) =>
        {
            response.StatusCode = (int)HttpStatusCode.Forbidden;
            response.Headers["X-RateLimit-Remaining"] = "0";
            response.Headers["X-RateLimit-Reset"] = "1786406400";
            return WriteResponseAsync(response, "{\"message\":\"rate limit\"}");
        });
        using var client = new ReleaseClient(string.Empty, server.Url, ShortTimeout);

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

    private static async Task WriteResponseAsync(HttpListenerResponse response, string body)
    {
        var bytes = Encoding.UTF8.GetBytes(body);
        response.ContentLength64 = bytes.Length;
        await response.OutputStream.WriteAsync(bytes);
        response.Close();
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

    private sealed class MockServer : IAsyncDisposable
    {
        private readonly HttpListener listener;
        private readonly Func<HttpListenerRequest, HttpListenerResponse, CancellationToken, Task> handler;
        private readonly CancellationTokenSource cancellation = new();
        private readonly ConcurrentBag<Task> handlers = new();
        private readonly Task loop;
        private int requestCount;

        private MockServer(HttpListener listener, string url,
            Func<HttpListenerRequest, HttpListenerResponse, CancellationToken, Task> handler)
        {
            this.listener = listener;
            this.handler = handler;
            Url = url;
            loop = Task.Run(RunAsync);
        }

        public string Url { get; }
        public int RequestCount => Volatile.Read(ref requestCount);

        public static Task<MockServer> StartAsync(
            Func<HttpListenerRequest, HttpListenerResponse, CancellationToken, Task> handler)
        {
            using var reservation = new TcpListener(IPAddress.Loopback, 0);
            reservation.Start();
            var port = ((IPEndPoint)reservation.LocalEndpoint).Port;
            reservation.Stop();
            var url = $"http://127.0.0.1:{port}/";
            var listener = new HttpListener();
            listener.Prefixes.Add(url);
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
                    handlers.Add(Task.Run(async () =>
                    {
                        try
                        {
                            await handler(context.Request, context.Response, cancellation.Token);
                        }
                        catch when (cancellation.IsCancellationRequested)
                        {
                        }
                    }));
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
            try { await Task.WhenAll(handlers.ToArray()); } catch { }
            cancellation.Dispose();
        }
    }
}
