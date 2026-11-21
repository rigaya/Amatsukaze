using System.Net;
using System.Net.Sockets;
using System.Text;
using Amatsukaze.Server.Update;

namespace AmatsukazeServerTest;

internal sealed class LoopbackUpdateHttpServer : IAsyncDisposable
{
    private readonly TcpListener listener = new(IPAddress.Loopback, 0);
    private readonly CancellationTokenSource shutdown = new();
    private readonly Task loop;
    private int requestCount;

    public LoopbackUpdateHttpServer(byte[] content, int statusCode = 200, int? contentLength = null,
        string extraHeaders = "")
    {
        listener.Start();
        Url = $"http://127.0.0.1:{((IPEndPoint)listener.LocalEndpoint).Port}/";
        var headers = Encoding.ASCII.GetBytes(
            $"HTTP/1.1 {statusCode} Response\r\nContent-Length: {contentLength ?? content.Length}\r\n" +
            $"Connection: close\r\n{extraHeaders}\r\n");
        loop = ServeAsync(headers, content);
    }

    public string Url { get; }
    public int RequestCount => Volatile.Read(ref requestCount);

    private async Task ServeAsync(byte[] headers, byte[] content)
    {
        try
        {
            while (!shutdown.IsCancellationRequested)
            {
                using var connection = await listener.AcceptTcpClientAsync(shutdown.Token);
                using var stream = connection.GetStream();
                using var request = new StreamReader(stream, Encoding.ASCII, leaveOpen: true);
                while (await request.ReadLineAsync(shutdown.Token) is { Length: > 0 })
                {
                }
                Interlocked.Increment(ref requestCount);
                await stream.WriteAsync(headers, shutdown.Token);
                await stream.WriteAsync(content, shutdown.Token);
            }
        }
        catch (OperationCanceledException) when (shutdown.IsCancellationRequested)
        {
        }
        catch (SocketException) when (shutdown.IsCancellationRequested)
        {
        }
    }

    public async ValueTask DisposeAsync()
    {
        shutdown.Cancel();
        try
        {
            await loop;
        }
        finally
        {
            listener.Stop();
            shutdown.Dispose();
        }
    }
}

internal sealed class UpdateResponseHandler(Func<CancellationToken, Task<HttpResponseMessage>> respond)
    : HttpMessageHandler
{
    protected override Task<HttpResponseMessage> SendAsync(
        HttpRequestMessage request, CancellationToken cancellationToken) => respond(cancellationToken);
}

internal static class UpdateHttpTestSupport
{
    public static UpdateHttpTiming Unbounded { get; } =
        new(TimeProvider.System, Timeout.InfiniteTimeSpan, TimeSpan.Zero);

    public static UpdateDiagnostics.NetworkProbes UnboundedNetworkProbes { get; } =
        UpdateDiagnostics.CreateNetworkProbes(Timeout.InfiniteTimeSpan, Timeout.InfiniteTimeSpan);

    public static HttpClient CreateClient(HttpMessageHandler? handler = null) =>
        new(handler ?? new HttpClientHandler { UseProxy = false })
        {
            Timeout = Timeout.InfiniteTimeSpan,
        };

    public static async Task WaitForCancellationAsync(CancellationToken cancellationToken)
    {
        var completion = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        using var registration = cancellationToken.Register(() => completion.TrySetCanceled(cancellationToken));
        await completion.Task;
    }
}
