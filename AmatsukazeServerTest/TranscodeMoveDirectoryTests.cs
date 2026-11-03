using Amatsukaze.Server;
using Xunit;

public class TranscodeMoveDirectoryTests
{
    [Fact]
    public void 作成失敗でも例外を伝播せず両方を試行する()
    {
        var path = Path.GetTempFileName();
        try
        {
            var warnings = new List<string>();
            TranscodeWorker.PrepareInputMoveDirectories(true, false, Path.Combine(path, "succeeded"), Path.Combine(path, "failed"), (dir, _) => warnings.Add(dir));
            Assert.Equal(2, warnings.Count);
            TranscodeWorker.PrepareInputMoveDirectories(true, true, Path.Combine(path, "succeeded"), Path.Combine(path, "failed"), (dir, _) => warnings.Add(dir));
            Assert.Equal(2, warnings.Count);
        }
        finally { File.Delete(path); }
    }
}
