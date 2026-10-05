using System.Text.Json;
using System.Threading.Tasks.Dataflow;
using Amatsukaze.Server;
using Xunit;

namespace AmatsukazeServerTest;

[Collection("REST一時保存先")]
public sealed class RestApiPriorityATests
{
    private static string Json(object value) => RestApiConfigurationTests.Json(value);
    private static string Ok((int Status, string Body) response) => RestApiConfigurationTests.RequestId(response);

    private static async Task SeedSettings(RestFileSystemFixture f)
    {
        var profile = ServerSupport.NormalizeProfile(null!);
        profile.Name = "標準";
        Ok(await f.Api.Send("POST", "/api/profiles", body: Json(profile)));
        Ok(await f.Api.Send("POST", "/api/autoselect", body: Json(new AutoSelectProfile { Name = "選択", Conditions = new() { new() { Profile = "標準", Priority = 3, Tag = "録画", TagEnabled = true } } })));
        Ok(await f.Api.Send("POST", "/api/services/update", body: Json(new ServiceSettingUpdate {
            Type = ServiceSettingUpdateType.Update, ServiceId = 42,
            Data = new ServiceSettingElement { ServiceId = 42, ServiceName = "放送局", JLSCommand = "解析.txt", JLSOption = "保持", LogoSettings = new() {
                new() { FileName = LogoSetting.NO_LOGO, ServiceId = 42, LogoName = "ロゴなし", Exists = true, Enabled = true }, } },
        })));
    }

    [Theory]
    [InlineData("POST", "/api/profiles")]
    [InlineData("PUT", "/api/profiles/{name}")]
    [InlineData("POST", "/api/autoselect")]
    [InlineData("PUT", "/api/autoselect/{name}")]
    [InlineData("PUT", "/api/settings")]
    [InlineData("PUT", "/api/finish-setting")]
    [InlineData("POST", "/api/services/update")]
    public async Task 空入力拒否は登録済み設定と保存済みファイルを保つ(string method, string path)
    {
        await using var f = new RestFileSystemFixture();
        await SeedSettings(f);
        var profile = f.Profiles["標準"];
        var auto = f.AutoSelects["選択"];
        var setting = f.Data.setting;
        var finish = f.Data.finishSetting;
        var service = f.Data.services.ServiceMap[42];
        var bytes = File.ReadAllBytes(Path.Combine(f.Root, "profile", "標準.profile"));
        var name = path.Contains("autoselect") ? "選択" : "標準";
        Assert.Equal(400, (await f.Api.Send(method, path, body: "null", routeValues: new { name })).Status);
        Assert.Same(profile, f.Profiles["標準"]);
        Assert.Same(auto, f.AutoSelects["選択"]);
        Assert.Same(service, f.Data.services.ServiceMap[42]);
        Assert.Same(setting, f.Data.setting);
        Assert.Same(finish, f.Data.finishSetting);
        Assert.Equal(bytes, File.ReadAllBytes(Path.Combine(f.Root, "profile", "標準.profile")));
        Assert.Equal("録画", Assert.Single(f.Api.Queue.Store.GetAutoSelects()).Conditions[0].Tag);
    }

    [Theory]
    [InlineData("/api/profiles/{name}")]
    [InlineData("/api/autoselect/{name}")]
    public async Task 未登録名の削除は登録済み設定とDTOを保つ(string path)
    {
        await using var f = new RestFileSystemFixture();
        await SeedSettings(f);
        var profile = f.Profiles["標準"];
        var auto = f.AutoSelects["選択"];
        var profiles = (await f.Api.Send("GET", "/api/profiles")).Body;
        var autos = (await f.Api.Send("GET", "/api/autoselect")).Body;
        Ok(await f.Api.Send("DELETE", path, routeValues: new { name = "未登録" }));
        Assert.Same(profile, Assert.Single(f.Profiles).Value);
        Assert.Same(auto, Assert.Single(f.AutoSelects).Value);
        Assert.Equal(profiles, (await f.Api.Send("GET", "/api/profiles")).Body);
        Assert.Equal(autos, (await f.Api.Send("GET", "/api/autoselect")).Body);
        Assert.True(File.Exists(Path.Combine(f.Root, "profile", "標準.profile")));
    }

