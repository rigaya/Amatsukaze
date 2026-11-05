using Amatsukaze.Server;
using System.Globalization;
using Xunit;

namespace AmatsukazeServerTest;

public class NicoJKProgramStartTests
{
    [Theory]
    [InlineData(DateTimeKind.Unspecified)]
    [InlineData(DateTimeKind.Local)]
    [InlineData(DateTimeKind.Utc)]
    public void 番組開始の年月日時分秒を常にJSTとして渡す(DateTimeKind kind)
    {
        var start = new DateTime(2026, 10, 1, 23, 0, 0, kind);
        var expected = new DateTimeOffset(2026, 10, 1, 23, 0, 0, TimeSpan.FromHours(9)).ToUnixTimeSeconds();
        Assert.Equal(" --nicojk-program-start " + expected.ToString(CultureInfo.InvariantCulture),
            EncodeServer.GetNicoJKProgramStartArgument(true, start));
    }

    [Fact]
    public void 無効な日時と実況無効時は番組開始を渡さない()
    {
        Assert.Empty(EncodeServer.GetNicoJKProgramStartArgument(true, DateTime.MinValue));
        Assert.Empty(EncodeServer.GetNicoJKProgramStartArgument(true, new DateTime(2000, 1, 1)));
        Assert.Empty(EncodeServer.GetNicoJKProgramStartArgument(false, new DateTime(2026, 10, 1)));
        Assert.NotEmpty(EncodeServer.GetNicoJKProgramStartArgument(true, new DateTime(2000, 1, 1, 0, 0, 1)));
    }

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public void ジョブで実況を付けないときは実況関連のCLI引数を出さない(bool enable)
    {
        // 起動処理を省き、引数生成に必要な設定だけを与える。
        var server = (EncodeServer)System.Runtime.CompilerServices.RuntimeHelpers.GetUninitializedObject(typeof(EncodeServer));
        var setting = new Setting { SchedulingEnabled = true, NicoJKAssPath = "nicojk_ass.py" };
        RestQueueTestFixture.SetProperty(server, "AppData_", new EncodeServer.AppData { setting = setting });
        var profile = new ProfileSetting
        {
            EnableNicoJK = true, NicoJKLog = true, IgnoreNicoJKError = true, NicoJKFormats = new[] { true, false, false, false },
            EncoderType = EncoderType.x264, OutputFormat = FormatType.MKV, FilterOption = FilterOption.None
        };
        var arguments = server.MakeAmatsukazeArgs(
            mode: ProcMode.Batch, profile: profile, setting: setting, workPath: "work", isGeneric: false,
            src: "input.ts", srcOrg: null, dst: "output.mkv", json: "result.json", streamFormat: VideoStreamFormat.MPEG2,
            serviceId: 3088, logofiles: null, ignoreNoLogo: true, jlscommand: null, jlsopt: null, ceopt: null,
            trimavs: null, divfile: null, resumeDir: null, batDir: "bat", inHandle: null, outHandle: null, pid: 0,
            programStart: new DateTime(2026, 10, 11, 1, 0, 0), enableNicoJK: enable);
        if (enable)
        {
            Assert.Contains(" --nicojk ", arguments);
            Assert.Contains(" --nicojk-program-start ", arguments);
            Assert.Contains(" --ignore-nicojk-error", arguments);
            Assert.Contains(" --nicojklog", arguments);
            Assert.Contains(" --nicojkmask ", arguments);
            Assert.True(arguments.Contains(" --nicojkass ") || arguments.Contains(" --nicoass "));
        }
        else
        {
            Assert.DoesNotContain("nicojk", arguments);
            Assert.DoesNotContain(" --nicoass", arguments);
        }
        Assert.True(profile.EnableNicoJK);
    }
}
