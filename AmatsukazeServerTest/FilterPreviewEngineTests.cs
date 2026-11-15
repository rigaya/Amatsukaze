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
        var frame = await engine.RequestAsync(new Dictionary<string, object> { ["cmd"] = "frame" }, 1, true, timeout.Token);
        Assert.Equal("4", info.Metadata["frames"]);
        Assert.Equal("ABCD", Encoding.ASCII.GetString(frame.Data));
        Assert.Equal("2", frame.Metadata["width"]);
    }
    [Fact]
    public async Task 呼出前に取り消された要求は取消として終了する()
    {
        if (OperatingSystem.IsWindows()) return;
        const string script = "printf 'AMTPREVIEW READY version=1\\n'; read line";
        using var engine = new FilterPreviewEngine("/bin/sh", new[] { "-c", script });
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();
        var error = await Record.ExceptionAsync(() => engine.RequestAsync(
            new Dictionary<string, object> { ["cmd"] = "config" }, 1, false, cancellation.Token));
        Assert.IsAssignableFrom<OperationCanceledException>(error);
    }
}
