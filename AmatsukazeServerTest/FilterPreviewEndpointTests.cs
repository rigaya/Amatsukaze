using System.Globalization;
using System.Text.Json;
using Amatsukaze.Lib;
using Amatsukaze.Server;
using Amatsukaze.Server.Rest;
using Xunit;

namespace AmatsukazeServerTest;

[Collection("REST一時保存先")]
public class FilterPreviewEndpointTests
{
    private const string Root = "/api/filter-preview/sessions";
    private const string Session = Root + "/{sessionId}";

    [Theory]
    [InlineData("extensionData")]
    [InlineData("ExtensionData")]
    public async Task 設定のXML拡張情報を含むJSONを読み込める(string key)
    {
        await using var fixture = new RestEndpointTestFixture();
        var body = JsonSerializer.Serialize(new
        {
            gen = 1, filterOption = "None",
            filterSetting = new Dictionary<string, object> { [key] = new { }, ["enableDeinterlace"] = true },
            encoderFilterSetting = new Dictionary<string, object> { [key] = new { } },
        });
        var result = await fixture.Send("PUT", "/api/filter-preview/sessions/{sessionId}/config", body: body,
            routeValues: new { sessionId = "存在しないセッション" });
        // JSONの読込みを完了してセッション検索へ進むため、JSON例外ではなく404になる。
        Assert.Equal(404, result.Status);
    }

    [Fact]
    public async Task 指定したプロファイルのフィルタだけを上書きする()
    {
        using var peer = new FilterPreviewViewTests.Peer();
        await using var fixture = CreateFixture(peer);
        var original = MakeProfile("元");
        var target = MakeProfile("Target");
        target.X264Option = "上書き先のエンコーダ設定";
        target.RenameFormat = "上書き先の出力書式";
        target.FilterPath = "上書き先のメイン.avs";
        target.PostFilterPath = "上書き先のポスト.avs";
        fixture.Profiles.Add(original.Name, original);
        fixture.Profiles.Add(target.Name, target);
        var sessionId = await CreateSessionAsync(fixture, peer, original);
        var config = await fixture.Api.Send("GET", Session + "/config", routeValues: new { sessionId });
        Assert.Equal(200, config.Status);
        using var configJson = JsonDocument.Parse(config.Body);
        Assert.Equal("元", configJson.RootElement.GetProperty("name").GetString());

        await ConfigureAsync(fixture, peer, sessionId, 1, "QSVEncFilter");
        var result = await fixture.Api.Send("POST", Session + "/save",
            body: """{"saveAs":false,"target":"target"}""", routeValues: new { sessionId });
        Assert.Equal(200, result.Status);
        var saved = fixture.Profiles[target.Name];
        Assert.Equal(target.Name, saved.Name);
        Assert.Equal(target.X264Option, saved.X264Option);
        Assert.Equal(target.RenameFormat, saved.RenameFormat);
        Assert.Equal(target.FilterPath, saved.FilterPath);
        Assert.Equal(target.PostFilterPath, saved.PostFilterPath);
        Assert.Equal(FilterOption.QSVEncFilter, saved.FilterOption);
        Assert.True(saved.FilterSetting.EnableResize);
        Assert.True(saved.EncoderFilterSetting.EnableDenoise);
        Assert.Equal("--vpp-knn", saved.QSVEncFilterOption);
        Assert.Equal("--vpp-deband", saved.NVEncFilterOption);
        Assert.Equal("--vpp-resize", saved.VCEEncFilterOption);
        Assert.Same(original, fixture.Profiles[original.Name]);
        Assert.Equal(FilterOption.None, original.FilterOption);
        Assert.Equal("Target.profile", Path.GetFileName(Assert.Single(Directory.GetFiles(Path.Combine(fixture.Root, "profile")))));

        var oldRequest = await fixture.Api.Send("POST", Session + "/save",
            body: """{"saveAs":false}""", routeValues: new { sessionId });
        Assert.Equal(200, oldRequest.Status);
        Assert.Equal(FilterOption.QSVEncFilter, fixture.Profiles[original.Name].FilterOption);

        var missing = await fixture.Api.Send("POST", Session + "/save",
            body: """{"saveAs":false,"target":"存在しない上書き先"}""", routeValues: new { sessionId });
        Assert.Equal(400, missing.Status);
        using var error = JsonDocument.Parse(missing.Body);
        Assert.Contains("存在しない上書き先", error.RootElement.GetProperty("message").GetString());
        Assert.False(fixture.Profiles.ContainsKey("存在しない上書き先"));

        var copy = await fixture.Api.Send("POST", Session + "/save",
            body: """{"saveAs":true,"name":"別名","target":"Target"}""", routeValues: new { sessionId });
        Assert.Equal(200, copy.Status);
        Assert.Equal(original.X264Option, fixture.Profiles["別名"].X264Option);
        Assert.Equal(FilterOption.QSVEncFilter, fixture.Profiles["別名"].FilterOption);
    }

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public async Task 元のプロファイルが一覧になくても選んだ上書き先に保存できる(bool preserveCustom)
    {
        using var peer = new FilterPreviewViewTests.Peer();
        await using var fixture = CreateFixture(peer);
        var original = MakeProfile("元");
        original.FilterOption = FilterOption.Custom;
        original.FilterPath = "元のメイン.avs";
        original.PostFilterPath = "元のポスト.avs";
        var target = MakeProfile("上書き先");
        target.FilterPath = "先のメイン.avs";
        target.PostFilterPath = "先のポスト.avs";
        fixture.Profiles.Add(target.Name, target);
        var sessionId = await CreateSessionAsync(fixture, peer, original);
        await ConfigureAsync(fixture, peer, sessionId, 1, "None");

        var result = await fixture.Api.Send("POST", Session + "/save",
            body: JsonSerializer.Serialize(new { saveAs = false, target = target.Name, preserveCustomFilter = preserveCustom }),
            routeValues: new { sessionId });

        Assert.Equal(200, result.Status);
        var saved = fixture.Profiles[target.Name];
        Assert.Equal(preserveCustom ? FilterOption.Custom : FilterOption.None, saved.FilterOption);
        Assert.Equal(preserveCustom ? original.FilterPath : target.FilterPath, saved.FilterPath);
        Assert.Equal(preserveCustom ? original.PostFilterPath : target.PostFilterPath, saved.PostFilterPath);
        Assert.False(fixture.Profiles.ContainsKey(original.Name));
    }

