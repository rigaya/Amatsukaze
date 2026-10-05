using System.Text;
using System.Text.Json;
using Amatsukaze.Server;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class RestApiReadAndQueueTests
{
    [Theory]
    [InlineData("/api/snapshot")]
    [InlineData("/api/system")]
    [InlineData("/api/server-env")]
    [InlineData("/api/info/summary")]
    [InlineData("/api/info/disks")]
    [InlineData("/api/ui-state")]
    [InlineData("/api/profile-options")]
    [InlineData("/api/autoselect/options")]
    [InlineData("/api/services")]
    [InlineData("/api/service-settings")]
    [InlineData("/api/service-options")]
    [InlineData("/api/settings/options")]
    [InlineData("/api/drcs")]
    [InlineData("/api/batfiles/addqueue")]
    [InlineData("/api/batfiles/queuefinish")]
    public async Task 読取APIは初期状態をJSONで返す(string path)
    {
        await using var f = new RestEndpointTestFixture();
        var response = await f.Send("GET", path);
        Assert.Equal(200, response.Status);
        using var json = JsonDocument.Parse(response.Body);
        Assert.Contains(json.RootElement.ValueKind, new[] { JsonValueKind.Object, JsonValueKind.Array });
    }

    [Theory]
    [InlineData("GET", "/api/update/status", 200)]
    [InlineData("POST", "/api/update/check", 200)]
    [InlineData("POST", "/api/update/apply", 503)]
    [InlineData("POST", "/api/update/discard-pending", 503)]
    [InlineData("POST", "/api/update/cancel", 503)]
    [InlineData("GET", "/api/update/job/{jobId}", 404)]
    [InlineData("GET", "/api/update/job/{jobId}/log", 404)]
    public async Task 更新APIは管理サービス未初期化時の応答を返す(string method, string path, int expected)
    {
        await using var f = new RestEndpointTestFixture();
        var response = await f.Send(method, path, body: method == "POST" ? "{}" : null, routeValues: new { jobId = "未登録" });
        Assert.Equal(expected, response.Status);
        if (path == "/api/update/status")
        {
            using var json = JsonDocument.Parse(response.Body);
            Assert.False(json.RootElement.GetProperty("supported").GetBoolean());
            Assert.Equal("update_manager_not_initialized", json.RootElement.GetProperty("unsupportedReason").GetString());
        }
        if (path == "/api/update/check")
        {
            using var json = JsonDocument.Parse(response.Body);
            Assert.Equal("", json.RootElement.GetProperty("jobId").GetString());
        }
    }

    [Fact]
    public async Task スナップショットと概要とUI状態は通知されたデータを返す()
    {
        await using var f = new RestEndpointTestFixture();
        await f.Queue.Seed(RestQueueTestFixture.Item(7));
        await f.Queue.Store.OnCommonData(new CommonData {
            ServerInfo = new ServerInfo { HostName = "試験サーバー", Version = "試験版", Platform = "試験OS" },
            UIState = new UIState { LastUsedProfile = "標準", LastOutputPath = "/試験", OutputPathHistory = new() { "/試験" } },
        });
        using var info = JsonDocument.Parse((await f.Send("GET", "/api/info/summary")).Body);
        Assert.Equal("試験サーバー", info.RootElement.GetProperty("hostName").GetString());
        using var ui = JsonDocument.Parse((await f.Send("GET", "/api/ui-state")).Body);
        Assert.Equal("標準", ui.RootElement.GetProperty("lastUsedProfile").GetString());
        Assert.Equal("/試験", Assert.Single(ui.RootElement.GetProperty("outputPathHistory").EnumerateArray()).GetString());
        using var snapshot = JsonDocument.Parse((await f.Send("GET", "/api/snapshot")).Body);
        Assert.Equal(7, Assert.Single(snapshot.RootElement.GetProperty("queueView").GetProperty("items").EnumerateArray()).GetProperty("id").GetInt32());
    }

    [Fact]
    public async Task オプションDTOは選択肢と通知済みスクリプト一覧を含む()
    {
        await using var f = new RestEndpointTestFixture();
        await f.Queue.Store.OnCommonData(new CommonData {
            JlsCommandFiles = new() { "解析.txt" }, MainScriptFiles = new() { "フィルタ.avs" },
            AddQueueBatFiles = new() { "追加.sh" }, QueueFinishBatFiles = new() { "完了.sh" },
        });
        using var profile = JsonDocument.Parse((await f.Send("GET", "/api/profile-options")).Body);
        Assert.NotEmpty(profile.RootElement.GetProperty("encoderList").EnumerateArray());
        Assert.Equal("解析.txt", Assert.Single(profile.RootElement.GetProperty("jlsCommandFiles").EnumerateArray()).GetString());
        Assert.Equal("フィルタ.avs", Assert.Single(profile.RootElement.GetProperty("mainScriptFiles").EnumerateArray()).GetString());
        using var service = JsonDocument.Parse((await f.Send("GET", "/api/service-options")).Body);
        Assert.Equal("解析.txt", Assert.Single(service.RootElement.GetProperty("jlsCommandFiles").EnumerateArray()).GetString());
        foreach (var pair in new[] { ("/api/batfiles/addqueue", "追加.sh"), ("/api/batfiles/queuefinish", "完了.sh") })
        {
            using var json = JsonDocument.Parse((await f.Send("GET", pair.Item1)).Body);
            Assert.Equal(pair.Item2, Assert.Single(json.RootElement.EnumerateArray()).GetString());
        }
        using var options = JsonDocument.Parse((await f.Send("GET", "/api/settings/options")).Body);
        Assert.NotEmpty(options.RootElement.GetProperty("trimAdjustPreviewScaleModes").EnumerateArray());
    }

    [Fact]
    public async Task コンソールは割当タスクの履歴と差分を返す()
    {
        await using var f = new RestEndpointTestFixture();
        var item = RestQueueTestFixture.Item(1, QueueState.Encoding);
        item.ConsoleId = 7;
        item.EncodeStart = new DateTime(2026, 10, 5);
        await f.Queue.Seed(item);
        await f.Queue.Store.OnConsoleUpdate(new ConsoleUpdate { index = 7, data = Encoding.ASCII.GetBytes("first\nsecond\n") });
        var response = await f.Send("GET", "/api/console/{taskId:int}", routeValues: new { taskId = 1 });
        Assert.Equal(200, response.Status);
        using var json = JsonDocument.Parse(response.Body);
        Assert.Equal(new[] { "first", "second" }, json.RootElement.GetProperty("lines").EnumerateArray().Select(line => line.GetString()));
        response = await f.Send("GET", "/api/console/{taskId:int}/changes", "?since=1", routeValues: new { taskId = 1 });
        Assert.Equal(200, response.Status);
        Assert.Contains("second", response.Body);
    }

    [Fact]
    public async Task メッセージAPIは操作IDとレベルで絞り込む()
    {
        await using var f = new RestEndpointTestFixture();
        await f.Send("POST", "/api/queue/change", body: "{\"itemId\":999,\"changeType\":\"Priority\",\"requestId\":\"missing\"}");
        var response = await f.Send("GET", "/api/messages/changes", "?since=0&page=queue&requestId=missing&levels=ERROR&max=1");
        Assert.Equal(200, response.Status);
        using var json = JsonDocument.Parse(response.Body);
        var message = Assert.Single(json.RootElement.GetProperty("items").EnumerateArray());
        Assert.Equal("missing", message.GetProperty("requestId").GetString());
        Assert.Equal("error", message.GetProperty("level").GetString());
    }

    [Theory]
    [InlineData("/api/queue/cancel-add")]
    [InlineData("/api/system/cancel-sleep")]
    [InlineData("/api/services/logo/rescan")]
    public async Task 補助操作はメモリ内の要求を受け付ける(string path)
    {
        await using var f = new RestEndpointTestFixture();
        var response = await f.Send("POST", path);
        Assert.Equal(200, response.Status);
        using var json = JsonDocument.Parse(response.Body);
        Assert.True(json.RootElement.GetProperty("ok").GetBoolean());
        if (path == "/api/queue/cancel-add")
            Assert.True(f.Queue.AddQueueCanceled);
        if (path == "/api/services/logo/rescan")
            Assert.True(f.Queue.LogoRescanRequested);
    }

    [Fact]
    public async Task 終了APIはfixtureの終了通知だけを呼ぶ()
    {
        await using var f = new RestEndpointTestFixture();
        var calls = 0;
        f.Queue.SetFinishRequested(() => calls++);
        Assert.Equal(200, (await f.Send("POST", "/api/system/end")).Status);
        Assert.Equal(1, calls);
    }

    [Fact]
    public async Task キュー停止再開はシステムDTOへ反映する()
    {
        await using var f = new RestEndpointTestFixture();
        foreach (var pause in new[] { true, false })
        {
            Assert.Equal(200, (await f.Send("POST", "/api/queue/pause", body: RestApiConfigurationTests.Json(new PauseRequest { IsQueue = true, Pause = pause }))).Status);
            using var json = JsonDocument.Parse((await f.Send("GET", "/api/system")).Body);
            Assert.Equal(pause, json.RootElement.GetProperty("state").GetProperty("pause").GetBoolean());
        }
    }

    [Fact]
    public async Task キュー優先度変更と通常移動は状態ストアも同期する()
    {
        await using var f = new RestEndpointTestFixture();
        await f.Queue.Seed(RestQueueTestFixture.Item(1), RestQueueTestFixture.Item(2), RestQueueTestFixture.Item(3));
        var response = await f.Send("POST", "/api/queue/change", body: "{\"itemId\":2,\"changeType\":\"Priority\",\"priority\":5,\"requestId\":\"priority\"}");
        RestApiConfigurationTests.RequestId(response);
        Assert.Equal(5, f.Queue.Manager.Queue.Single(i => i.Id == 2).Priority);
        Assert.Equal(5, f.Queue.Store.GetQueueView(null!).Items.Single(i => i.Id == 2).Priority);
        using (var json = JsonDocument.Parse(response.Body)) Assert.Equal("info", json.RootElement.GetProperty("level").GetString());
        RestApiConfigurationTests.RequestId(await f.Send("POST", "/api/queue/change", body: "{\"itemId\":3,\"changeType\":\"Move\",\"position\":0}"));
        Assert.Equal(new[] { 3, 1, 2 }, f.Queue.Manager.Queue.Select(i => i.Id));
        Assert.Equal(new[] { 3, 1, 2 }, f.Queue.Store.GetQueueView(null!).Items.Select(i => i.Id));
    }

    [Theory]
    [InlineData("ResetState")]
    [InlineData("UpdateProfile")]
    [InlineData("Profile")]
    public async Task エンコード中の変更拒否はエラー通知を返し項目を維持する(string changeType)
    {
        await using var f = new RestEndpointTestFixture();
        var item = RestQueueTestFixture.Item(1, QueueState.Encoding);
        await f.Queue.Seed(item);
        var response = await f.Send("POST", "/api/queue/change", body: RestApiConfigurationTests.Json(new { itemId = 1, changeType, profile = "別名" }));
        var id = RestApiConfigurationTests.RequestId(response);
        Assert.True(f.Queue.Store.TryGetMessageForRequestId(id, out var message));
        Assert.Equal("error", message.Level);
        Assert.Same(item, Assert.Single(f.Queue.Manager.Queue));
        Assert.Equal("標準", item.ProfileName);
        Assert.Equal(QueueState.Encoding, item.State);
    }
}

