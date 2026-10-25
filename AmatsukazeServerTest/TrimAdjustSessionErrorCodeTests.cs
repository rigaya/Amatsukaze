using System.Net;
using System.Text;
using System.Text.Json;
using Amatsukaze.Server;
using Amatsukaze.Shared;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class TrimAdjustSessionErrorCodeTests
{
    [Fact]
    public async Task キャッシュがない場合は専用コードと既存メッセージを返す()
    {
        await using var f = new RestEndpointTestFixture();
        await f.Queue.Seed(RestQueueTestFixture.Item(1, QueueState.Complete));

        var response = await f.Send("POST", "/api/trim/sessions", body: "{\"queueItemId\":1}");

        Assert.Equal((int)HttpStatusCode.BadRequest, response.Status);
        using var json = JsonDocument.Parse(response.Body);
        Assert.Equal(RestApiErrorCodes.TrimCacheUnavailable, json.RootElement.GetProperty("code").GetString());
        Assert.Contains("amts0.dat", json.RootElement.GetProperty("message").GetString());
    }

    [Fact]
    public async Task キューID不正の400にはキャッシュ復元用コードを付けない()
    {
        await using var f = new RestEndpointTestFixture();

        var response = await f.Send("POST", "/api/trim/sessions", body: "{\"queueItemId\":999}");

        Assert.Equal((int)HttpStatusCode.BadRequest, response.Status);
        using var json = JsonDocument.Parse(response.Body);
        Assert.False(json.RootElement.TryGetProperty("code", out _));
    }

    [Fact]
    public async Task APIクライアントはエラーコードとメッセージを保持する()
    {
        using var client = new HttpClient(new StaticResponseHandler()) { BaseAddress = new Uri("http://localhost") };
        var api = new AmatsukazeApi(client);

        var result = await api.CreateTrimSessionAsync(new TrimAdjustSessionRequest { QueueItemId = 1 });

        Assert.False(result.Ok);
        Assert.Equal((int)HttpStatusCode.BadRequest, result.StatusCode);
        Assert.Equal(RestApiErrorCodes.TrimCacheUnavailable, result.ErrorCode);
        Assert.Contains("amts0.dat", result.Error);
    }

    private sealed class StaticResponseHandler : HttpMessageHandler
    {
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
        {
            Assert.Equal(HttpMethod.Post, request.Method);
            Assert.Equal("/api/trim/sessions", request.RequestUri?.AbsolutePath);
            var body = "{\"code\":\"trim_cache_unavailable\",\"message\":\"amts0.datが見つかりません\"}";
            return Task.FromResult(new HttpResponseMessage(HttpStatusCode.BadRequest)
            {
                Content = new StringContent(body, Encoding.UTF8, "application/json"),
            });
        }
    }
}
