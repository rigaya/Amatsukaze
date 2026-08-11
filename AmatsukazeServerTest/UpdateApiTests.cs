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
}
