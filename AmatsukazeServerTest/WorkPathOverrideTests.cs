using Amatsukaze.Server;
using System.Runtime.Serialization;
using Xunit;

namespace AmatsukazeServerTest;

public class WorkPathOverrideTests
{
    [Fact]
    public void 未指定なら実行時点のグローバル設定を使用する()
    {
        var setting = new Setting { WorkPath = "global-before" };
        var item = new QueueItem();

        Assert.Equal("global-before", item.GetEffectiveWorkPath(setting));

        setting.WorkPath = "global-after";
        Assert.Equal("global-after", item.GetEffectiveWorkPath(setting));
    }

    [Fact]
    public void 個別指定はグローバル設定を変更しても維持する()
    {
        var setting = new Setting { WorkPath = "global-before" };
        var item = new QueueItem { WorkPathOverride = "task-work" };

        setting.WorkPath = "global-after";

        Assert.Equal("task-work", item.GetEffectiveWorkPath(setting));
    }

    [Fact]
    public void 空白指定は未指定として扱う()
    {
        var setting = new Setting { WorkPath = "global-work" };
        var item = new QueueItem { WorkPathOverride = "   " };

        Assert.Equal("global-work", item.GetEffectiveWorkPath(setting));
    }

    [Theory]
    [InlineData(null)]
    [InlineData("task-work")]
    public void キュー保存後も個別指定の有無を維持する(string? workPathOverride)
    {
        var serializer = new DataContractSerializer(typeof(QueueItem));
        var source = new QueueItem { WorkPathOverride = workPathOverride };
        using var stream = new MemoryStream();

        serializer.WriteObject(stream, source);
        stream.Position = 0;
        var restored = Assert.IsType<QueueItem>(serializer.ReadObject(stream));

        Assert.Equal(workPathOverride, restored.WorkPathOverride);
    }
}
