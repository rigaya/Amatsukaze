using System.IO.Pipelines;
using System.Net;
using System.Net.Http.Json;
using System.Text;
using System.Text.Json;
using System.Threading.Channels;
using Amatsukaze.Server.Rest;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Hosting.Server;
using Microsoft.AspNetCore.Hosting.Server.Features;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Xunit;

namespace AmatsukazeServerTest;

public class FilterPreviewViewTests
{
    [Fact]
    public async Task HTTPは完了前に暫定画像を送って確定画像と終了を順番に返す()
    {
        await using var host = CreateHost();
        using var peer = new Peer();
        await peer.ConfigureAsync(1);
        host.MapPost("/view", context => FilterPreviewViewEndpoint.WriteAsync(context,
            (deliver, token) => peer.Engine.StreamViewAsync(new() { ["op"] = "seek", ["seconds"] = 2.5 }, 1, deliver, token)));
        await host.StartAsync();
        using var client = CreateClient(host);
        using var request = new HttpRequestMessage(HttpMethod.Post, "/view") { Content = JsonContent.Create(new { }) };
        var responseTask = client.SendAsync(request, HttpCompletionOption.ResponseHeadersRead);
        var command = await peer.NextCommandAsync("view");
        Assert.Equal(2.5, command.GetProperty("seconds").GetDouble());
        long id = command.GetProperty("id").GetInt64();
        var early = Peer.View(id, 1, 1, provisional: true, [(byte)'\n', 2, 3]);
        await peer.SendAsync(early);
        using var response = await responseTask;
        response.EnsureSuccessStatusCode();
        Assert.Empty(response.Content.Headers.ContentEncoding);
        await using var body = await response.Content.ReadAsStreamAsync();
        // 確定応答の送信前に画像まで読めることを確認する。全体を貯める実装ではここで停止する。
        await AssertRecordAsync(body, early);
        var confirmed = Peer.View(id, 1, 2, provisional: false, [], reuse: true);
        await peer.SendAsync(confirmed);
        await AssertRecordAsync(body, confirmed);
        var final = Peer.View(id, 1, 3, provisional: false, [4, 5, 6]);
        var done = Encoding.UTF8.GetBytes($"AMTPREVIEW DONE id={id} gen=1 status=ok\n");
        await peer.SendAsync(final);
        await peer.SendAsync(done);
        await AssertRecordAsync(body, final);
        await AssertRecordAsync(body, done);
        Assert.Equal(0, await body.ReadAsync(new byte[1]));
    }