[Collection("REST一時保存先")]
public sealed class RestApiFileTests
{
    [Fact]
    public async Task ログ一覧とページングとCSVは通知済み履歴を返す()
    {
        await using var f = new RestFileSystemFixture();
        const string reason = "試験理由";
        await f.Api.Queue.Store.OnUIData(new UIData {
            LogData = new LogData { Items = new() { new() { SrcPath = "/架空/番組.ts", Reason = reason } } },
            CheckLogData = new CheckLogData { Items = new() { new() { SrcPath = "/架空/確認.ts", Reason = reason } } },
        });
        foreach (var path in new[] { "/api/logs/encode", "/api/logs/check" })
        {
            var response = await f.Api.Send("GET", path);
            Assert.Equal(200, response.Status);
            using var json = JsonDocument.Parse(response.Body);
            Assert.Equal(reason, Assert.Single(json.RootElement.EnumerateArray()).GetProperty("reason").GetString());
        }
        foreach (var path in new[] { "/api/logs/encode/page", "/api/logs/check/page" })
        {
            var response = await f.Api.Send("GET", path, "?offset=-1&limit=0");
            Assert.Equal(200, response.Status);
            using var json = JsonDocument.Parse(response.Body);
            Assert.Equal(1, json.RootElement.GetProperty("total").GetInt32());
            Assert.Single(json.RootElement.GetProperty("items").EnumerateArray());
        }
        foreach (var path in new[] { "/api/logs/encode.csv", "/api/logs/check.csv" })
        {
            var response = await f.Api.Send("GET", path);
            Assert.Equal(200, response.Status);
            Assert.Contains(reason, response.Body);
        }
    }

