using System.Text.Json;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class RestApiInputValidationTests
{
    [Theory]
    [InlineData("/api/logo/analyze", "null")]
    [InlineData("/api/logo/analyze", "{}")]
    [InlineData("/api/logo/analyze", "{\"queueItemId\":999}")]
    [InlineData("/api/logo/analyze/auto", "null")]
    [InlineData("/api/logo/analyze/auto", "{}")]
    [InlineData("/api/logo/analyze/auto", "{\"queueItemId\":999}")]
    [InlineData("/api/logo/preview/sessions", "null")]
    [InlineData("/api/logo/preview/sessions", "{}")]
    [InlineData("/api/logo/preview/sessions", "{\"queueItemId\":999}")]
    [InlineData("/api/trim/sessions", "null")]
    [InlineData("/api/trim/sessions", "{}")]
    [InlineData("/api/trim/sessions", "{\"queueItemId\":999}")]
    [InlineData("/api/trim/requeue", "null")]
    [InlineData("/api/trim/requeue", "{}")]
    [InlineData("/api/trim/requeue", "{\"queueItemId\":999,\"profile\":\"標準\"}")]
    public async Task ネイティブ処理の開始前に必須入力不足と未知キューIDを拒否する(string path, string body)
    {
        await using var f = new RestEndpointTestFixture();
        Assert.Equal(400, (await f.Send("POST", path, body: body)).Status);
        Assert.Empty(f.Queue.Manager.Queue);
    }

    [Theory]
    [InlineData("GET", "/api/logo/analyze/auto/{jobId}", "", 404)]
    [InlineData("GET", "/api/logo/analyze/auto/{jobId}/debug/{kind}", "", 404)]
    [InlineData("GET", "/api/logo/analyze/{jobId}", "", 404)]
    [InlineData("GET", "/api/logo/analyze/{jobId}/file", "", 404)]
    [InlineData("GET", "/api/logo/analyze/{jobId}/image", "", 404)]
    [InlineData("GET", "/api/logo/analyze/{jobId}/debug-image", "", 404)]
    [InlineData("POST", "/api/logo/analyze/{jobId}/apply", "", 400)]
    [InlineData("POST", "/api/logo/analyze/{jobId}/discard", "", 400)]
    [InlineData("GET", "/api/logo/preview/sessions/{sessionId}/frame", "?pos=0", 404)]
    [InlineData("GET", "/api/logo/preview/sessions/{sessionId}/frame", "", 404)]
    [InlineData("DELETE", "/api/logo/preview/sessions/{sessionId}", "", 404)]
    [InlineData("GET", "/api/trim/sessions/{sessionId}/bundle", "?n=0", 404)]
    [InlineData("GET", "/api/trim/sessions/{sessionId}/bundle", "?n=abc", 400)]
    [InlineData("GET", "/api/trim/sessions/{sessionId}/waveform", "?n=0", 404)]
    [InlineData("GET", "/api/trim/sessions/{sessionId}/waveform", "", 400)]
    [InlineData("DELETE", "/api/trim/sessions/{sessionId}", "", 404)]
    [InlineData("DELETE", "/api/trim/tempdir/{queueItemId:int}", "", 400)]
    [InlineData("GET", "/api/assets/logo/{serviceId:int}/{logoIdx:int}", "", 404)]
    [InlineData("GET", "/api/assets/drcs/{md5}", "", 404)]
    public async Task 未登録IDと必須クエリ不足はファイルやネイティブへ入らず拒否する(string method, string path, string query, int expected)
    {
        await using var f = new RestEndpointTestFixture();
        var values = new { jobId = "未登録", kind = "point", sessionId = "未登録", queueItemId = 999, serviceId = 999, logoIdx = 0, md5 = "未登録" };
        Assert.Equal(expected, (await f.Send(method, path, query, routeValues: values)).Status);
    }

    [Theory]
    [InlineData("/api/trim/sessions/{sessionId}/save", "null")]
    [InlineData("/api/trim/sessions/{sessionId}/save", "{}")]
    public async Task Trim保存は未知セッションと空入力を拒否する(string path, string body)
    {
        await using var f = new RestEndpointTestFixture();
        Assert.Equal(400, (await f.Send("POST", path, body: body, routeValues: new { sessionId = "未登録" })).Status);
    }

    [Theory]
    [InlineData("/api/services/logo", "application/json", "{}")]
    [InlineData("/api/services/logo/probe", "application/json", "{}")]
    [InlineData("/api/services/logo", "application/x-www-form-urlencoded", "serviceId=42")]
    [InlineData("/api/services/logo/probe", "application/x-www-form-urlencoded", "serviceId=42")]
    public async Task ロゴアップロードはフォーム形式と必須ファイルを検証する(string path, string contentType, string body)
    {
        await using var f = new RestEndpointTestFixture();
        Assert.Equal(400, (await f.Send("POST", path, body: body, contentType: contentType)).Status);
    }

    [Theory]
    [InlineData("POST", "/api/drcs", "null")]
    [InlineData("PUT", "/api/drcs/map", "null")]
    [InlineData("PUT", "/api/drcs/map", "{}")]
    [InlineData("PUT", "/api/drcs/map", "{\"md5\":\" \"}")]
    [InlineData("DELETE", "/api/drcs/map/{md5}", "")]
    [InlineData("POST", "/api/queue/change", "null")]
    [InlineData("POST", "/api/queue/pause", "null")]
    [InlineData("POST", "/api/makescript/preview", "null")]
    [InlineData("POST", "/api/makescript/preview", "{}")]
    [InlineData("POST", "/api/makescript/file", "null")]
    [InlineData("POST", "/api/makescript/file", "{}")]
    public async Task 補助更新は必須入力不足を拒否する(string method, string path, string body)
    {
        await using var f = new RestEndpointTestFixture();
        Assert.Equal(400, (await f.Send(method, path, body: body, routeValues: new { md5 = " " })).Status);
    }

    [Theory]
    [InlineData("/api/logs/file", "")]
    [InlineData("/api/logs/file", "?encodeStart=invalid")]
    [InlineData("/api/logs/file", "?checkStart=invalid")]
    [InlineData("/api/console/{taskId:int}/changes", "")]
    [InlineData("/api/console/{taskId:int}/changes", "?since=invalid")]
    public async Task ログとコンソールは必須クエリを検証する(string path, string query)
    {
        await using var f = new RestEndpointTestFixture();
        Assert.Equal(400, (await f.Send("GET", path, query, routeValues: new { taskId = 999 })).Status);
    }

    [Theory]
    [InlineData("/api/console/{taskId:int}", "")]
    [InlineData("/api/console/{taskId:int}/changes", "?since=0")]
    public async Task 未登録タスクのコンソールは見つからない(string path, string query)
    {
        await using var f = new RestEndpointTestFixture();
        Assert.Equal(404, (await f.Send("GET", path, query, routeValues: new { taskId = 999 })).Status);
    }

    [Fact]
    public async Task 全OPTIONSハンドラーは成功しキューを変更しない()
    {
        await using var f = new RestEndpointTestFixture();
        var paths = f.Endpoints.Where(e => e.Metadata.GetMetadata<Microsoft.AspNetCore.Routing.HttpMethodMetadata>()?.HttpMethods.Contains("OPTIONS") == true)
            .Select(e => e.RoutePattern.RawText!).ToArray();
        Assert.Equal(14, paths.Length);
        foreach (var path in paths)
            Assert.Equal(200, (await f.Send("OPTIONS", path)).Status);
        Assert.Empty(f.Queue.Manager.Queue);
    }
}
