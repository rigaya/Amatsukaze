using System;
using System.Collections.Generic;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Amatsukaze.Server.Rest;
using Xunit;

namespace AmatsukazeServerTest;

public class FilterPreviewEngineTests
{
    [LinuxFact]
    public async Task エラー本文のidとgenを応答の識別子として扱わない()
    {
        using var engine = StartEngine(
            "printf 'AMTPREVIEW READY version=2\\n'; read line; printf 'AMTPREVIEW INFO gen=1 frames=4\\n'; read line; printf 'AMTPREVIEW ERROR id=1 gen=1 message=外部ログ id=99 gen=88 rc=-55\\n'; read line");
        _ = await engine.RequestAsync(new Dictionary<string, object> { ["cmd"] = "config" }, 1, false, CancellationToken.None);
        var error = await Record.ExceptionAsync(() => engine.RequestAsync(
            new Dictionary<string, object> { ["cmd"] = "frame" }, 1, true, CancellationToken.None));
        var ioError = Assert.IsType<System.IO.IOException>(error);
        Assert.Equal("外部ログ id=99 gen=88 rc=-55", ioError.Message);
    }

    [LinuxFact]
    public async Task 対応しないREADYの版は起動失敗として返す()
    {
        using var engine = StartEngine("printf 'AMTPREVIEW READY version=3\\n'; read line");
        var error = await Record.ExceptionAsync(() => engine.WaitReadyAsync(CancellationToken.None));
        Assert.IsType<System.IO.IOException>(error);
        Assert.Contains("バージョン", error.Message);
    }

    [LinuxFact]
    public async Task 偽エンジンのヘッダとバイナリを続けて受信できる()
    {
        const string script = "printf 'AMTPREVIEW READY version=2\\n'; read line; printf 'AMTPREVIEW INFO gen=1 frames=4\\n'; read line; printf 'AMTPREVIEW FRAME id=1 gen=1 bytes=4 width=2 height=2\\nABCD'; read line";
        using var engine = StartEngine(script);
        var info = await engine.RequestAsync(new Dictionary<string, object> { ["cmd"] = "config" }, 1, false, CancellationToken.None);
        var frame = await engine.RequestAsync(new Dictionary<string, object> { ["cmd"] = "frame" }, 1, true, CancellationToken.None);
        Assert.Equal("4", info.Metadata["frames"]);
        Assert.Equal("ABCD", Encoding.ASCII.GetString(frame.Data));
        Assert.Equal("2", frame.Metadata["width"]);
    }
    [LinuxFact]
    public async Task 新しい設定は古い設定の待機を解除する()
    {
        const string script = """
            printf 'AMTPREVIEW READY version=2\n'
            read first
            read second
            case "$first" in *'"gen":1'*) ;; *) exit 1;; esac
            case "$second" in *'"gen":2'*) ;; *) exit 1;; esac
            printf 'AMTPREVIEW INFO gen=2 frames=4\n'
            read line
            """;
        using var engine = StartEngine(script.ReplaceLineEndings("\n"));
        await engine.WaitReadyAsync(CancellationToken.None);
        var old = engine.RequestAsync(new Dictionary<string, object> { ["cmd"] = "config" }, 1, false, CancellationToken.None);
        var next = engine.RequestAsync(new Dictionary<string, object> { ["cmd"] = "config" }, 2, false, CancellationToken.None);
        var oldResult = Record.ExceptionAsync(async () => await old);
        var nextResult = Record.ExceptionAsync(async () => await next);
        await Task.WhenAll(oldResult, nextResult);
        var reply = await next;
        Assert.IsAssignableFrom<OperationCanceledException>(await oldResult);
        Assert.Null(await nextResult);
        Assert.Equal("2", reply.Metadata["gen"]);
        Assert.Equal("4", reply.Metadata["frames"]);
    }

    [LinuxFact]
    public async Task 呼出前に取り消された要求は取消として終了する()
    {
        const string script = "printf 'AMTPREVIEW READY version=2\\n'; read line";
        using var engine = StartEngine(script);
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();
        var error = await Record.ExceptionAsync(() => engine.RequestAsync(
            new Dictionary<string, object> { ["cmd"] = "config" }, 1, false, cancellation.Token));
        Assert.IsAssignableFrom<OperationCanceledException>(error);
    }

    [Fact]
    public async Task READYを待つ期限を偽時計で進める()
    {
        var clock = new FakeTimeProvider();
        var deadline = TimeSpan.FromSeconds(15);
        var timing = FilterPreviewEngineTiming.Default with { Clock = clock };
        var ready = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var timers = clock.ObserveTimers();
        var wait = timing.WaitReadyAsync(ready.Task, CancellationToken.None);

        Assert.Equal(deadline, await timers.ReadAsync());
        clock.Advance(deadline - TimeSpan.FromTicks(1));
        Assert.False(wait.IsCompleted);
        clock.Advance(TimeSpan.FromTicks(1));

        await Assert.ThrowsAsync<TimeoutException>(() => wait);
    }

    private static FilterPreviewEngine StartEngine(string script) =>
        new("/bin/sh", new[] { "-c", script }, new FilterPreviewEngineTiming(
            TimeProvider.System, Timeout.InfiniteTimeSpan, Timeout.InfiniteTimeSpan));
}
