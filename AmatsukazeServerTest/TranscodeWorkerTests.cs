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

    [Fact]
    public void CM解析は出力先にCMログを生成する()
    {
        Assert.Equal("output/program-cm.log", TranscodeWorker.GetOutputLogPath(ProcMode.CMCheck, "output/program", false, true));
    }

    [Fact]
    public void DRCSチェックは出力先ログを生成しない()
    {
        Assert.Null(TranscodeWorker.GetOutputLogPath(ProcMode.DrcsCheck, "output/program", false, true));
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
