using System;
using System.Globalization;
using System.Collections;
using System.Reflection;
using System.IO;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Amatsukaze.Lib;
using Amatsukaze.Server;
using Amatsukaze.Server.Rest;
using Xunit;

namespace AmatsukazeServerTest;

[Collection("REST一時保存先")]
public class FilterPreviewServiceTests
{
    [Fact]
    public async Task 偽エンジンでセッションの設定取得とフレーム取得を行う()
    {
        if (OperatingSystem.IsWindows()) return;
        await using var fixture = new RestFileSystemFixture();
        var executable = Path.Combine(fixture.Root, "fake-engine");
        File.WriteAllText(executable, "#!/bin/sh\nprintf 'AMTPREVIEW READY version=1\\n'\nread line\nprintf 'AMTPREVIEW INFO gen=1 frames=4\\n'\nread line\nprintf 'AMTPREVIEW FRAME id=1 gen=1 pos=0 format=jpeg width=2 height=2 bytes=4 pitches=2,1,1\\nABCD'\nread line\nprintf 'AMTPREVIEW ERROR id=0 gen=2 message=failed\\n'\nread line\n");
        File.SetUnixFileMode(executable, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
        fixture.Data.setting.FilterPreviewEnginePath = executable;
        await fixture.Api.Queue.Store.OnCommonData(new CommonData { Setting = fixture.Data.setting });
        var temp = Path.Combine(fixture.Root, "work", "temp");
        Directory.CreateDirectory(temp);
        File.WriteAllBytes(Path.Combine(temp, "amts0.dat"), new byte[] { 0 });
        var start = new DateTime(2026, 1, 2, 3, 4, 5);
        var logs = Path.Combine(fixture.Root, "data", "logs");
        Directory.CreateDirectory(logs);
        File.WriteAllText(Path.Combine(logs, start.ToString("yyyy-MM-dd_HHmmss.fff", CultureInfo.InvariantCulture) + ".txt"), "一時フォルダ: " + temp, Util.AmatsukazeDefaultEncoding);
        var item = RestQueueTestFixture.Item(123, QueueState.Complete);
        item.EncodeStart = start;
        item.Profile = ServerSupport.NormalizeProfile(null!);
        item.Profile.Name = "標準";
        await fixture.Api.Queue.Seed(item);
        using var service = new FilterPreviewService(fixture.Api.Queue.Store, _ => new FilterPreviewSourceFormat { Progressive = 0, SarWidth = 4, SarHeight = 3, ColorSpace = 1 });
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        var id = await service.CreateAsync(123, timeout.Token);
        var initial = await service.GetConfigurationAsync(id, timeout.Token);
        Assert.Equal(0, initial.Gen);
        var configured = await service.ConfigureAsync(id, new FilterPreviewConfigRequest { Gen = 1, FilterOption = FilterOption.None }, timeout.Token);
        Assert.Equal("4", configured.Metadata["frames"]);
        var frame = await service.FrameAsync(id, new FilterPreviewFrameRequest { Gen = 1, Pos = 0, Format = "jpeg" }, timeout.Token);
        Assert.Equal("ABCD", Encoding.ASCII.GetString(frame.Data));
        var cached = await service.FrameAsync(id, new FilterPreviewFrameRequest { Gen = 1, Pos = 0, Format = "jpeg" }, timeout.Token);
        Assert.Same(frame.Data, cached.Data);
        await Assert.ThrowsAsync<ArgumentException>(() => service.ConfigureAsync(id, new FilterPreviewConfigRequest { Gen = 2, FilterOption = FilterOption.Custom }, timeout.Token));
        var after = await service.GetConfigurationAsync(id, timeout.Token);
        Assert.Equal(1, after.Gen);
        Assert.Equal(FilterOption.None, after.FilterOption);
        await Assert.ThrowsAsync<IOException>(() => service.ConfigureAsync(id, new FilterPreviewConfigRequest { Gen = 2, FilterOption = FilterOption.Setting, FilterSetting = new FilterSetting { EnableResize = true } }, timeout.Token));
        after = await service.GetConfigurationAsync(id, timeout.Token);
        Assert.Equal(FilterOption.None, after.FilterOption);
        var previewRoot = Path.Combine(temp, "filter_preview");
        var workDirectory = Path.Combine(previewRoot, id);
        Assert.True(Directory.Exists(workDirectory));
        Assert.True(service.Close(id));
        Assert.False(Directory.Exists(workDirectory));
        Assert.True(File.Exists(Path.Combine(previewRoot, "src.avs")));
        Assert.True(File.Exists(Path.Combine(temp, "amts0.dat")));

        var expiredId = await service.CreateAsync(123, timeout.Token);
        var sessions = (IDictionary)typeof(FilterPreviewService).GetField("sessions", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(service)!;
        var expired = sessions[expiredId]!;
        expired.GetType().GetField("LastUsed")!.SetValue(expired, DateTime.UtcNow.AddMinutes(-6).Ticks);
        typeof(FilterPreviewService).GetMethod("Cleanup", BindingFlags.Instance | BindingFlags.NonPublic)!.Invoke(service, null);
        Assert.False(Directory.Exists(Path.Combine(previewRoot, expiredId)));
        Assert.True(File.Exists(Path.Combine(previewRoot, "src.avs")));
    }
}
