using System.Reflection;
using Amatsukaze.Server;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class TranscodeAudioDiffTests
{
    [Theory]
    [InlineData("")]
    [InlineData(",\"audiodiff\":null")]
    public void 未計測の音声統計は成功ログにも数値を設定しない(string property)
    {
        var log = ReadLog(property);

        Assert.True(log.Success);
        Assert.Single(log.OutPath);
        Assert.Null(log.AudioDiff);
        Assert.Null(log.DisplayAvgAudioDiff);
        Assert.Null(log.DisplayAudioMaxDiff);
    }

    [Fact]
    public void 従来形式の音声統計は各計測値を維持する()
    {
        var log = ReadLog("""
            ,"audiodiff":{"totalsrcframes":100,"totaloutframes":95,"totaloutuniqueframes":90,
            "notincludedper":10,"avgdiff":2.5,"maxdiff":12.5,"maxdiffpos":3.5}
            """);

        var diff = Assert.IsType<AudioDiff>(log.AudioDiff);
        Assert.Equal(100, diff.TotalSrcFrames);
        Assert.Equal(95, diff.TotalOutFrames);
        Assert.Equal(90, diff.TotalOutUniqueFrames);
        Assert.Equal(10, diff.NotIncludedPer);
        Assert.Equal(2.5, diff.AvgDiff);
        Assert.Equal(12.5, diff.MaxDiff);
        Assert.Equal(3.5, diff.MaxDiffPos);
    }

    private static LogItem ReadLog(string property)
    {
        var path = Path.GetTempFileName();
        try
        {
            File.WriteAllText(path, """
                {"srcpath":"source.ts","outfiles":[{"path":"output.ts","subs":[]}],"logofiles":[],
                "error":{},"srcfilesize":1000,"intvideofilesize":900,"outfilesize":100,
                "srcduration":30,"outduration":30,"cmanalyze":false,"nicojk":false,"trimavs":false
                """ + property + "}");
            var method = typeof(TranscodeWorker).GetMethod("LogFromJson", BindingFlags.NonPublic | BindingFlags.Instance)!;
            return Assert.IsType<LogItem>(method.Invoke(new TranscodeWorker(0, null!), new object[] {
                false, "試験プロファイル", path, new DateTime(2026, 11, 10), new DateTime(2026, 11, 10, 0, 1, 0), new QueueItem(), 1,
            }));
        }
        finally { File.Delete(path); }
    }
}
