using System.Text.Json;
using Amatsukaze.Lib;
using Amatsukaze.Server;
using Xunit;

namespace AmatsukazeServerTest;

[Collection("REST一時保存先")]
public sealed class FilterAdjustQueueTests
{
    [Fact]
    public async Task ロゴがないチャンネルでもフィルタ調整をスケジュールする()
    {
        await using var fixture = new RestFileSystemFixture();
        var item = RestQueueTestFixture.Item(7, QueueState.LogoPending);
        item.Mode = ProcMode.FilterAdjust;
        item.ServiceId = 3088;
        item.Profile = ServerSupport.NormalizeProfile(null!);
        fixture.Data.services.ServiceMap[item.ServiceId] = new ServiceSettingElement { LogoSettings = new List<LogoSetting>() };
        await fixture.Api.Queue.Seed(item);

        Assert.True(fixture.Api.Queue.Manager.UpdateQueueItem(item, null));
        Assert.Equal(QueueState.Queue, item.State);
        Assert.Empty(item.FailReason);
    }

    [Fact]
    public async Task フィルタ調整の一時ファイルからは通常処理として再投入する()
    {
        await using var fixture = new RestFileSystemFixture();
        var item = RestQueueTestFixture.Item(7, QueueState.Complete);
        item.Mode = ProcMode.FilterAdjust;
        item.Profile = ServerSupport.NormalizeProfile(null!);
        item.Profile.Name = "標準";
        fixture.Profiles.Add(item.Profile.Name, item.Profile);
        item.ServiceId = 3088;
        fixture.Data.services.ServiceMap[item.ServiceId] = new ServiceSettingElement { LogoSettings = new List<LogoSetting>() };
        item.SrcPath = Path.Combine(fixture.Root, "入力.ts");
        File.WriteAllText(item.SrcPath, "試験用");
        item.EncodeStart = new DateTime(2026, 1, 2, 3, 4, 5);
        var temp = Path.Combine(fixture.Root, "work", "amt-test");
        Directory.CreateDirectory(temp);
        File.WriteAllBytes(Path.Combine(temp, "amts0.dat"), [0]);
        string log = fixture.Api.Queue.Server.GetCheckLogFileBase(item.EncodeStart) + ".txt";
        Directory.CreateDirectory(Path.GetDirectoryName(log)!);
        File.WriteAllText(log, "一時フォルダ: " + temp, Util.AmatsukazeDefaultEncoding);
        await fixture.Api.Queue.Seed(item);

        Assert.Equal(log, fixture.Api.Queue.Store.ResolveTaskLogPathById(item.Id));
        var trim = await fixture.Api.Send("POST", "/api/trim/sessions", body: """{"queueItemId":7}""");
        Assert.Equal(400, trim.Status);
        using var trimError = JsonDocument.Parse(trim.Body);
        Assert.Contains("フィルタ調整のタスク", trimError.RootElement.GetProperty("message").GetString());

        var response = await fixture.Api.Send("POST", "/api/trim/requeue",
            body: """{"queueItemId":7,"profile":"標準","priority":3}""");
        Assert.Equal(200, response.Status);
        using var json = JsonDocument.Parse(response.Body);
        Assert.False(json.RootElement.GetProperty("reuseTempDir").GetBoolean());
        int newId = json.RootElement.GetProperty("queueItemId").GetInt32();
        var requeued = Assert.Single(fixture.Api.Queue.Manager.Queue, value => value.Id == newId);
        Assert.Equal(ProcMode.Batch, requeued.Mode);
        Assert.Null(requeued.ResumeDir);
        Assert.True(File.Exists(Path.Combine(temp, "amts0.dat")));
    }
}
