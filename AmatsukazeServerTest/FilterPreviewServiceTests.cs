using System;
using System.IO;
using System.Runtime.Versioning;
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
    [LinuxTheory]
    [SupportedOSPlatform("linux")]
    [InlineData(FilterOption.None, ProcMode.Batch, null)]
    [InlineData(FilterOption.Custom, ProcMode.Batch, null)]
    [InlineData(FilterOption.None, ProcMode.FilterAdjust, CheckType.CM)]
    [InlineData(FilterOption.None, ProcMode.FilterAdjust, CheckType.TS)]
    public async Task 偽エンジンでセッションの設定取得とフレーム取得を行う(
        FilterOption originalOption, ProcMode mode, CheckType? logType)
    {
        await using var fixture = new RestFileSystemFixture();
        var executable = Path.Combine(fixture.Root, "fake-engine");
        File.WriteAllText(executable, "#!/bin/sh\nprintf 'AMTPREVIEW READY version=2\\n'\nread -r line\nprintf '%s\\n' \"$line\" > \"$0.config\"\nprintf 'AMTPREVIEW INFO gen=1 frames=4\\n'\nread line\nprintf 'AMTPREVIEW FRAME id=1 gen=1 pos=0 format=jpeg width=2 height=2 bytes=4 pitches=2,1,1\\nABCD'\nread -r line\nprintf '%s\\n' \"$line\" >> \"$0.config\"\nprintf 'AMTPREVIEW ERROR id=0 gen=2 message=failed\\n'\nread line\n");
        File.SetUnixFileMode(executable, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
        fixture.Data.setting.FilterPreviewEnginePath = executable;
        await fixture.Api.Queue.Store.OnCommonData(new CommonData { Setting = fixture.Data.setting });
        var temp = Path.Combine(fixture.Root, "work", "temp");
        Directory.CreateDirectory(temp);
        File.WriteAllBytes(Path.Combine(temp, "amts0.dat"), new byte[] { 0 });
        var start = new DateTime(2026, 1, 2, 3, 4, 5);
        var server = fixture.Api.Queue.Server;
        var log = (logType.HasValue ? server.GetCheckLogFileBase(start) : server.GetLogFileBase(start)) + ".txt";
        Directory.CreateDirectory(Path.GetDirectoryName(log)!);
        File.WriteAllText(log, "一時フォルダ: " + temp, Util.AmatsukazeDefaultEncoding);
        var item = RestQueueTestFixture.Item(123, QueueState.Complete);
        item.Mode = mode;
        item.SrcPath = @"C:\録画\フィルタ確認.ts";
        item.EncodeStart = start;
        item.Profile = ServerSupport.NormalizeProfile(null!);
        item.Profile.Name = "標準";
        item.Profile.FilterOption = originalOption;
        item.Profile.FilterPath = "カスタム.avs";
        await fixture.Api.Queue.Seed(item);
        if (logType is { } type)
        {
            await fixture.Api.Queue.Store.OnUIData(new UIData
            {
                CheckLogData = new CheckLogData
                {
                    Items = [new CheckLogItem { Type = type, SrcPath = item.SrcPath, CheckStartDate = start, Success = true }],
                },
            });
        }
        var timing = new FilterPreviewEngineTiming(new FakeTimeProvider(), Timeout.InfiniteTimeSpan, Timeout.InfiniteTimeSpan);
        using var service = new FilterPreviewService(fixture.Api.Queue.Store,
            _ => new FilterPreviewSourceFormat { Progressive = 0, SarWidth = 4, SarHeight = 3, ColorSpace = 1 }, timing);
        var id = await service.CreateAsync(123, CancellationToken.None);
        var sourceName = service.GetSourceName(id);
        var initial = await service.GetConfigurationAsync(id, CancellationToken.None);
        var configured = await service.ConfigureAsync(id, new FilterPreviewConfigRequest { Gen = 1, FilterOption = FilterOption.None }, CancellationToken.None);
        var frame = await service.FrameAsync(id, new FilterPreviewFrameRequest { Gen = 1, Pos = 0, Format = "jpeg" }, CancellationToken.None);
        var cached = await service.FrameAsync(id, new FilterPreviewFrameRequest { Gen = 1, Pos = 0, Format = "jpeg" }, CancellationToken.None);
        var customError = await Record.ExceptionAsync(() => service.ConfigureAsync(id, new FilterPreviewConfigRequest { Gen = 2, FilterOption = FilterOption.Custom }, CancellationToken.None));
        var engineError = await Record.ExceptionAsync(() => service.ConfigureAsync(id, new FilterPreviewConfigRequest { Gen = 2, FilterOption = FilterOption.Setting, FilterSetting = new FilterSetting { EnableDeinterlace = true, DeinterlaceAlgorithm = DeinterlaceAlgorithm.Yadif } }, CancellationToken.None));
        var after = await service.GetConfigurationAsync(id, CancellationToken.None);
        var preserved = await service.GetEditedProfileAsync(id, CancellationToken.None, true);
        var selected = await service.GetEditedProfileAsync(id, CancellationToken.None);
        var previewRoot = Path.Combine(temp, "filter_preview");
        var workDirectory = Path.Combine(previewRoot, id);
        var closed = service.Close(id);

        Assert.Equal(originalOption, preserved.FilterOption);
        Assert.Equal("カスタム.avs", preserved.FilterPath);
        Assert.Equal(FilterOption.None, selected.FilterOption);
        Assert.Equal("フィルタ確認.ts", sourceName);
        Assert.Equal(0, initial.Gen);
        Assert.Equal("4", configured.Metadata["frames"]);
        var commands = File.ReadAllLines(executable + ".config");
        Assert.Equal(2, commands.Length);
        using var firstConfig = System.Text.Json.JsonDocument.Parse(commands[0]);
        Assert.Equal("tff", firstConfig.RootElement.GetProperty("interlace").GetString());
        Assert.Equal("tff", firstConfig.RootElement.GetProperty("afterInterlace").GetString());
        using var secondConfig = System.Text.Json.JsonDocument.Parse(commands[1]);
        Assert.Equal("tff", secondConfig.RootElement.GetProperty("interlace").GetString());
        Assert.Equal("progressive", secondConfig.RootElement.GetProperty("afterInterlace").GetString());
        Assert.Equal("ABCD", Encoding.ASCII.GetString(frame.Data));
        Assert.Equal("ABCD", Encoding.ASCII.GetString(cached.Data));
        Assert.IsType<ArgumentException>(customError);
        Assert.IsType<IOException>(engineError);
        Assert.Equal(2, after.Gen);
        Assert.Equal(FilterOption.None, after.FilterOption);
        Assert.True(closed);
        Assert.False(Directory.Exists(workDirectory));
        Assert.True(File.Exists(Path.Combine(previewRoot, "src.avs")));
        Assert.True(File.Exists(Path.Combine(temp, "amts0.dat")));
    }
}
