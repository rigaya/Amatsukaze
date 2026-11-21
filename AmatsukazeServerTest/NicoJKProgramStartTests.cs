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

    private static string MakeNicoJKArguments(ProfileSetting profile, string nicoJKAlt, string nicoJKAssPath = "nicojk_ass.py")
    {
        // 起動処理を省き、引数生成に必要な設定だけを与える。
        var server = (EncodeServer)System.Runtime.CompilerServices.RuntimeHelpers.GetUninitializedObject(typeof(EncodeServer));
        var setting = new Setting { SchedulingEnabled = true, NicoJKAssPath = nicoJKAssPath };
        RestQueueTestFixture.SetProperty(server, "AppData_", new EncodeServer.AppData { setting = setting });
        return server.MakeAmatsukazeArgs(
            mode: ProcMode.Batch, profile: profile, setting: setting, workPath: "work", isGeneric: false,
            src: "input.ts", srcOrg: null, dst: "output.mkv", json: "result.json", streamFormat: VideoStreamFormat.MPEG2,
            serviceId: 3088, logofiles: null, ignoreNoLogo: true, jlscommand: null, jlsopt: null, ceopt: null,
            trimavs: null, divfile: null, resumeDir: null, batDir: "bat", inHandle: null, outHandle: null, pid: 0,
            programStart: new DateTime(2026, 10, 11, 1, 0, 0), nicoJKAlt: nicoJKAlt);
    }

    private static ProfileSetting MakeNicoJKProfile(bool enable) => new ProfileSetting
    {
        EnableNicoJK = enable, NicoJKLog = true, IgnoreNicoJKError = true, NicoJKFormats = new[] { true, false, false, false },
        EncoderType = EncoderType.x264, OutputFormat = FormatType.MKV, FilterOption = FilterOption.None
    };

    [Fact]
    public void 実況の引数を出す()
    {
        var arguments = MakeNicoJKArguments(MakeNicoJKProfile(true), null);
        Assert.Contains(" --nicojk ", arguments);
        Assert.Contains(" --nicojk-program-start ", arguments);
        Assert.Contains(" --ignore-nicojk-error", arguments);
        Assert.Contains(" --nicojklog", arguments);
        Assert.Contains(" --nicojkmask ", arguments);
        Assert.True(arguments.Contains(" --nicojkass ") || arguments.Contains(" --nicoass "));
        Assert.DoesNotContain("--nicojk-alt", arguments);
    }

    [Fact]
    public void 実況を無効にしたプロファイルでは実況関連のCLI引数を出さない()
    {
        var arguments = MakeNicoJKArguments(MakeNicoJKProfile(false), "merge");
        Assert.DoesNotContain("nicojk", arguments);
        Assert.DoesNotContain(" --nicoass", arguments);
    }

    [Theory]
    [InlineData("replace")]
    [InlineData("merge")]
    public void 別放送のコメントの指定をnicojk_assへ渡す(string mode)
    {
        var arguments = MakeNicoJKArguments(MakeNicoJKProfile(true), mode);
        Assert.Contains(" --nicojk-alt " + mode, arguments);
        Assert.Contains(" --nicojkass ", arguments);
    }

    [Theory]
    // Linux: NicoConvASSのパスがあってもnicojk_assだけ
    [InlineData(false, "NicoConvASS.exe", "nicojk_ass", null, " --nicojkass \"nicojk_ass\"")]
    [InlineData(false, "NicoConvASS.exe", "nicojk_ass", "merge", " --nicojk-alt merge --nicojkass \"nicojk_ass\"")]
    // Windows: NicoConvASSがなければnicojk_ass
    [InlineData(true, "", "nicojk_ass.exe", "replace", " --nicojk-alt replace --nicojkass \"nicojk_ass.exe\"")]
    // Windows: NicoConvASSがあれば通常はNicoConvASSだけ
    [InlineData(true, "NicoConvASS.exe", "nicojk_ass.exe", null, " --nicoass \"NicoConvASS.exe\"")]
    // Windows: 別放送のコメントを使うなら、NicoConvASSとnicojk_assの両方を渡す
    [InlineData(true, "NicoConvASS.exe", "nicojk_ass.exe", "merge",
        " --nicojk-alt merge --nicoass \"NicoConvASS.exe\" --nicojkass \"nicojk_ass.exe\"")]
    // Windows: nicojk_assのパスがなければ別放送のコメントは使わない
    [InlineData(true, "NicoConvASS.exe", "", "merge", " --nicoass \"NicoConvASS.exe\"")]
    public void 別放送のコメントはNicoConvASSの構成でもnicojk_assで使う(
        bool isWindows, string nicoConvAssPath, string nicoJKAssPath, string nicoJKAlt, string expected)
    {
        Assert.Equal(expected, EncodeServer.GetNicoJKToolArguments(isWindows, nicoConvAssPath, nicoJKAssPath, nicoJKAlt));
    }

    [Theory]
    [InlineData(0, 7, true, null)]
    [InlineData(1, 7, true, "replace")]
    [InlineData(2, 7, true, "merge")]
    [InlineData(2, 7, false, null)]
    [InlineData(2, 6, true, null)]
    public void アニメで別放送のコメントを使う設定のときだけモードを決める(int referenceMode, int level1, bool enable, string expected)
    {
        var profile = new ProfileSetting { EnableNicoJK = enable, NicoJKReferenceMode = referenceMode };
        var genre = new List<GenreItem> { new GenreItem { Space = (int)GenreSpace.ARIB, Level1 = level1 } };
        Assert.Equal(expected, EncodeServer.GetNicoJKAltMode(profile, genre));
    }

    [Fact]
    public void ジャンルがないときは別放送のコメントを使わない()
    {
        var profile = new ProfileSetting { EnableNicoJK = true, NicoJKReferenceMode = 1 };
        Assert.Null(EncodeServer.GetNicoJKAltMode(profile, null));
        Assert.Null(EncodeServer.GetNicoJKAltMode(profile, new List<GenreItem>()));
    }
}
