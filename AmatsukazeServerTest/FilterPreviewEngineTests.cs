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
    [Fact]
    public async Task 偽エンジンのヘッダとバイナリを続けて受信できる()
    {
        if (OperatingSystem.IsWindows()) return;
        const string script = "printf 'AMTPREVIEW READY version=1\\n'; read line; printf 'AMTPREVIEW INFO gen=1 frames=4\\n'; read line; printf 'AMTPREVIEW FRAME id=1 gen=1 bytes=4 width=2 height=2\\nABCD'; read line";
        using var engine = new FilterPreviewEngine("/bin/sh", new[] { "-c", script });
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        var info = await engine.RequestAsync(new Dictionary<string, object> { ["cmd"] = "config" }, 1, false, timeout.Token);
        Assert.Equal("4", info.Metadata["frames"]);
        var frame = await engine.RequestAsync(new Dictionary<string, object> { ["cmd"] = "frame" }, 1, true, timeout.Token);
        Assert.Equal("ABCD", Encoding.ASCII.GetString(frame.Data));
        Assert.Equal("2", frame.Metadata["width"]);
    }
    [Fact]
    public async Task 新しい設定は古い設定の待機を解除する()
    {
        if (OperatingSystem.IsWindows()) return;
        const string script = "printf 'AMTPREVIEW READY version=1\\n'; read line; read line; printf 'AMTPREVIEW INFO gen=2 frames=4\\n'; read line";
        using var engine = new FilterPreviewEngine("/bin/sh", new[] { "-c", script });
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        var old = engine.RequestAsync(new Dictionary<string, object> { ["cmd"] = "config" }, 1, false, timeout.Token);
        await Task.Delay(100, timeout.Token);
        var next = engine.RequestAsync(new Dictionary<string, object> { ["cmd"] = "config" }, 2, false, timeout.Token);
        await Assert.ThrowsAnyAsync<OperationCanceledException>(async () => await old);
        Assert.Equal("4", (await next).Metadata["frames"]);
    }
}
