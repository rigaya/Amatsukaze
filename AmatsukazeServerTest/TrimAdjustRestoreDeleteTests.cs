using System.Reflection;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using Amatsukaze.Server;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class TrimAdjustRestoreDeleteTests
{
    [Theory]
    [InlineData(1, 400, true)]
    [InlineData(2, 200, false)]
    public async Task CM解析一時フォルダー削除は同じキューの復元中だけ保留する(
        int restoringQueueItemId, int expectedStatus, bool expectTempDirExists)
    {
        var fixture = new RestEndpointTestFixture();
        string? registryRoot = null;
        try
        {
            var registry = RestQueueTestFixture.GetField(
                fixture.Queue.Server, "trimAdjustTempDirRegistry");
            registryRoot = (string)RestQueueTestFixture.GetField(registry, "workRoot");
            var tempDir = Path.Combine(registryRoot, "amt123456789");
            Directory.CreateDirectory(tempDir);
            File.WriteAllText(Path.Combine(tempDir, "amts0.dat"), "fixture");

            var item = RestQueueTestFixture.Item(1, QueueState.Complete);
            item.Mode = ProcMode.CMCheck;
            item.ResumeDir = tempDir;
            var other = RestQueueTestFixture.Item(2, QueueState.Complete);
            other.Mode = ProcMode.CMCheck;
            await fixture.Queue.Seed(item, other);

            var host = RestQueueTestFixture.GetField(fixture, "host");
            var trimAdjust = RestQueueTestFixture.GetField(host, "trimAdjust");
            RestQueueTestFixture.SetField(
                trimAdjust, "_restoringKey", (restoringQueueItemId, 0));

            var result = await fixture.Send(
                "DELETE", "/api/trim/tempdir/{queueItemId:int}",
                routeValues: new { queueItemId = 1 });

            Assert.Equal(expectedStatus, result.Status);
            Assert.Equal(expectTempDirExists, Directory.Exists(tempDir));
            if (restoringQueueItemId == 1)
            {
                using var json = JsonDocument.Parse(result.Body);
                Assert.Equal(
                    "一時ファイルを復元中のためフォルダー削除を保留しました",
                    json.RootElement.GetProperty("message").GetString());
            }
        }
        finally
        {
            try { await fixture.DisposeAsync(); }
            finally { DeleteTestRegistryRoot(registryRoot); }
        }
    }

    [Fact]
    public async Task 復元開始は一時フォルダー変更ロックの解放までキーを登録しない()
    {
        var fixture = new RestEndpointTestFixture();
        string? registryRoot = null;
        try
        {
            var registry = RestQueueTestFixture.GetField(
                fixture.Queue.Server, "trimAdjustTempDirRegistry");
            registryRoot = (string)RestQueueTestFixture.GetField(registry, "workRoot");

            var host = RestQueueTestFixture.GetField(fixture, "host");
            var trimAdjust = RestQueueTestFixture.GetField(host, "trimAdjust");
            var mutationLock = RestQueueTestFixture.GetField(trimAdjust, "tempDirMutationLock");
            var restoreKeyField = trimAdjust.GetType().GetField(
                "_restoringKey", BindingFlags.Instance | BindingFlags.NonPublic)
                ?? throw new InvalidOperationException("復元中キーのフィールドが見つかりません");
            var restoreJobField = trimAdjust.GetType().GetField(
                "_restoreJobTask", BindingFlags.Instance | BindingFlags.NonPublic)
                ?? throw new InvalidOperationException("復元ジョブフィールドが見つかりません");
            var restoreMethod = trimAdjust.GetType().GetMethod(
                "TryRestoreAndCreateSessionAsync", BindingFlags.Instance | BindingFlags.Public)
                ?? throw new InvalidOperationException("復元開始メソッドが見つかりません");

            // Monitorはスレッド固有なので、ロックを保持する同期区間を専用スレッド上で実行する。
            Task<object?>? invocation = null;
            await Task.Factory.StartNew(() =>
            {
                var started = new TaskCompletionSource<bool>(
                    TaskCreationOptions.RunContinuationsAsynchronously);
                var lockTaken = false;
                Monitor.Enter(mutationLock, ref lockTaken);
                try
                {
                    var pendingInvocation = Task.Run<object?>(() =>
                    {
                        started.TrySetResult(true);
                        return restoreMethod.Invoke(
                            trimAdjust,
                            new object?[] { int.MaxValue, 0, CancellationToken.None });
                    });
                    invocation = pendingInvocation;

                    Assert.True(started.Task.Wait(TimeSpan.FromSeconds(5)));
                    Assert.False(pendingInvocation.Wait(TimeSpan.FromMilliseconds(250)));
                    Assert.Null(restoreKeyField.GetValue(trimAdjust));
                    Assert.Null(restoreJobField.GetValue(trimAdjust));
                }
                finally
                {
                    if (lockTaken) Monitor.Exit(mutationLock);
                }
            }, CancellationToken.None, TaskCreationOptions.LongRunning, TaskScheduler.Default);

            var startedInvocation = invocation
                ?? throw new InvalidOperationException("復元呼び出しタスクが開始されませんでした");
            var restoreRequest = Assert.IsAssignableFrom<Task>(
                await startedInvocation.WaitAsync(TimeSpan.FromSeconds(5)));
            // int.MaxValueはfixtureに存在せず、復元元検証でCLI起動前に終了する。
            await restoreRequest.WaitAsync(TimeSpan.FromSeconds(5));
        }
        finally
        {
            try { await fixture.DisposeAsync(); }
            finally { DeleteTestRegistryRoot(registryRoot); }
        }
    }

    private static void DeleteTestRegistryRoot(string? registryRoot)
    {
        if (string.IsNullOrWhiteSpace(registryRoot)) return;
        var fullRoot = Path.GetFullPath(registryRoot);
        var tempRoot = Path.TrimEndingDirectorySeparator(
            Path.GetFullPath(Path.GetTempPath()));
        var comparison = OperatingSystem.IsWindows()
            ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
        if (!string.Equals(Path.GetDirectoryName(fullRoot), tempRoot, comparison)
            || !Path.GetFileName(fullRoot).StartsWith(
                "amt-trim-registry-", StringComparison.Ordinal))
            throw new InvalidOperationException("テスト用一時ルート以外は後片付けできません");
        if (Directory.Exists(fullRoot))
        {
            // 責務: このテストfixtureが生成したGUID付き一時ルートを後片付けする。
            Directory.Delete(fullRoot, recursive: true);
        }
    }
}
