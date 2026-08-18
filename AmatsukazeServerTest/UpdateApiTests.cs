using System.Net;
using System.Text;
using Amatsukaze.Shared;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class UpdateApiTests
{
    [Fact]
    public async Task 更新適用エラーはJsonから表示文言を取り出す()
    {
        using var client = new HttpClient(new StaticResponseHandler(HttpStatusCode.Conflict,
            "{\"error\":\"別の更新適用が実行中です\"}"))
        {
            BaseAddress = new Uri("http://localhost"),
        };
        var api = new AmatsukazeApi(client);

        var result = await api.ApplyUpdatesAsync(new[] { "x264" });

        Assert.False(result.Ok);
        Assert.Equal(409, result.StatusCode);
        Assert.Equal("別の更新適用が実行中です", result.Error);
    }

    [Fact]
    public async Task 更新中止エラーがJsonでなければ本文を維持する()
    {
        const string body = "proxy error";
        using var client = new HttpClient(new StaticResponseHandler(HttpStatusCode.BadGateway,
            body))
        {
            BaseAddress = new Uri("http://localhost"),
        };
        var api = new AmatsukazeApi(client);

        var result = await api.CancelUpdateAsync("job");

        Assert.False(result.Ok);
        Assert.Equal(502, result.StatusCode);
        Assert.Equal(body, result.Error);
    }

    [Fact]
    public async Task 保留中の本体更新を専用エンドポイントで破棄する()
    {
        var handler = new CapturingResponseHandler();
        using var client = new HttpClient(handler) { BaseAddress = new Uri("http://localhost") };
        var api = new AmatsukazeApi(client);

        var result = await api.DiscardPendingSelfUpdateAsync();

        Assert.True(result.Ok);
        Assert.Equal("/api/update/discard-pending", handler.RequestPath);
    }

    private sealed class StaticResponseHandler(HttpStatusCode statusCode, string body)
        : HttpMessageHandler
    {
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request,
            CancellationToken cancellationToken)
        {
            return Task.FromResult(new HttpResponseMessage(statusCode)
            {
                Content = new StringContent(body, Encoding.UTF8, "application/json"),
            });
        }
    }

    private sealed class CapturingResponseHandler : HttpMessageHandler
    {
        public string RequestBody { get; private set; } = string.Empty;
        public string RequestPath { get; private set; } = string.Empty;

        protected override async Task<HttpResponseMessage> SendAsync(HttpRequestMessage request,
            CancellationToken cancellationToken)
        {
            RequestPath = request.RequestUri!.AbsolutePath;
            RequestBody = await request.Content!.ReadAsStringAsync(cancellationToken);
            return new HttpResponseMessage(HttpStatusCode.OK)
            {
                Content = new StringContent("{\"ok\":true}", Encoding.UTF8,
                    "application/json"),
            };
        }
    }
}
