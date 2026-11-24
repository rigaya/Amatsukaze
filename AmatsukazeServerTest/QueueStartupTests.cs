using System.Reflection;
using System.Threading.Tasks.Dataflow;
using Amatsukaze.Server;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class QueueStartupTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task 先に届いた追加要求は初回走査の終了後に処理する(bool scanFails)
    {
        var fixture = new RestQueueTestFixture();
        var requests = fixture.CaptureQueueRequests();
        var firstScan = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        RestQueueTestFixture.SetField(fixture.Server, "firstFileScan", firstScan);
        requests.Post(new AddQueueRequest { Outputs = new List<OutputInfo>() });
        requests.Complete();

        var queue = RunThread(fixture.Server, "QueueThread");
        Assert.False(queue.IsCompleted);
        Assert.Equal(1, requests.Count);

        if (scanFails)
        {
            // 設定を入れていないので、ファイル監視の初期化で例外になる。
            RestQueueTestFixture.SetField(fixture.Server, "watchFileQ",
                new BufferBlock<int>());
            await RunThread(fixture.Server, "WatchFileThread").WaitAsync(TimeSpan.FromSeconds(5));
        }
        else
        {
            firstScan.SetResult(true);
        }

        await queue.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(0, requests.Count);
        Assert.Contains("出力が1つもありません", fixture.Store.GetLastOperationResult().Message);
    }

    private static Task RunThread(EncodeServer server, string name) =>
        (Task)typeof(EncodeServer).GetMethod(name, BindingFlags.Instance | BindingFlags.NonPublic)!
            .Invoke(server, null)!;
}
