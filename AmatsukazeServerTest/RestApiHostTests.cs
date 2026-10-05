using System.Reflection;
using System.Text;
using System.Text.Json;
using Amatsukaze.Server;
using Amatsukaze.Server.Rest;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Routing;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class RestApiHostTests
{
    private sealed class EndpointFixture : IAsyncDisposable
    {
        public RestQueueTestFixture Queue { get; } = new();
        private readonly WebApplication app;
        public EndpointFixture()
        {
            // 本番と同じJSON設定とMapEndpointsを使い、StartAsyncは呼ばずポートを開かない。
            app = (WebApplication)typeof(RestApiHost).GetMethod("BuildWebApp", BindingFlags.Instance | BindingFlags.NonPublic)!
                .Invoke(Queue.CreateRestHost(), new object[] { 0 })!;
        }

        public IReadOnlyList<RouteEndpoint> Endpoints => ((IEndpointRouteBuilder)app).DataSources
            .SelectMany(source => source.Endpoints).OfType<RouteEndpoint>().ToList();

        public async Task<(int Status, string Body)> Send(string method, string path, string query = "", string? body = null)
        {
            var endpoint = Assert.Single(Endpoints, e => e.RoutePattern.RawText == path &&
                e.Metadata.GetMetadata<HttpMethodMetadata>()?.HttpMethods.Contains(method) == true);
            var context = new DefaultHttpContext { RequestServices = app.Services };
            context.Request.Method = method;
            context.Request.Path = path;
            context.Request.QueryString = new QueryString(query);
            context.Request.Body = new MemoryStream(Encoding.UTF8.GetBytes(body ?? ""));
            context.Request.ContentType = "application/json";
            context.Request.ContentLength = context.Request.Body.Length;
            context.Response.Body = new MemoryStream();
            context.SetEndpoint(endpoint);
            await endpoint.RequestDelegate!(context);
            context.Response.Body.Position = 0;
            return (context.Response.StatusCode, await new StreamReader(context.Response.Body).ReadToEndAsync());
        }

        public ValueTask DisposeAsync() => app.DisposeAsync();
    }

    [Theory]
    [InlineData("/api/health", "GET")]
    [InlineData("/api/queue", "GET")]
    [InlineData("/api/queue/changes", "GET")]
    [InlineData("/api/queue/add", "POST")]
    [InlineData("/api/queue/change", "POST")]
    [InlineData("/api/queue/move-many", "POST")]
    [InlineData("/api/profiles", "GET")]
    public async Task APIのパスとHTTPメソッドを登録する(string path, string method)
    {
        await using var fixture = new EndpointFixture();
        Assert.Single(fixture.Endpoints, e => e.RoutePattern.RawText == path &&
            e.Metadata.GetMetadata<HttpMethodMetadata>()?.HttpMethods.Contains(method) == true);
    }

    [Fact]
    public async Task ヘルス応答は成功を返す()
    {
        await using var fixture = new EndpointFixture();
        var result = await fixture.Send("GET", "/api/health");
        Assert.Equal(200, result.Status);
        using var json = JsonDocument.Parse(result.Body);
        Assert.True(json.RootElement.GetProperty("ok").GetBoolean());
    }

    [Theory]
    [InlineData("")]
    [InlineData("?since=abc")]
    [InlineData("?since=9223372036854775808")]
    public async Task キュー差分は未指定または不正なカーソルを拒否する(string query)
    {
        await using var fixture = new EndpointFixture();
        var result = await fixture.Send("GET", "/api/queue/changes", query);
        Assert.Equal(400, result.Status);
        using var json = JsonDocument.Parse(result.Body);
        Assert.Equal("since is required.", json.RootElement.GetProperty("error").GetString());
    }

    [Fact]
    public async Task キュー検索クエリを変換してDTOをJSONで返す()
    {
        await using var fixture = new EndpointFixture();
        await fixture.Queue.Seed(RestQueueTestFixture.Item(1), RestQueueTestFixture.Item(2, QueueState.Complete));
        var result = await fixture.Send("GET", "/api/queue", "?state=complete&search=" + Uri.EscapeDataString("番組2") + "&searchTargets=file,service&hideOneSeg=true");
        Assert.Equal(200, result.Status);
        using var json = JsonDocument.Parse(result.Body);
        var item = Assert.Single(json.RootElement.GetProperty("items").EnumerateArray());
        Assert.Equal(2, item.GetProperty("id").GetInt32());
        Assert.Equal("Batch", item.GetProperty("mode").GetString());
        Assert.Equal("Complete", item.GetProperty("state").GetString());
        Assert.True(json.RootElement.GetProperty("filters").GetProperty("hideOneSeg").GetBoolean());
        var changes = await fixture.Send("GET", "/api/queue/changes", "?since=1");
        Assert.Equal(200, changes.Status);
    }

    [Theory]
    [InlineData("null", null)]
    [InlineData("{}", "Output is required.")]
    [InlineData("{\"outputs\":[]}", "Output is required.")]
    [InlineData("{\"outputs\":[{\"dstPath\":\" \"}]}", "Output directory is required.")]
    [InlineData("{\"targets\":[{\"path\":\"\"}],\"outputs\":[{\"dstPath\":\"/架空\"}]}", "Target file not found.")]
    public async Task キュー追加は不足入力を登録前に拒否する(string body, string? error)
    {
        await using var fixture = new EndpointFixture();
        var result = await fixture.Send("POST", "/api/queue/add", body: body);
        Assert.Equal(400, result.Status);
        if (error != null)
        {
            using var json = JsonDocument.Parse(result.Body);
            Assert.Equal(error, json.RootElement.GetProperty("error").GetString());
        }
        Assert.Empty(fixture.Queue.Manager.Queue);
    }

    [Theory]
    [InlineData("null")]
    [InlineData("{}")]
    [InlineData("{\"itemIds\":[]}")]
    public async Task 複数移動は対象ID不足を拒否する(string body)
    {
        await using var fixture = new EndpointFixture();
        Assert.Equal(400, (await fixture.Send("POST", "/api/queue/move-many", body: body)).Status);
    }

    [Theory]
    [InlineData(-1)]
    [InlineData(int.MinValue)]
    public async Task RESTの単一移動でも負位置を無視してキューを維持する(int position)
    {
        await using var fixture = new EndpointFixture();
        await fixture.Queue.Seed(RestQueueTestFixture.Item(1), RestQueueTestFixture.Item(2));
        var before = fixture.Queue.Store.GetQueueView(null!);
        var result = await fixture.Send("POST", "/api/queue/change", body:
            JsonSerializer.Serialize(new { itemId = 1, changeType = "Move", position, requestId = "invalid-move" }));
        Assert.Equal(200, result.Status);
        using var json = JsonDocument.Parse(result.Body);
        Assert.True(json.RootElement.GetProperty("ok").GetBoolean());
        Assert.Equal(new[] { 1, 2 }, fixture.Queue.Manager.Queue.Select(i => i.Id));
        var after = fixture.Queue.Store.GetQueueView(null!);
        Assert.Equal(before.Version, after.Version);
        Assert.Equal(before.Digest, after.Digest);
        Assert.False(fixture.Queue.Store.TryGetMessageForRequestId("invalid-move", out _));
    }

    [Fact]
    public async Task RESTの複数移動はキューと状態ストアを更新する()
    {
        await using var fixture = new EndpointFixture();
        await fixture.Queue.Seed(RestQueueTestFixture.Item(1), RestQueueTestFixture.Item(2), RestQueueTestFixture.Item(3));
        var result = await fixture.Send("POST", "/api/queue/move-many",
            body: "{\"itemIds\":[3,1],\"dropIndex\":0,\"requestId\":\"move-test\"}");
        Assert.Equal(200, result.Status);
        using var json = JsonDocument.Parse(result.Body);
        Assert.True(json.RootElement.GetProperty("ok").GetBoolean());
        Assert.Equal("move-test", json.RootElement.GetProperty("requestId").GetString());
        Assert.Equal(new[] { 3, 1, 2 }, fixture.Queue.Manager.Queue.Select(i => i.Id));
        Assert.Equal(new[] { 3, 1, 2 }, fixture.Queue.Store.GetQueueView(null!).Items.Select(i => i.Id));
    }

    [Fact]
    public async Task エンコードログのページングは負の位置と上限を正規化する()
    {
        await using var fixture = new EndpointFixture();
        await fixture.Queue.Store.OnUIData(new UIData { LogData = new LogData
        {
            Items = Enumerable.Range(0, 250).Select(i => new LogItem { SrcPath = $"/架空/{i}.ts" }).ToList(),
        } });
        var result = await fixture.Send("GET", "/api/logs/encode/page", "?offset=-3&limit=999");
        Assert.Equal(200, result.Status);
        using var json = JsonDocument.Parse(result.Body);
        Assert.Equal(250, json.RootElement.GetProperty("total").GetInt32());
        Assert.Equal(200, json.RootElement.GetProperty("items").GetArrayLength());
        var beyond = await fixture.Send("GET", "/api/logs/encode/page", "?offset=999&limit=0");
        using var beyondJson = JsonDocument.Parse(beyond.Body);
        Assert.Empty(beyondJson.RootElement.GetProperty("items").EnumerateArray());
    }
}