    [Fact]
    public async Task HTTPの受信中断をエンジンの取消へ伝える()
    {
        await using var host = CreateHost();
        using var peer = new Peer();
        await peer.ConfigureAsync(1);
        host.MapPost("/view", context => FilterPreviewViewEndpoint.WriteAsync(context,
            (deliver, token) => peer.Engine.StreamViewAsync(new() { ["op"] = "step", ["direction"] = "next" }, 1, deliver, token)));
        await host.StartAsync();
        using var client = CreateClient(host);
        using var cancellation = new CancellationTokenSource();
        using var request = new HttpRequestMessage(HttpMethod.Post, "/view") { Content = JsonContent.Create(new { }) };
        var response = client.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, cancellation.Token);
        var command = await peer.NextCommandAsync("view");
        cancellation.Cancel();
        var error = await Record.ExceptionAsync(async () => await response);
        Assert.IsAssignableFrom<OperationCanceledException>(error);
        var cancel = await peer.NextCommandAsync("cancel");
        Assert.Equal(command.GetProperty("id").GetInt64(), cancel.GetProperty("id").GetInt64());
    }

    [Fact]
    public async Task 画像途中のEOFを成功応答にしない()
    {
        using var peer = new Peer();
        await peer.ConfigureAsync(1);
        var records = new List<FilterPreviewEngine.ViewRecord>();
        var pending = peer.Engine.StreamViewAsync(new() { ["op"] = "seek" }, 1,
            (record, _) => { records.Add(record); return Task.CompletedTask; }, CancellationToken.None);
        var command = await peer.NextCommandAsync("view");
        long id = command.GetProperty("id").GetInt64();
        var complete = Peer.View(id, 1, 1, provisional: true, [1]);
        var truncated = Peer.View(id, 1, 2, provisional: false, [2, 3]);
        await peer.SendAsync(complete);
        await peer.SendAsync(truncated[..^1]);
        peer.EndOutput();
        await Assert.ThrowsAsync<EndOfStreamException>(() => pending);
        Assert.Equal(new byte[] { 1 }, Assert.Single(records).Payload);
    }

    [Fact]
    public async Task 世代を切り替えると古い画像を読み捨てて新しい応答を分離する()
    {
        using var peer = new Peer();
        await peer.ConfigureAsync(1);
        var received = new List<byte>();
        var old = peer.Engine.StreamViewAsync(new() { ["op"] = "seek" }, 1,
            (record, _) => { received.AddRange(record.Payload); return Task.CompletedTask; }, CancellationToken.None);
        var first = await peer.NextCommandAsync("view");
        long oldId = first.GetProperty("id").GetInt64();
        await peer.ConfigureAsync(2, async () => await peer.SendAsync(Peer.View(oldId, 1, 1, false, [44])));
        var error = await Record.ExceptionAsync(async () => await old);
        Assert.IsAssignableFrom<OperationCanceledException>(error);
        Assert.Empty(received);

        var next = peer.Engine.StreamViewAsync(new() { ["op"] = "snapshot", ["format"] = "jpeg" }, 2,
            (record, _) => { received.AddRange(record.Payload); return Task.CompletedTask; }, CancellationToken.None);
        long id = (await peer.NextCommandAsync("view")).GetProperty("id").GetInt64();
        await peer.SendAsync(Peer.View(id, 2, 1, false, [55]));
        await peer.SendTextAsync($"AMTPREVIEW DONE id={id} gen=2 status=ok\n");
        await next;
        Assert.Equal(new byte[] { 55 }, received);
    }

    [Fact]
    public async Task v1エンジンは古い版であることをREADYのエラーで知らせる()
    {
        using var peer = new Peer();
        await peer.SendTextAsync("AMTPREVIEW READY version=1\n");
        var error = await Assert.ThrowsAsync<IOException>(() => peer.Engine.WaitReadyAsync(CancellationToken.None));
        Assert.Contains("古い", error.Message);
        Assert.Contains("v2", error.Message);
    }

    [Theory]
    [InlineData("seek", 1.25, "drag", null, null)]
    [InlineData("step", null, null, "previous", null)]
    [InlineData("display", null, null, null, "sideBySide")]
    public void エンコーダ操作は時刻や方向を変えずに転送する(string op, double? seconds, string? phase, string? direction, string? mode)
    {
        var command = new FilterPreviewViewRequest { Op = op, Seconds = seconds, Phase = phase, Direction = direction, Mode = mode }.ToCommand();
        Assert.Equal(op, command["op"]);
        Assert.Equal(seconds, command.GetValueOrDefault("seconds"));
        Assert.Equal(phase, command.GetValueOrDefault("phase"));
        Assert.Equal(direction, command.GetValueOrDefault("direction"));
        Assert.Equal(mode, command.GetValueOrDefault("mode"));
        Assert.False(command.ContainsKey("pos"));
    }

    private static WebApplication CreateHost()
    {
        var builder = WebApplication.CreateSlimBuilder();
        builder.WebHost.ConfigureKestrel(options => options.Listen(IPAddress.Loopback, 0));
        builder.Services.Configure<HostOptions>(options => options.ShutdownTimeout = Timeout.InfiniteTimeSpan);
        return builder.Build();
    }

    private static HttpClient CreateClient(WebApplication host)
    {
        var address = host.Services.GetRequiredService<IServer>().Features.Get<IServerAddressesFeature>()!.Addresses.Single();
        return new HttpClient(new SocketsHttpHandler { UseProxy = false })
        { BaseAddress = new Uri(address), Timeout = Timeout.InfiniteTimeSpan };
    }

    private static async Task AssertRecordAsync(Stream body, byte[] expected)
    {
        var actual = new byte[expected.Length];
        await body.ReadExactlyAsync(actual);
        Assert.Equal(expected, actual);
    }

    internal sealed class Peer : IDisposable
    {
        private readonly Pipe output = new();
        private readonly CommandWriter input = new();
        private bool ready;
        private int outputEnded;
        public FilterPreviewEngine Engine { get; }

        public Peer() => Engine = new FilterPreviewEngine(input, output.Reader.AsStream(), EndOutput);

        public async Task ConfigureAsync(long generation, Func<Task>? beforeReply = null)
        {
            if (!ready) { await SendTextAsync("AMTPREVIEW READY version=2\n"); ready = true; }
            var config = Engine.RequestAsync(new() { ["cmd"] = "config", ["kind"] = "encoder" }, generation, false, CancellationToken.None);
            Assert.Equal(generation, (await NextCommandAsync("config")).GetProperty("gen").GetInt64());
            if (beforeReply != null) await beforeReply();
            await SendTextAsync($"AMTPREVIEW INFO gen={generation} kind=encoder width=4 height=4\n");
            await config;
        }

        public async Task<JsonElement> NextCommandAsync(string name)
        {
            using var document = JsonDocument.Parse(await input.Commands.Reader.ReadAsync());
            var command = document.RootElement.Clone();
            Assert.Equal(name, command.GetProperty("cmd").GetString());
            return command;
        }

        public Task SendTextAsync(string line) => SendAsync(Encoding.UTF8.GetBytes(line));
        public async Task SendAsync(byte[] bytes) => _ = await output.Writer.WriteAsync(bytes);
        public void EndOutput()
        {
            if (Interlocked.Exchange(ref outputEnded, 1) == 0) output.Writer.Complete();
        }
        public void Dispose() => Engine.Dispose();

        public static byte[] View(long id, long gen, long revision, bool provisional, byte[] bytes, bool reuse = false)
        {
            object frame = reuse ? new { side = "after", bytes = bytes.Length, provisional, reuse = true }
                : new { side = "after", bytes = bytes.Length, provisional };
            var meta = JsonSerializer.SerializeToUtf8Bytes(new
            {
                frames = new[] { frame },
            });
            var header = Encoding.UTF8.GetBytes($"AMTPREVIEW VIEW id={id} gen={gen} revision={revision} metaBytes={meta.Length} bytes={bytes.Length}\n");
            return [.. header, .. meta, .. bytes];
        }
    }

    private sealed class CommandWriter : TextWriter
    {
        public override Encoding Encoding => Encoding.UTF8;
        public Channel<string> Commands { get; } = Channel.CreateUnbounded<string>();
        public override Task WriteLineAsync(string? value) => Commands.Writer.WriteAsync(value!).AsTask();
    }
}