    [Theory]
    [InlineData("/api/services/logo")]
    [InlineData("/api/services/logo/probe")]
    public async Task 空のロゴファイルをネイティブ処理前に拒否して設定を保つ(string path)
    {
        await using var f = new RestFileSystemFixture();
        await SeedSettings(f);
        var before = (await f.Api.Send("GET", "/api/service-settings")).Body;
        // ASCIIのboundaryを使い、実ファイルのアップロードや画像解析は行わない。
        const string form = "--test-boundary\r\nContent-Disposition: form-data; name=\"image\"; filename=\"empty.lgd\"\r\nContent-Type: application/octet-stream\r\n\r\n\r\n--test-boundary--\r\n";
        var response = await f.Api.Send("POST", path, body: form, contentType: "multipart/form-data; boundary=test-boundary");
        Assert.Equal(400, response.Status);
        Assert.Contains("Empty file.", response.Body);
        Assert.Equal(before, (await f.Api.Send("GET", "/api/service-settings")).Body);
        Assert.Empty(Directory.GetFiles(Path.Combine(f.Root, "logo")));
    }

    [Fact]
    public async Task 自動選択の条件更新は辞書と一覧DTOへ反映する()
    {
        await using var f = new RestFileSystemFixture();
        await SeedSettings(f);
        var updated = new AutoSelectProfile { Name = "選択", Conditions = new() { new() { Profile = "標準", Priority = 5, Tag = "変更", TagEnabled = true } } };
        Ok(await f.Api.Send("PUT", "/api/autoselect/{name}", body: Json(updated), routeValues: new { name = updated.Name }));
        var condition = Assert.Single(f.AutoSelects["選択"].Conditions);
        Assert.Equal(5, condition.Priority);
        Assert.Equal("変更", condition.Tag);
        var response = await f.Api.Send("GET", "/api/autoselect");
        Assert.Equal(200, response.Status);
        using var dto = JsonDocument.Parse(response.Body);
        var c = Assert.Single(Assert.Single(dto.RootElement.EnumerateArray()).GetProperty("conditions").EnumerateArray());
        Assert.Equal("標準", c.GetProperty("profile").GetString());
        Assert.Equal(5, c.GetProperty("priority").GetInt32());
        Assert.Equal("変更", c.GetProperty("tag").GetString());
        Assert.True(c.GetProperty("tagEnabled").GetBoolean());
    }

    [Fact]
    public async Task 自動選択の候補DTOは登録済み設定と条件選択肢を返す()
    {
        await using var f = new RestFileSystemFixture();
        await SeedSettings(f);
        var response = await f.Api.Send("GET", "/api/autoselect/options");
        Assert.Equal(200, response.Status);
        using var json = JsonDocument.Parse(response.Body);
        var root = json.RootElement;
        Assert.Equal("標準", Assert.Single(root.GetProperty("profileNames").EnumerateArray()).GetString());
        Assert.Equal(new[] { 1, 2, 3, 4, 5 }, root.GetProperty("priorityList").EnumerateArray().Select(p => p.GetInt32()));
        var service = Assert.Single(root.GetProperty("services").EnumerateArray());
        Assert.Equal(42, service.GetProperty("serviceId").GetInt32());
        Assert.Equal("放送局", service.GetProperty("name").GetString());
        Assert.NotEmpty(root.GetProperty("genres").EnumerateArray());
        Assert.Contains(root.GetProperty("videoSizes").EnumerateArray(), v => v.GetProperty("value").GetString() == "FullHD");
    }

