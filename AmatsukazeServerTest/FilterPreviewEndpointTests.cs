using System.Globalization;
using System.Runtime.Versioning;
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

    [Theory]
    [InlineData(FilterOption.VCEEncFilter, "VCEEnc", null)]
    [InlineData(FilterOption.VCEEncFilter, "VCEEnc", "")]
    [InlineData(FilterOption.VCEEncFilter, "VCEEnc", "missing")]
    [InlineData(FilterOption.QSVEncFilter, "QSVEnc", "missing")]
    [InlineData(FilterOption.NVEncFilter, "NVEnc", "missing")]
    public async Task 利用できないエンコーダのパスは設定の受信時に拒否する(FilterOption option, string encoder, string? name)
    {
        using var peer = new FilterPreviewViewTests.Peer();
        await using var fixture = CreateFixture(peer);
        var path = string.IsNullOrEmpty(name) ? name : Path.Combine(fixture.Root, name);
        switch (option)
        {
            case FilterOption.QSVEncFilter: fixture.Data.setting.QSVEncPath = path; break;
            case FilterOption.NVEncFilter: fixture.Data.setting.NVEncPath = path; break;
            case FilterOption.VCEEncFilter: fixture.Data.setting.VCEEncPath = path; break;
        }
        await fixture.Api.Queue.Store.OnCommonData(new CommonData { Setting = fixture.Data.setting });
        var sessionId = await CreateSessionAsync(fixture, peer, MakeProfile("元"));
        await ConfigureAsync(fixture, peer, sessionId, 1, "None");

        using var cancel = new CancellationTokenSource();
        var attemptedForwarding = peer.NextCommandAsync("config", cancel.Token);
        try
        {
            var body = JsonSerializer.Serialize(new { gen = 2, filterOption = option.ToString() });
            var request = fixture.Api.Send("PUT", Session + "/config", body: body, routeValues: new { sessionId });
            Assert.Same(request, await Task.WhenAny(request, attemptedForwarding));
            var result = await request;
            Assert.Equal(400, result.Status);
            using var error = JsonDocument.Parse(result.Body);
            Assert.Equal(string.IsNullOrEmpty(path)
                ? $"{encoder}のパスが設定されていません(設定画面で指定してください)"
                : $"{encoder}の実行ファイルが見つかりません: {path}",
                error.RootElement.GetProperty("message").GetString());
        }
        finally { cancel.Cancel(); }
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => attemptedForwarding);

        // 拒否した設定はエンジンの世代を進めず、同じセッションで別の設定を選べる。
        await ConfigureAsync(fixture, peer, sessionId, 2, "None");
    }

    [LinuxFact]
    [SupportedOSPlatform("linux")]
    public async Task PATHにあるエンコーダ名を設定で受け付ける()
    {
        using var peer = new FilterPreviewViewTests.Peer();
        await using var fixture = CreateFixture(peer);
        var tools = Path.Combine(fixture.Root, "自動検索先");
        Directory.CreateDirectory(tools);
        var executable = Path.Combine(tools, "vceencc");
        File.WriteAllText(executable, "");
        File.SetUnixFileMode(executable, UnixFileMode.UserRead | UnixFileMode.UserExecute);
        fixture.Data.setting.VCEEncPath = "vceencc";
        await fixture.Api.Queue.Store.OnCommonData(new CommonData { Setting = fixture.Data.setting });
        var previousPath = Environment.GetEnvironmentVariable("PATH");
        try
        {
            Environment.SetEnvironmentVariable("PATH", tools);
            var sessionId = await CreateSessionAsync(fixture, peer, MakeProfile("元"));
            await ConfigureAsync(fixture, peer, sessionId, 1, "VCEEncFilter");
        }
        finally { Environment.SetEnvironmentVariable("PATH", previousPath); }
    }

    private static ProfileSetting MakeProfile(string name)
    {
        var profile = ServerSupport.NormalizeProfile(null!);
        profile.Name = name;
        profile.X264Option = "元のエンコーダ設定";
        return profile;
    }

    [Fact]
    public async Task 素材一覧は一時データが残る完了済みの候補を新しい順に返す()
    {
        await using var fixture = new RestFileSystemFixture();
        var start = new DateTime(2026, 1, 2, 3, 4, 5);
        var items = new List<QueueItem>();
        foreach (var (id, mode, state, hasDat) in new[]
        {
            (1, ProcMode.Batch, QueueState.Complete, true),
            (2, ProcMode.FilterAdjust, QueueState.Complete, true),
            (3, ProcMode.CMCheck, QueueState.Complete, false),
            (4, ProcMode.FilterAdjust, QueueState.Encoding, true),
            (5, ProcMode.Test, QueueState.Complete, true),
            (6, ProcMode.FilterAdjust, QueueState.Complete, false),
            (7, ProcMode.Batch, QueueState.Complete, true),
        })
        {
            var item = RestQueueTestFixture.Item(id, state);
            item.Mode = mode;
            item.EncodeStart = start.AddSeconds(id);
            item.EncodeTime = TimeSpan.FromSeconds(10);
            item.SrcPath = Path.Combine(fixture.Root, $"素材{id}.ts");
            item.Profile = MakeProfile($"作成時{id}");
            // 通常のエンコードは一時フォルダを残す設定のものだけが候補になる。7は設定がないので除く。
            item.Profile.NoRemoveTmp = id == 1;
            if (id != 6)
            {
                var temp = Path.Combine(fixture.Root, "work", $"amt-{id}");
                Directory.CreateDirectory(temp);
                if (hasDat) File.WriteAllBytes(Path.Combine(temp, "amts0.dat"), [0]);
                var server = fixture.Api.Queue.Server;
                var log = (item.IsCheck ? server.GetCheckLogFileBase(item.EncodeStart) : server.GetLogFileBase(item.EncodeStart)) + ".txt";
                Directory.CreateDirectory(Path.GetDirectoryName(log)!);
                File.WriteAllText(log, "一時フォルダ: " + temp, Util.AmatsukazeDefaultEncoding);
            }
            items.Add(item);
        }
        await fixture.Api.Queue.Seed(items.ToArray());

        var result = await fixture.Api.Send("GET", "/api/filter-preview/sources");
        Assert.Equal(200, result.Status);
        using var json = JsonDocument.Parse(result.Body);
        var rows = json.RootElement.EnumerateArray().ToArray();
        Assert.Equal(new[] { 2, 1 }, rows.Select(row => row.GetProperty("queueItemId").GetInt32()));
        Assert.Equal("素材2.ts", rows[0].GetProperty("fileName").GetString());
        Assert.Equal("FilterAdjust", rows[0].GetProperty("mode").GetString());
        Assert.Equal("作成時2", rows[0].GetProperty("profileName").GetString());
    }

    [Fact]
    public async Task 指定された別のプロファイルを初期値と上書き先とコピー元に使う()
    {
        using var peer = new FilterPreviewViewTests.Peer();
        await using var fixture = CreateFixture(peer);
        var original = MakeProfile("録画時");
        var selected = MakeProfile("選択中");
        selected.X264Option = "コピー元の設定";
        selected.FilterOption = FilterOption.Custom;
        selected.FilterPath = "選択中のフィルタ.avs";
        fixture.Profiles.Add(original.Name, original);
        fixture.Profiles.Add(selected.Name, selected);
        var sessionId = await CreateSessionAsync(fixture, peer, original, selected.Name);
        var route = new { sessionId };
        var config = await fixture.Api.Send("GET", Session + "/config", routeValues: route);
        Assert.Equal(200, config.Status);
        using var json = JsonDocument.Parse(config.Body);
        Assert.Equal(selected.Name, json.RootElement.GetProperty("name").GetString());
        Assert.Equal("Custom", json.RootElement.GetProperty("filterOption").GetString());

        await ConfigureAsync(fixture, peer, sessionId, 1, "None");
        var copy = await fixture.Api.Send("POST", Session + "/save",
            body: """{"saveAs":true,"name":"別名","preserveCustomFilter":true}""", routeValues: route);
        Assert.Equal(200, copy.Status);
        Assert.Equal(selected.X264Option, fixture.Profiles["別名"].X264Option);
        Assert.Equal(selected.FilterPath, fixture.Profiles["別名"].FilterPath);
        Assert.Equal(FilterOption.Custom, fixture.Profiles["別名"].FilterOption);

        await ConfigureAsync(fixture, peer, sessionId, 2, "QSVEncFilter");
        var overwrite = await fixture.Api.Send("POST", Session + "/save",
            body: """{"saveAs":false}""", routeValues: route);
        Assert.Equal(200, overwrite.Status);
        Assert.Equal(FilterOption.QSVEncFilter, fixture.Profiles[selected.Name].FilterOption);
        Assert.Equal(FilterOption.None, fixture.Profiles[original.Name].FilterOption);
    }

    [Fact]
    public async Task 存在しないプロファイル名でセッションを開くと400になる()
    {
        await using var fixture = new RestFileSystemFixture();
        var result = await fixture.Api.Send("POST", Root, body: """{"queueItemId":123,"profileName":"存在しない"}""");
        Assert.Equal(400, result.Status);
        using var json = JsonDocument.Parse(result.Body);
        Assert.Contains("指定されたプロファイル", json.RootElement.GetProperty("message").GetString());
    }

    private static RestFileSystemFixture CreateFixture(FilterPreviewViewTests.Peer peer) =>
        new(state => new FilterPreviewService(state, _ => new FilterPreviewSourceFormat(),
            createEngine: (_, _) => peer.Engine));

    private static async Task<string> CreateSessionAsync(RestFileSystemFixture fixture, FilterPreviewViewTests.Peer peer, ProfileSetting profile, string? profileName = null)
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

        var body = profileName == null ? """{"queueItemId":123}""" : JsonSerializer.Serialize(new { queueItemId = 123, profileName });
        var response = await fixture.Api.Send("POST", Root, body: body);
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
        using var cancel = new CancellationTokenSource();
        var pendingCommand = peer.NextCommandAsync("config", cancel.Token);
        if (await Task.WhenAny(response, pendingCommand) == response)
        {
            cancel.Cancel();
            var failed = await response;
            Assert.Fail($"設定がエンジンへ送られませんでした。HTTP {failed.Status} {failed.Body}");
        }
        var command = await pendingCommand;
        Assert.Equal(gen, command.GetProperty("gen").GetInt64());
        var kind = command.GetProperty("kind").GetString();
        await peer.SendTextAsync($"AMTPREVIEW INFO gen={gen} kind={kind} frames=4 width=2 height=2\n");
        Assert.Equal(200, (await response).Status);
    }
}