    [Theory]
    [InlineData("encodeStart")]
    [InlineData("checkStart")]
    public async Task ログ読取は一時領域の指定日時だけを返す(string key)
    {
        await using var f = new RestFileSystemFixture();
        var date = new DateTime(2026, 10, 5, 12, 0, 0);
        var path = key == "encodeStart" ? f.Api.Queue.Server.GetLogFileBase(date) : f.Api.Queue.Server.GetCheckLogFileBase(date);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllBytes(path + ".txt", Util.AmatsukazeDefaultEncoding.GetBytes("試験ログ"));
        var response = await f.Api.Send("GET", "/api/logs/file", "?" + key + "=" + Uri.EscapeDataString(date.ToString("O")));
        Assert.Equal(200, response.Status);
        using var json = JsonDocument.Parse(response.Body);
        Assert.Equal("試験ログ", json.RootElement.GetProperty("content").GetString());
        Assert.True(json.RootElement.GetProperty("meta").GetProperty("size").GetInt64() > 0);
    }

    [Fact]
    public async Task パス候補は拡張子とファイル許可で絞り込む()
    {
        await using var f = new RestFileSystemFixture();
        File.WriteAllText(Path.Combine(f.Root, "番組.ts"), "テスト用");
        File.WriteAllText(Path.Combine(f.Root, "除外.txt"), "テスト用");
        var query = "?input=" + Uri.EscapeDataString(f.Root + Path.DirectorySeparatorChar) + "&ext=ts&allowDirs=false";
        using var json = JsonDocument.Parse((await f.Api.Send("GET", "/api/path/suggest", query)).Body);
        Assert.Empty(json.RootElement.GetProperty("dirs").EnumerateArray());
        var file = Assert.Single(json.RootElement.GetProperty("files").EnumerateArray());
        Assert.Contains("番組.ts", file.ToString());
        using var noFiles = JsonDocument.Parse((await f.Api.Send("GET", "/api/path/suggest", query + "&allowFiles=false")).Body);
        Assert.Empty(noFiles.RootElement.GetProperty("files").EnumerateArray());
    }