    [Fact]
    public async Task サービスの通常更新は一覧と設定DTOと存在フラグを同期する()
    {
        await using var f = new RestFileSystemFixture();
        await SeedSettings(f);
        Ok(await f.Api.Send("POST", "/api/services/update", body: Json(new ServiceSettingUpdate {
            Type = ServiceSettingUpdateType.Update, ServiceId = 42,
            Data = new ServiceSettingElement { ServiceId = 42, ServiceName = "変更局", JLSCommand = "変更.txt", JLSOption = "変更オプション", DisableCMCheck = true,
                LogoSettings = new() { new() { FileName = LogoSetting.NO_LOGO, Enabled = false, Exists = false } } },
        })));
        Assert.Equal("変更局", f.Data.services.ServiceMap[42].ServiceName);
        Assert.True(f.Data.services.ServiceMap[42].LogoSettings[0].Exists);
        foreach (var path in new[] { "/api/services", "/api/service-settings" })
        {
            var response = await f.Api.Send("GET", path);
            Assert.Equal(200, response.Status);
            using var json = JsonDocument.Parse(response.Body);
            var service = Assert.Single(json.RootElement.EnumerateArray());
            Assert.Equal(42, service.GetProperty("serviceId").GetInt32());
            Assert.Equal("変更局", service.GetProperty(path == "/api/services" ? "name" : "serviceName").GetString());
            var logo = Assert.Single(service.GetProperty(path == "/api/services" ? "logoList" : "logos").EnumerateArray());
            Assert.Equal(LogoSetting.NO_LOGO, logo.GetProperty("fileName").GetString());
            Assert.False(logo.GetProperty("enabled").GetBoolean());
            Assert.True(logo.GetProperty("exists").GetBoolean());
            Assert.Equal(JsonValueKind.Null, logo.GetProperty("imageUrl").ValueKind);
            if (path == "/api/service-settings")
            {
                Assert.True(service.GetProperty("disableCMCheck").GetBoolean());
                Assert.Equal("変更.txt", service.GetProperty("jlsCommand").GetString());
                Assert.Equal("変更オプション", service.GetProperty("jlsOption").GetString());
            }
        }
    }

    [Theory]
    [InlineData("PUT", "/api/service-settings/{serviceId}/logos/period")]
    [InlineData("PUT", "/api/service-settings/{serviceId}/logos/enabled")]
    [InlineData("DELETE", "/api/service-settings/{serviceId}/logos")]
    public async Task ロゴの入力拒否と未知サービスは登録済み設定を保つ(string method, string path)
    {
        await using var f = new RestFileSystemFixture();
        await SeedSettings(f);
        var before = (await f.Api.Send("GET", "/api/service-settings")).Body;
        var service = f.Data.services.ServiceMap[42];
        Assert.Equal(400, (await f.Api.Send(method, path, body: "{}", routeValues: new { serviceId = 42 })).Status);
        Assert.Equal(404, (await f.Api.Send(method, path, body: Json(new { fileName = LogoSetting.NO_LOGO }), routeValues: new { serviceId = 999 })).Status);
        Assert.Same(service, f.Data.services.ServiceMap[42]);
        Assert.Equal(before, (await f.Api.Send("GET", "/api/service-settings")).Body);
    }

    [Fact]
    public async Task ロゴなし未登録時の削除拒否はサービスを保つ()
    {
        await using var f = new RestFileSystemFixture();
        Ok(await f.Api.Send("POST", "/api/services/update", body: Json(new ServiceSettingUpdate {
            Type = ServiceSettingUpdateType.Update, ServiceId = 42,
            Data = new ServiceSettingElement { ServiceId = 42, ServiceName = "放送局", LogoSettings = new() },
        })));
        var original = f.Data.services.ServiceMap[42];
        Assert.Equal(404, (await f.Api.Send("DELETE", "/api/service-settings/{serviceId}/logos/no-logo", routeValues: new { serviceId = 42 })).Status);
        Assert.Same(original, f.Data.services.ServiceMap[42]);
        Assert.Empty(original.LogoSettings);
    }

