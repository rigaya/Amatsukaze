using System.Reflection;
using System.Text;
using Amatsukaze.Server;
using Amatsukaze.Server.Rest;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Routing;
using Xunit;

namespace AmatsukazeServerTest;

internal sealed class RestEndpointTestFixture : IAsyncDisposable
{
    public RestQueueTestFixture Queue { get; } = new();
    private readonly WebApplication app;
    private readonly RestApiHost host;
    public IReadOnlyList<RouteEndpoint> Endpoints => ((IEndpointRouteBuilder)app).DataSources
        .SelectMany(source => source.Endpoints).OfType<RouteEndpoint>().ToList();

    public RestEndpointTestFixture()
    {
        host = Queue.CreateRestHost();
        try
        {
            // 本番登録とJSON設定を使う。待受けを開始せずHTTP接続も作らない。
            app = (WebApplication)typeof(RestApiHost).GetMethod("BuildWebApp", BindingFlags.Instance | BindingFlags.NonPublic)!
                .Invoke(host, new object[] { 0 })!;
        }
        catch { host.Dispose(); throw; }
    }

    public async Task<(int Status, string Body)> Send(string method, string path, string query = "", string? body = null,
        object? routeValues = null, string contentType = "application/json")
    {
        var endpoint = Assert.Single(Endpoints, e => e.RoutePattern.RawText == path &&
            e.Metadata.GetMetadata<HttpMethodMetadata>()?.HttpMethods.Contains(method) == true);
        var context = new DefaultHttpContext { RequestServices = app.Services };
        context.Request.Method = method;
        context.Request.Path = path;
        // 実際のルーターと同じく、数値パラメータも文字列としてバインダーへ渡す。
        context.Request.RouteValues = new RouteValueDictionary(new RouteValueDictionary(routeValues)
            .ToDictionary(pair => pair.Key, pair => (object?)Convert.ToString(pair.Value, System.Globalization.CultureInfo.InvariantCulture)));
        context.Request.QueryString = new QueryString(query);
        using var input = new MemoryStream(Encoding.UTF8.GetBytes(body ?? ""));
        using var output = new MemoryStream();
        context.Request.Body = input;
        context.Request.ContentType = contentType;
        context.Request.ContentLength = input.Length;
        context.Response.Body = output;
        context.SetEndpoint(endpoint);
        await endpoint.RequestDelegate!(context);
        return (context.Response.StatusCode, Encoding.UTF8.GetString(output.ToArray()));
    }

    public async ValueTask DisposeAsync()
    {
        try { await app.DisposeAsync(); }
        finally { host.Dispose(); }
    }
}

[CollectionDefinition("REST一時保存先", DisableParallelization = true)]
public sealed class RestFileSystemCollection { }

// 相対パスを使う本番保存処理を、このfixtureだけの一時ディレクトリへ閉じ込める。
// 作業ディレクトリはプロセス全体に影響するので、使用する試験は上記コレクションへ所属させる。
internal sealed class RestFileSystemFixture : IAsyncDisposable
{
    private readonly string previousDirectory;
    private PauseScheduler? pauseScheduler;
    public string Root { get; }
    public RestEndpointTestFixture Api { get; }
    public EncodeServer.AppData Data => Api.Queue.Server.AppData_;
    public Dictionary<string, ProfileSetting> Profiles { get; } = new(StringComparer.OrdinalIgnoreCase);
    public Dictionary<string, AutoSelectProfile> AutoSelects { get; } = new(StringComparer.OrdinalIgnoreCase);

    public RestFileSystemFixture()
    {
        previousDirectory = Directory.GetCurrentDirectory();
        Root = Path.Combine(Path.GetTempPath(), "amt-rest-api-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(Root);
        Directory.SetCurrentDirectory(Root);
        try
        {
            foreach (var name in new[] { "profile", "config", "logo", "drcs", "data", "work" })
                Directory.CreateDirectory(Path.Combine(Root, name));
            Api = new RestEndpointTestFixture();
            var server = Api.Queue.Server;
            var data = new EncodeServer.AppData
            {
                setting = CreateSetting(), scriptData = new MakeScriptData(), finishSetting = new FinishSetting(),
                services = new ServiceSetting { ServiceMap = new() },
            };
            // 設定更新に必要なメモリ内のAppData、辞書、資源管理だけを追加する。保存スレッドは起動しない。
            RestQueueTestFixture.SetProperty(server, "AppData_", data);
            RestQueueTestFixture.SetProperty(server, "ResourceManager", new ResourceManager());
            RestQueueTestFixture.SetField(server, "profiles", Profiles);
            RestQueueTestFixture.SetField(server, "autoSelects", AutoSelects);
            var pool = (WorkerPool)RestQueueTestFixture.GetField(server, "workerPool");
            pauseScheduler = new PauseScheduler(server, pool);
            RestQueueTestFixture.SetField(server, "pauseScheduler", pauseScheduler);
            RestQueueTestFixture.SetField(server, "drcsManager", new DRCSManager(server));
            Api.Queue.Store.OnCommonData(new CommonData { Setting = data.setting, MakeScriptData = data.scriptData, FinishSetting = data.finishSetting }).GetAwaiter().GetResult();
        }
        catch
        {
            pauseScheduler?.Complete();
            Api?.DisposeAsync().AsTask().GetAwaiter().GetResult();
            Directory.SetCurrentDirectory(previousDirectory);
            Directory.Delete(Root, true);
            throw;
        }
    }

    // DRCS更新は既存画像の辞書を必要とする。画像ロードやネイティブ処理を使わず通知だけを初期化する。
    public Task SeedDrcs(DrcsImage image)
    {
        var manager = RestQueueTestFixture.GetField(Api.Queue.Server, "drcsManager");
        RestQueueTestFixture.SetField(manager, "drcsMap", new Dictionary<string, DrcsImage> { [image.MD5] = image });
        return Api.Queue.Store.OnDrcsData(new DrcsImageUpdate { Type = DrcsUpdateType.Update, Image = image });
    }

    public Setting CreateSetting()
    {
        var tool = Path.Combine(Root, "tool");
        File.WriteAllText(tool, "テスト用。実行しない。");
        var setting = new Setting();
        // SetDefaultPathによる実環境の実行ファイル探索を避け、すべての指定パスを一時領域に向ける。
        foreach (var property in typeof(Setting).GetProperties().Where(p => p.CanWrite && p.PropertyType == typeof(string) && p.Name.EndsWith("Path")))
            property.SetValue(setting, tool);
        setting.WorkPath = Path.Combine(Root, "work");
        setting.NumParallel = 0;
        setting.NumGPU = 1;
        setting.MaxGPUResources = Enumerable.Repeat(100, ResourceManager.MAX_GPU).ToArray();
        setting.RunHours = Enumerable.Repeat(true, 24).ToArray();
        return setting;
    }

    public async ValueTask DisposeAsync()
    {
        try { pauseScheduler?.Complete(); await Api.DisposeAsync(); }
        finally
        {
            Directory.SetCurrentDirectory(previousDirectory);
            Directory.Delete(Root, true);
        }
    }
}
