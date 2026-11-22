using Amatsukaze.Server;
using Xunit;

namespace AmatsukazeServerTest;

public class TranscodeWorkerTests
{
    [Theory]
    [InlineData(ProcMode.Batch)]
    [InlineData(ProcMode.AutoBatch)]
    [InlineData(ProcMode.Test)]
    public void エンコードは出力先にEncログを生成する(ProcMode mode)
    {
        Assert.Equal("output/program-enc.log", TranscodeWorker.GetOutputLogPath(mode, "output/program", false, false));
    }

    [Theory]
    [InlineData(ProcMode.CMCheck)]
    [InlineData(ProcMode.TrimAdjust)]
    public void CM解析は出力先にCMログを生成する(ProcMode mode)
    {
        Assert.Equal("output/program-cm.log", TranscodeWorker.GetOutputLogPath(mode, "output/program", false, true));
    }

    [Theory]
    [InlineData(ProcMode.DrcsCheck)]
    [InlineData(ProcMode.FilterAdjust)]
    public void DRCSチェックとTS解析は出力先ログを生成しない(ProcMode mode)
    {
        Assert.Null(TranscodeWorker.GetOutputLogPath(mode, "output/program", false, true));
    }

    [Fact]
    public void フィルタ調整はTS解析に必要な入力の引数だけを渡す()
    {
        var fixture = new RestQueueTestFixture();
        var setting = new Setting { SchedulingEnabled = true, ChapterExePath = "chapter.exe", JoinLogoScpPath = "join_logo_scp.exe" };
        var profile = ServerSupport.NormalizeProfile(null!);
        profile.EnablePmtCut = true;
        profile.PmtCutHeadRate = 25;
        profile.PmtCutTailRate = 75;
        profile.H264Deocder = DecoderType.CUVID;
        string args = fixture.Server.MakeAmatsukazeArgs(
            ProcMode.FilterAdjust, profile, setting, "作業先", false,
            "入力.ts", "コピー元.ts", "出力.mkv", "出力.json", VideoStreamFormat.H264,
            3088, ["logo.lgd"], true, "JL.txt", "JLS", "CE", "trim.avs", "div.txt", "再開先", "bat",
            "10", "20", 0);

        Assert.Contains(" --mode ts_analyze ", args);
        Assert.Contains(@" -i ""入力.ts"" -s 3088 ", args);
        Assert.Contains(@" --original-input-file ""コピー元.ts""", args);
        Assert.Contains(" --resource-manager 10:20", args);
        Assert.Contains(" --pmt-cut 0.25:0.75", args);
        Assert.Contains(" --h264decoder CUVID", args);
        Assert.Contains(@" -w ""作業先"" --no-remove-tmp", args);
        foreach (string option in new[] { " --chapter", " --jls", " --logo", " --auto-logo", " --trimavs", " --divfile", " --resume-dir", " --subtitles", " -o " })
            Assert.DoesNotContain(option, args);
        Assert.Equal(CheckType.TS, ProcMode.FilterAdjust.GetCheckLogType());
    }

    [Fact]
    public void エンコードログ無効時は出力先ログを生成しない()
    {
        Assert.Null(TranscodeWorker.GetOutputLogPath(ProcMode.Batch, "output/program", true, false));
    }

    [Fact]
    public void CM解析ログ無効時は出力先ログを生成しない()
    {
        Assert.Null(TranscodeWorker.GetOutputLogPath(ProcMode.CMCheck, "output/program", false, false));
    }

    [Fact]
    public void エンコードログ無効時もCM解析ログは生成できる()
    {
        Assert.Equal("output/program-cm.log",
            TranscodeWorker.GetOutputLogPath(ProcMode.CMCheck, "output/program", true, true));
    }
}