    [Fact]
    public async Task 未登録サービスへのロゴなし追加は既存設定を保つ()
    {
        await using var f = new RestFileSystemFixture();
        await SeedSettings(f);
        var original = f.Data.services.ServiceMap[42];
        var before = (await f.Api.Send("GET", "/api/service-settings")).Body;
        Ok(await f.Api.Send("POST", "/api/service-settings/{serviceId}/logos/no-logo", routeValues: new { serviceId = 999 }));
        Assert.Same(original, Assert.Single(f.Data.services.ServiceMap).Value);
        Assert.Equal(before, (await f.Api.Send("GET", "/api/service-settings")).Body);
    }

    [Fact]
    public async Task ロゴなし追加は既存ロゴを保持して個別設定を増やす()
    {
        await using var f = new RestFileSystemFixture();
        await SeedSettings(f);
        var original = f.Data.services.ServiceMap[42].LogoSettings[0];
        Ok(await f.Api.Send("POST", "/api/service-settings/{serviceId}/logos/no-logo", routeValues: new { serviceId = 42 }));
        var logos = f.Data.services.ServiceMap[42].LogoSettings;
        Assert.Equal(2, logos.Count);
        Assert.Same(original, logos[0]);
        Assert.Equal(LogoSetting.NO_LOGO, logos[1].FileName);
        Assert.Equal(42, logos[1].ServiceId);
        using var dto = JsonDocument.Parse((await f.Api.Send("GET", "/api/service-settings")).Body);
        Assert.Equal(2, Assert.Single(dto.RootElement.EnumerateArray()).GetProperty("logos").GetArrayLength());
    }

    [Fact]
    public async Task 完了設定はシステムDTOへ待機秒数と実行除外を反映する()
    {
        await using var f = new RestFileSystemFixture();
        Ok(await f.Api.Send("PUT", "/api/finish-setting", body: Json(new FinishSetting { Action = FinishAction.None, Seconds = 45, noActionExe = true, noActionExeList = new() { "試験.exe" } })));
        using var json = JsonDocument.Parse((await f.Api.Send("GET", "/api/system")).Body);
        var finish = json.RootElement.GetProperty("finishSetting");
        Assert.Equal("None", finish.GetProperty("action").GetString());
        Assert.Equal(45, finish.GetProperty("seconds").GetInt32());
        Assert.True(finish.GetProperty("noActionExe").GetBoolean());
        Assert.Equal("試験.exe", Assert.Single(finish.GetProperty("noActionExeList").EnumerateArray()).GetString());
    }

    [Fact]
    public async Task キュー追加は入力DTOをメモリ内要求へ変換して応答する()
    {
        await using var f = new RestFileSystemFixture();
        var requests = f.Api.Queue.CaptureQueueRequests();
        var input = Path.Combine(f.Root, "入力.ts");
        // ファイル存在検証用のダミー。受信スレッドがないので内容の解析・実行は行われない。
        File.WriteAllBytes(input, Array.Empty<byte>());
        var response = await f.Api.Send("POST", "/api/queue/add", body: Json(new {
            targets = new[] { new { path = input } }, outputs = new[] { new { dstPath = f.Root, profile = "標準", priority = 4 } },
            mode = "CMCheck", tags = new[] { "録画", "試験" }, addQueueBat = "追加.sh", requestId = "queued",
        }));
        Assert.Equal(200, response.Status);
        using var json = JsonDocument.Parse(response.Body);
        Assert.Equal("queued", json.RootElement.GetProperty("requestId").GetString());
        Assert.True(requests.TryReceive(out var request));
        var data = Assert.IsType<AddQueueRequest>(request);
        Assert.Equal(input, Assert.Single(data.Targets).Path);
        Assert.Equal(f.Root, Assert.Single(data.Outputs).DstPath);
        Assert.Equal("標準", data.Outputs[0].Profile);
        Assert.Equal(4, data.Outputs[0].Priority);
        Assert.Equal(ProcMode.CMCheck, data.Mode);
        Assert.Equal("queued", data.RequestId);
        Assert.Equal(new[] { "録画", "試験" }, data.Tags);
        Assert.Equal("追加.sh", data.AddQueueBat);
        Assert.Empty(f.Api.Queue.Manager.Queue);
    }

