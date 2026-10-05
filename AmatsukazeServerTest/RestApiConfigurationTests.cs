using System.Text.Json;
using System.Text.Json.Serialization;
using Amatsukaze.Server;
using Xunit;

namespace AmatsukazeServerTest;

[Collection("REST一時保存先")]
public sealed class RestApiConfigurationTests
{
    internal static string Json(object data) => JsonSerializer.Serialize(data, new JsonSerializerOptions
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        Converters = { new JsonStringEnumConverter() },
    });

    internal static string RequestId((int Status, string Body) response)
    {
        Assert.Equal(200, response.Status);
        using var json = JsonDocument.Parse(response.Body);
        Assert.True(json.RootElement.GetProperty("ok").GetBoolean());
        var id = json.RootElement.GetProperty("requestId").GetString();
        Assert.False(string.IsNullOrWhiteSpace(id));
        return id!;
    }

    [Theory]
    [InlineData("POST", "/api/profiles", "")]
    [InlineData("POST", "/api/profiles", "null")]
    [InlineData("PUT", "/api/profiles/{name}", " ")]
    [InlineData("PUT", "/api/profiles/{name}", "null")]
    [InlineData("POST", "/api/autoselect", "")]
    [InlineData("POST", "/api/autoselect", "null")]
    [InlineData("PUT", "/api/autoselect/{name}", "")]
    [InlineData("PUT", "/api/autoselect/{name}", "null")]
    [InlineData("PUT", "/api/settings", "")]
    [InlineData("PUT", "/api/settings", "null")]
    [InlineData("PUT", "/api/makescript", "null")]
    [InlineData("PUT", "/api/finish-setting", "null")]
    [InlineData("POST", "/api/services/update", "null")]
    public async Task 設定更新は空入力を拒否し既存データを保つ(string method, string path, string body)
    {
        await using var f = new RestFileSystemFixture();
        var setting = f.Data.setting;
        var script = f.Data.scriptData;
        var finish = f.Data.finishSetting;
        Assert.Equal(400, (await f.Api.Send(method, path, body: body, routeValues: new { name = "標準" })).Status);
        Assert.Same(setting, f.Data.setting);
        Assert.Same(script, f.Data.scriptData);
        Assert.Same(finish, f.Data.finishSetting);
        Assert.Empty(f.Profiles);
        Assert.Empty(f.AutoSelects);
        Assert.Empty(f.Data.services.ServiceMap);
        Assert.Empty(Directory.GetFiles(Path.Combine(f.Root, "profile")));
    }

    [Fact]
    public async Task プロファイルは追加更新リネーム削除とDTOを同期する()
    {
        await using var f = new RestFileSystemFixture();
        var profile = ServerSupport.NormalizeProfile(null!);
        profile.Name = "標準";
        profile.EncoderType = EncoderType.x264;
        RequestId(await f.Api.Send("POST", "/api/profiles", body: Json(profile)));
        Assert.True(File.Exists(Path.Combine(f.Root, "profile", "標準.profile")));
        profile.Name = "保持";
        RequestId(await f.Api.Send("POST", "/api/profiles", body: Json(profile)));
        var preserved = f.Profiles["保持"];
        profile.Name = "標準";
        profile.X264Option = "--crf 20";
        RequestId(await f.Api.Send("PUT", "/api/profiles/{name}", body: Json(profile), routeValues: new { name = "標準" }));
        Assert.Equal("--crf 20", f.Profiles["標準"].X264Option);
        using (var json = JsonDocument.Parse((await f.Api.Send("GET", "/api/profiles")).Body))
            Assert.Contains(json.RootElement.EnumerateArray(), p => p.GetProperty("name").GetString() == "標準");
        RequestId(await f.Api.Send("PUT", "/api/profiles/{name}", body: Json(profile), routeValues: new { name = "改名" }));
        Assert.False(f.Profiles.ContainsKey("標準"));
        Assert.True(File.Exists(Path.Combine(f.Root, "profile", "改名.profile")));
        RequestId(await f.Api.Send("DELETE", "/api/profiles/{name}", routeValues: new { name = "改名" }));
        Assert.False(File.Exists(Path.Combine(f.Root, "profile", "改名.profile")));
        Assert.Same(preserved, Assert.Single(f.Profiles).Value);
    }

    [Fact]
    public async Task プロファイルの重複追加は上書きし重複リネームは既存を保つ()
    {
        await using var f = new RestFileSystemFixture();
        var p = ServerSupport.NormalizeProfile(null!);
        p.Name = "標準";
        RequestId(await f.Api.Send("POST", "/api/profiles", body: Json(p)));
        p.X264Option = "更新";
        RequestId(await f.Api.Send("POST", "/api/profiles", body: Json(p)));
        Assert.Equal("更新", Assert.Single(f.Profiles).Value.X264Option);
        p.Name = "既存";
        RequestId(await f.Api.Send("POST", "/api/profiles", body: Json(p)));
        var before = f.Profiles.ToDictionary(pair => pair.Key, pair => pair.Value);
        p.Name = "標準";
        var id = RequestId(await f.Api.Send("PUT", "/api/profiles/{name}", body: Json(p), routeValues: new { name = "既存" }));
        Assert.True(f.Api.Queue.Store.TryGetMessageForRequestId(id, out var message));
        Assert.Equal("error", message.Level);
        Assert.Equal(2, f.Profiles.Count);
        foreach (var pair in before) Assert.Same(pair.Value, f.Profiles[pair.Key]);
    }

    [Theory]
    [InlineData("DELETE", "/api/profiles/{name}")]
    [InlineData("DELETE", "/api/autoselect/{name}")]
    public async Task 未登録名の削除は別の設定へ影響しない(string method, string path)
    {
        await using var f = new RestFileSystemFixture();
        RequestId(await f.Api.Send(method, path, routeValues: new { name = "未登録" }));
        Assert.Empty(f.Profiles);
        Assert.Empty(f.AutoSelects);
        Assert.Empty(Directory.GetFiles(Path.Combine(f.Root, "profile")));
    }

    [Fact]
    public async Task 無効なプロファイル更新は保存済み設定とDTOを保つ()
    {
        await using var f = new RestFileSystemFixture();
        var profile = ServerSupport.NormalizeProfile(null!);
        profile.Name = "標準";
        RequestId(await f.Api.Send("POST", "/api/profiles", body: Json(profile)));
        var before = f.Profiles[profile.Name];
        var file = Path.Combine(f.Root, "profile", "標準.profile");
        var content = File.ReadAllBytes(file);
        f.Data.setting.X264Path = Path.Combine(f.Root, "未登録エンコーダ");
        profile.X264Option = "変更";
        var id = RequestId(await f.Api.Send("PUT", "/api/profiles/{name}", body: Json(profile), routeValues: new { name = profile.Name }));
        Assert.True(f.Api.Queue.Store.TryGetMessageForRequestId(id, out var message));
        Assert.Equal("error", message.Level);
        Assert.Same(before, f.Profiles[profile.Name]);
        Assert.Equal(content, File.ReadAllBytes(file));
        Assert.Equal(before.X264Option, Assert.Single(f.Api.Queue.Store.GetProfiles()).X264Option);
    }

    [Fact]
    public async Task プロファイル更新は再帰的にExtensionDataを除去する()
    {
        await using var f = new RestFileSystemFixture();
        var profile = ServerSupport.NormalizeProfile(null!);
        profile.Name = "標準";
        var node = System.Text.Json.Nodes.JsonNode.Parse(Json(profile))!;
        node["extensionData"] = 123;
        node["filterSetting"]!["ExtensionData"] = "互換情報";
        RequestId(await f.Api.Send("POST", "/api/profiles", body: node.ToJsonString()));
        Assert.Equal("標準", Assert.Single(f.Profiles).Value.Name);
    }

    [Theory]
    [InlineData("PUT", "/api/profiles/{name}")]
    [InlineData("PUT", "/api/autoselect/{name}")]
    public async Task 未登録名への通常更新は追加として扱う(string method, string path)
    {
        await using var f = new RestFileSystemFixture();
        object data;
        if (path.Contains("profiles"))
        {
            var profile = ServerSupport.NormalizeProfile(null!);
            profile.Name = "未登録";
            data = profile;
        }
        else data = new AutoSelectProfile { Name = "未登録", Conditions = new() };
        RequestId(await f.Api.Send(method, path, body: Json(data), routeValues: new { name = "未登録" }));
        Assert.Equal(1, f.Profiles.Count + f.AutoSelects.Count);
        Assert.True(f.Profiles.ContainsKey("未登録") || f.AutoSelects.ContainsKey("未登録"));
    }

    [Fact]
    public async Task 自動選択は追加重複更新リネーム削除とDTOを同期する()
    {
        await using var f = new RestFileSystemFixture();
        var p = new AutoSelectProfile { Name = "選択", Conditions = new() };
        RequestId(await f.Api.Send("POST", "/api/autoselect", body: Json(p)));
        RequestId(await f.Api.Send("POST", "/api/autoselect", body: Json(p)));
        Assert.Single(f.AutoSelects);
        RequestId(await f.Api.Send("PUT", "/api/autoselect/{name}", body: Json(p), routeValues: new { name = p.Name }));
        using (var json = JsonDocument.Parse((await f.Api.Send("GET", "/api/autoselect")).Body))
            Assert.Equal("選択", Assert.Single(json.RootElement.EnumerateArray()).GetProperty("name").GetString());
        RequestId(await f.Api.Send("PUT", "/api/autoselect/{name}", body: Json(p), routeValues: new { name = "改名" }));
        Assert.Equal("改名", Assert.Single(f.AutoSelects).Value.Name);
        RequestId(await f.Api.Send("DELETE", "/api/autoselect/{name}", routeValues: new { name = "改名" }));
        Assert.Empty(f.AutoSelects);
    }

    [Fact]
    public async Task 自動選択は未登録プロファイルの参照を拒否して既存を保つ()
    {
        await using var f = new RestFileSystemFixture();
        var original = new AutoSelectProfile { Name = "選択", Conditions = new() };
        RequestId(await f.Api.Send("POST", "/api/autoselect", body: Json(original)));
        var before = f.AutoSelects[original.Name];
        var invalid = new AutoSelectProfile { Name = original.Name, Conditions = new() { new() { Profile = "未登録" } } };
        var id = RequestId(await f.Api.Send("PUT", "/api/autoselect/{name}", body: Json(invalid), routeValues: new { name = original.Name }));
        Assert.True(f.Api.Queue.Store.TryGetMessageForRequestId(id, out var message));
        Assert.Equal("error", message.Level);
        Assert.Same(before, f.AutoSelects[original.Name]);
    }

    [Fact]
    public async Task 共通設定更新は状態ストアへ反映し無効な作業パスは既存を保つ()
    {
        await using var f = new RestFileSystemFixture();
        var setting = f.CreateSetting();
        setting.EnableShutdownAction = true;
        var id = RequestId(await f.Api.Send("PUT", "/api/settings", body: Json(setting)));
        Assert.True(f.Api.Queue.Store.TryGetMessageForRequestId(id, out var success));
        Assert.Equal("info", success.Level);
        Assert.True(f.Data.setting.EnableShutdownAction);
        using (var json = JsonDocument.Parse((await f.Api.Send("GET", "/api/settings")).Body))
            Assert.True(json.RootElement.GetProperty("enableShutdownAction").GetBoolean());
        var before = f.Data.setting;
        setting.WorkPath = Path.Combine(f.Root, "存在しない作業先");
        id = RequestId(await f.Api.Send("PUT", "/api/settings", body: Json(setting)));
        Assert.True(f.Api.Queue.Store.TryGetMessageForRequestId(id, out var error));
        Assert.Equal("error", error.Level);
        Assert.Same(before, f.Data.setting);
    }

    [Fact]
    public async Task スクリプトと完了設定は独立して更新される()
    {
        await using var f = new RestFileSystemFixture();
        var setting = f.Data.setting;
        RequestId(await f.Api.Send("PUT", "/api/makescript", body: Json(new MakeScriptData { Profile = "標準", OutDir = f.Root, Priority = 4 })));
        Assert.Equal(4, f.Data.scriptData.Priority);
        using (var json = JsonDocument.Parse((await f.Api.Send("GET", "/api/makescript")).Body))
            Assert.Equal("標準", json.RootElement.GetProperty("profile").GetString());
        RequestId(await f.Api.Send("PUT", "/api/finish-setting", body: Json(new FinishSetting { Action = FinishAction.None, Seconds = 123 })));
        Assert.Equal(123, f.Data.finishSetting.Seconds);
        Assert.Equal(4, f.Data.scriptData.Priority);
        Assert.Same(setting, f.Data.setting);
    }

    [Theory]
    [InlineData("PUT", "/api/service-settings/{serviceId}/logos/period", "{}", 400)]
    [InlineData("PUT", "/api/service-settings/{serviceId}/logos/enabled", "null", 400)]
    [InlineData("DELETE", "/api/service-settings/{serviceId}/logos", "{}", 400)]
    [InlineData("PUT", "/api/service-settings/{serviceId}/logos/period", "{\"fileName\":\"未登録\"}", 404)]
    [InlineData("PUT", "/api/service-settings/{serviceId}/logos/enabled", "{\"fileName\":\"未登録\"}", 404)]
    [InlineData("DELETE", "/api/service-settings/{serviceId}/logos", "{\"fileName\":\"未登録\"}", 404)]
    [InlineData("DELETE", "/api/service-settings/{serviceId}/logos/no-logo", "", 404)]
    public async Task サービス更新は必須入力不足と未知IDを拒否する(string method, string path, string body, int expected)
    {
        await using var f = new RestFileSystemFixture();
        Assert.Equal(expected, (await f.Api.Send(method, path, body: body, routeValues: new { serviceId = 42 })).Status);
        Assert.Empty(f.Data.services.ServiceMap);
    }

    [Theory]
    [InlineData("PUT", "/api/service-settings/{serviceId}/logos/period")]
    [InlineData("PUT", "/api/service-settings/{serviceId}/logos/enabled")]
    [InlineData("DELETE", "/api/service-settings/{serviceId}/logos")]
    public async Task 未登録ロゴへの操作は既存サービス設定を保つ(string method, string path)
    {
        await using var f = new RestFileSystemFixture();
        var service = new ServiceSettingElement { ServiceId = 42, ServiceName = "放送局", LogoSettings = new() { new() { FileName = LogoSetting.NO_LOGO } } };
        RequestId(await f.Api.Send("POST", "/api/services/update", body: Json(new ServiceSettingUpdate { Type = ServiceSettingUpdateType.Update, ServiceId = 42, Data = service })));
        var before = f.Data.services.ServiceMap[42];
        Assert.Equal(404, (await f.Api.Send(method, path, body: "{\"fileName\":\"未登録\"}", routeValues: new { serviceId = 42 })).Status);
        Assert.Same(before, f.Data.services.ServiceMap[42]);
        Assert.Equal(LogoSetting.NO_LOGO, Assert.Single(before.LogoSettings).FileName);
    }

    [Fact]
    public async Task ロゴ削除の不正なファイル名は設定とファイルを保つ()
    {
        await using var f = new RestFileSystemFixture();
        const string invalid = "../外部.lgd";
        var service = new ServiceSettingElement { ServiceId = 42, ServiceName = "放送局", LogoSettings = new() { new() { FileName = invalid } } };
        RequestId(await f.Api.Send("POST", "/api/services/update", body: Json(new ServiceSettingUpdate { Type = ServiceSettingUpdateType.Update, ServiceId = 42, Data = service })));
        var sentinel = Path.Combine(f.Root, "外部.lgd");
        File.WriteAllText(sentinel, "保持");
        Assert.Equal(500, (await f.Api.Send("DELETE", "/api/service-settings/{serviceId}/logos", body: Json(new { fileName = invalid }), routeValues: new { serviceId = 42 })).Status);
        Assert.Equal("保持", File.ReadAllText(sentinel));
        Assert.Equal(invalid, Assert.Single(f.Data.services.ServiceMap[42].LogoSettings).FileName);
    }

    [Fact]
    public async Task サービスとロゴの更新削除は別サービスを保つ()
    {
        await using var f = new RestFileSystemFixture();
        var service = new ServiceSettingElement { ServiceId = 42, ServiceName = "放送局", LogoSettings = new() {
            new() { FileName = "test.lgd", Enabled = true, Exists = true, ServiceId = 42 }, } };
        RequestId(await f.Api.Send("POST", "/api/services/update", body: Json(new ServiceSettingUpdate { Type = ServiceSettingUpdateType.Update, ServiceId = 42, Data = service })));
        var other = new ServiceSettingElement { ServiceId = 43, ServiceName = "保持", LogoSettings = new() };
        RequestId(await f.Api.Send("POST", "/api/services/update", body: Json(new ServiceSettingUpdate { Type = ServiceSettingUpdateType.Update, ServiceId = 43, Data = other })));
        var preserved = f.Data.services.ServiceMap[43];
        RequestId(await f.Api.Send("PUT", "/api/service-settings/{serviceId}/logos/enabled", body: "{\"fileName\":\"test.lgd\",\"enabled\":false}", routeValues: new { serviceId = 42 }));
        Assert.False(f.Data.services.ServiceMap[42].LogoSettings[0].Enabled);
        Assert.True(f.Data.services.ServiceMap[42].LogoSettings[0].Exists);
        RequestId(await f.Api.Send("PUT", "/api/service-settings/{serviceId}/logos/period", body: "{\"fileName\":\"test.lgd\",\"from\":\"2020-01-01T00:00:00\",\"to\":\"2030-01-01T00:00:00\"}", routeValues: new { serviceId = 42 }));
        Assert.Equal(new DateTime(2030, 1, 1), f.Data.services.ServiceMap[42].LogoSettings[0].To);
        File.WriteAllText(Path.Combine(f.Root, "logo", "test.lgd"), "テスト用");
        RequestId(await f.Api.Send("DELETE", "/api/service-settings/{serviceId}/logos", body: "{\"fileName\":\"test.lgd\"}", routeValues: new { serviceId = 42 }));
        Assert.False(File.Exists(Path.Combine(f.Root, "logo", "test.lgd")));
        Assert.Empty(f.Data.services.ServiceMap[42].LogoSettings);
        RequestId(await f.Api.Send("POST", "/api/service-settings/{serviceId}/logos/no-logo", routeValues: new { serviceId = 42 }));
        Assert.Equal(LogoSetting.NO_LOGO, Assert.Single(f.Data.services.ServiceMap[42].LogoSettings).FileName);
        // 実ロゴの寸法取得はネイティブ依存なので、ロゴなし設定でDTOを確認する。
        using (var json = JsonDocument.Parse((await f.Api.Send("GET", "/api/service-settings")).Body))
            Assert.Contains(json.RootElement.EnumerateArray(), s => s.GetProperty("serviceId").GetInt32() == 42);
        RequestId(await f.Api.Send("DELETE", "/api/service-settings/{serviceId}/logos/no-logo", routeValues: new { serviceId = 42 }));
        Assert.Empty(f.Data.services.ServiceMap[42].LogoSettings);
        Assert.Same(preserved, f.Data.services.ServiceMap[43]);
    }
}