    [Fact]
    public async Task 古い縮小指定を無視して両経路とも原寸を要求する()
    {
        using var peer = new FilterPreviewViewTests.Peer();
        await using var fixture = CreateFixture(peer);
        var sessionId = await CreateSessionAsync(fixture, peer, MakeProfile("元"));
        await ConfigureAsync(fixture, peer, sessionId, 1, "None");
        var frameResponse = fixture.Api.Send("POST", Session + "/frame",
            body: """{"gen":1,"pos":0,"scale":2,"format":"jpeg"}""", routeValues: new { sessionId });
        var frame = await peer.NextCommandAsync("frame");
        var frameId = frame.GetProperty("id").GetInt64();
        await peer.SendTextAsync($"AMTPREVIEW FRAME id={frameId} gen=1 pos=0 format=jpeg width=2 height=2 bytes=4\nABCD");
        var frameResult = await frameResponse;
        Assert.Equal(200, frameResult.Status);
        Assert.Equal("ABCD", frameResult.Body);
        Assert.False(frame.TryGetProperty("scale", out _));
        var cached = await fixture.Api.Send("POST", Session + "/frame",
            body: """{"gen":1,"pos":0,"scale":99,"format":"jpeg"}""", routeValues: new { sessionId });
        Assert.Equal(200, cached.Status);
        Assert.Equal("ABCD", cached.Body);

        await ConfigureAsync(fixture, peer, sessionId, 2, "QSVEncFilter");
        var viewResponse = fixture.Api.Send("POST", Session + "/view",
            body: """{"gen":2,"op":"seek","seconds":1,"scale":99}""", routeValues: new { sessionId });
        var view = await peer.NextCommandAsync("view");
        var viewId = view.GetProperty("id").GetInt64();
        await peer.SendTextAsync($"AMTPREVIEW DONE id={viewId} gen=2 status=ok\n");
        var viewResult = await viewResponse;
        Assert.Equal(200, viewResult.Status);
        Assert.Contains("status=ok", viewResult.Body);
        Assert.False(view.TryGetProperty("scale", out _));
    }

    private static ProfileSetting MakeProfile(string name)
    {
        var profile = ServerSupport.NormalizeProfile(null!);
        profile.Name = name;
        profile.X264Option = "元のエンコーダ設定";
        return profile;
    }

    private static RestFileSystemFixture CreateFixture(FilterPreviewViewTests.Peer peer) =>
        new(state => new FilterPreviewService(state, _ => new FilterPreviewSourceFormat(),
            createEngine: (_, _) => peer.Engine));

    private static async Task<string> CreateSessionAsync(RestFileSystemFixture fixture, FilterPreviewViewTests.Peer peer, ProfileSetting profile)
    {
        var temp = Path.Combine(fixture.Root, "work", "temp");
        Directory.CreateDirectory(temp);
        File.WriteAllBytes(Path.Combine(temp, "amts0.dat"), [0]);
        var start = new DateTime(2026, 1, 2, 3, 4, 5);
        var logs = Path.Combine(fixture.Root, "data", "logs");
        Directory.CreateDirectory(logs);
        File.WriteAllText(Path.Combine(logs, start.ToString("yyyy-MM-dd_HHmmss.fff", CultureInfo.InvariantCulture) + ".txt"),
            "一時フォルダ: " + temp, Util.AmatsukazeDefaultEncoding);
        var item = RestQueueTestFixture.Item(123, QueueState.Complete);
        item.EncodeStart = start;
        item.Profile = profile;
        await fixture.Api.Queue.Seed(item);
        await peer.SendTextAsync("AMTPREVIEW READY version=2\n");

        var response = await fixture.Api.Send("POST", Root, body: """{"queueItemId":123}""");
        Assert.Equal(200, response.Status);
        using var json = JsonDocument.Parse(response.Body);
        return json.RootElement.GetProperty("sessionId").GetString()!;
    }

    private static async Task ConfigureAsync(RestFileSystemFixture fixture, FilterPreviewViewTests.Peer peer,
        string sessionId, long gen, string filterOption)
    {
        var body = JsonSerializer.Serialize(new
        {
            gen, filterOption, filterSetting = new { enableResize = true, resizeWidth = 1280, resizeHeight = 720 },
            encoderFilterSetting = new { enableDenoise = true },
            qsvEncFilterOption = "--vpp-knn", nvEncFilterOption = "--vpp-deband", vceEncFilterOption = "--vpp-resize",
        });
        var response = fixture.Api.Send("PUT", Session + "/config", body: body, routeValues: new { sessionId });
        var command = await peer.NextCommandAsync("config");
        var kind = command.GetProperty("kind").GetString();
        await peer.SendTextAsync($"AMTPREVIEW INFO gen={gen} kind={kind} frames=4 width=2 height=2\n");
        Assert.Equal(200, (await response).Status);
    }
}