    [Fact]
    public async Task キュー追加の入力拒否は登録済みキューと要求バッファを保つ()
    {
        await using var f = new RestFileSystemFixture();
        var requests = f.Api.Queue.CaptureQueueRequests();
        await f.Api.Queue.Seed(RestQueueTestFixture.Item(1));
        var before = f.Api.Queue.Store.GetQueueView(null!);
        Assert.Equal(400, (await f.Api.Send("POST", "/api/queue/add", body: "{\"outputs\":[]}")).Status);
        Assert.Single(f.Api.Queue.Manager.Queue);
        Assert.Equal(before.Digest, f.Api.Queue.Store.GetQueueView(null!).Digest);
        Assert.False(requests.TryReceive(out _));
    }

    [Fact]
    public async Task キュー差分DTOは更新内容とカーソルを返す()
    {
        await using var f = new RestEndpointTestFixture();
        await f.Queue.Seed(RestQueueTestFixture.Item(1));
        // 一覧同期直後の空の差分履歴を初期化し、保持されているカーソルからの更新を確認する。
        Ok(await f.Send("POST", "/api/queue/change", body: "{\"itemId\":1,\"changeType\":\"Priority\",\"priority\":4}"));
        var since = f.Queue.Store.GetQueueView(null!).Version;
        Ok(await f.Send("POST", "/api/queue/change", body: "{\"itemId\":1,\"changeType\":\"Priority\",\"priority\":5}"));
        var current = f.Queue.Store.GetQueueView(null!);
        var response = await f.Send("GET", "/api/queue/changes", "?since=" + since.ToString(System.Globalization.CultureInfo.InvariantCulture));
        Assert.Equal(200, response.Status);
        using var json = JsonDocument.Parse(response.Body);
        Assert.False(json.RootElement.GetProperty("fullSyncRequired").GetBoolean());
        Assert.Equal(since, json.RootElement.GetProperty("fromVersion").GetInt64());
        Assert.Equal(current.Version, json.RootElement.GetProperty("toVersion").GetInt64());
        Assert.Equal(current.Digest, json.RootElement.GetProperty("queueViewDigest").GetString());
        var change = Assert.Single(json.RootElement.GetProperty("changes").EnumerateArray());
        Assert.Equal("Update", change.GetProperty("type").GetString());
        Assert.Equal(1, change.GetProperty("item").GetProperty("id").GetInt32());
        Assert.Equal(5, change.GetProperty("item").GetProperty("priority").GetInt32());
    }

    [Fact]
    public async Task 未登録IDへの移動と変更は既存キューを保つ()
    {
        await using var f = new RestEndpointTestFixture();
        await f.Queue.Seed(RestQueueTestFixture.Item(1));
        var before = f.Queue.Store.GetQueueView(null!);
        Ok(await f.Send("POST", "/api/queue/move-many", body: "{\"itemIds\":[999],\"dropIndex\":0}"));
        var id = Ok(await f.Send("POST", "/api/queue/change", body: "{\"itemId\":999,\"changeType\":\"Priority\",\"priority\":5}"));
        Assert.True(f.Queue.Store.TryGetMessageForRequestId(id, out var message));
        Assert.Equal("error", message.Level);
        Assert.Equal(1, Assert.Single(f.Queue.Manager.Queue).Id);
        var after = f.Queue.Store.GetQueueView(null!);
        Assert.Equal(before.Version, after.Version);
        Assert.Equal(before.Digest, after.Digest);
    }
}