    [Theory]
    [InlineData("sh")]
    [InlineData("bat")]
    public async Task スクリプト生成はプレビューとダウンロードで同じ内容を返す(string type)
    {
        await using var f = new RestFileSystemFixture();
        var data = new MakeScriptData { Profile = "標準", OutDir = f.Root, Priority = 3 };
        RestApiConfigurationTests.RequestId(await f.Api.Send("PUT", "/api/makescript", body: RestApiConfigurationTests.Json(data)));
        var body = RestApiConfigurationTests.Json(new { makeScriptData = data, targetHost = "local", scriptType = type });
        var preview = await f.Api.Send("POST", "/api/makescript/preview", body: body);
        Assert.Equal(200, preview.Status);
        using var json = JsonDocument.Parse(preview.Body);
        var script = json.RootElement.GetProperty("commandLine").GetString();
        Assert.Contains("127.0.0.1", script);
        Assert.Contains("標準", script);
        var file = await f.Api.Send("POST", "/api/makescript/file", body: body);
        Assert.Equal(200, file.Status);
        Assert.Equal(script, file.Body);
        var current = await f.Api.Send("GET", "/api/makescript/preview");
        Assert.Equal(200, current.Status);
        Assert.Contains("127.0.0.1", current.Body);
    }

    [Fact]
    public async Task DRCSマッピング更新と削除は一時ファイルと状態を同期する()
    {
        await using var f = new RestFileSystemFixture();
        var image = new DrcsImage { MD5 = "00112233445566778899aabbccddeeff", MapStr = null };
        await f.SeedDrcs(image);
        Assert.Equal(200, (await f.Api.Send("PUT", "/api/drcs/map", body: RestApiConfigurationTests.Json(new { md5 = image.MD5, mapStr = "文字" }))).Status);
        Assert.Contains(image.MD5 + "=文字", File.ReadAllText(f.Api.Queue.Server.GetDRCSMapPath()));
        using (var json = JsonDocument.Parse((await f.Api.Send("GET", "/api/drcs")).Body))
            Assert.Equal("文字", Assert.Single(json.RootElement.EnumerateArray()).GetProperty("mapStr").GetString());
        Assert.Equal(200, (await f.Api.Send("POST", "/api/drcs", body: RestApiConfigurationTests.Json(new { md5 = image.MD5, mapStr = "変更" }))).Status);
        Assert.Contains(image.MD5 + "=変更", File.ReadAllText(f.Api.Queue.Server.GetDRCSMapPath()));
        Assert.Equal(200, (await f.Api.Send("DELETE", "/api/drcs/map/{md5}", routeValues: new { md5 = image.MD5 })).Status);
        Assert.DoesNotContain(image.MD5, File.ReadAllText(f.Api.Queue.Server.GetDRCSMapPath()));
        using var empty = JsonDocument.Parse((await f.Api.Send("GET", "/api/drcs")).Body);
        Assert.Empty(empty.RootElement.EnumerateArray());
        using var appearances = JsonDocument.Parse((await f.Api.Send("GET", "/api/drcs/appearance/{md5}", routeValues: new { md5 = image.MD5 })).Body);
        Assert.Equal(image.MD5, appearances.RootElement.GetProperty("md5").GetString());
        Assert.Empty(appearances.RootElement.GetProperty("items").EnumerateArray());
    }
}
