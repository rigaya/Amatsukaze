using System.Collections.Specialized;
using System.Net;
using System.Net.Sockets;
using System.Reflection;
using System.Runtime.CompilerServices;
using Amatsukaze.Server;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class ClientManagerTests
{
    [Fact]
    public void 通知音の準備に失敗しても呼び出し元に例外を伝播しない()
    {
        // 設定ファイルの読み込みやサーバー起動を避け、通知音に必要な依存だけを用意する。
        var server = (EncodeServer)RuntimeHelpers.GetUninitializedObject(typeof(EncodeServer));
        typeof(EncodeServer).GetProperty("Client", BindingFlags.Instance | BindingFlags.NonPublic)!
            .SetValue(server, new ClientManager(null!));
        var playSound = typeof(EncodeServer).GetMethod("PlaySound", BindingFlags.Instance | BindingFlags.NonPublic)!;

        // 空の接続一覧で再生準備へ進み、パス生成時の例外を意図的に発生させる。
        var exception = Record.Exception(() => playSound.Invoke(server, new object?[] { null }));

        Assert.Null(exception);
    }

    [Fact]
    public async Task 切断後も接続時のアドレスを参照できる()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        using var peer = new TcpClient();
        await peer.ConnectAsync((IPEndPoint)listener.LocalEndpoint);
        using var accepted = await listener.AcceptTcpClientAsync();
        var client = new Client(accepted, new ClientManager(null!));
        var remote = client.RemoteIP;
        var local = client.LocalIP;

        // 送信側と受信側が同時に切断を検出しても、二重の終了処理は安全であること。
        await Task.WhenAll(Enumerable.Range(0, 8).Select(_ => Task.Run(client.Close)));

        Assert.Equal(remote, client.RemoteIP);
        Assert.Equal(local, client.LocalIP);
    }

    [Fact]
    public async Task 受信開始直後に切断しても登録を正常に解除できる()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        using var peer = new TcpClient();
        await peer.ConnectAsync((IPEndPoint)listener.LocalEndpoint);
        using var accepted = await listener.AcceptTcpClientAsync();
        var manager = new ClientManager(null!);
        var client = new Client(accepted, manager);
        manager.ClientList.Add(client);

        // Start が最初の await で中断せずに終了する経路を確実に通す。
        client.Close();
        await client.Start();

        Assert.Empty(manager.ClientList);
        Assert.False(manager.HasLocalClient());
    }

    [Fact]
    public async Task 接続の登録と切断が通知および終了と重なっても例外にならない()
    {
        const int connectionCount = 32;
        var manager = new ClientManager(null!);
        using var registered = new SemaphoreSlim(0);
        var allRemoved = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var removedCount = 0;
        manager.ClientList.CollectionChanged += (_, change) =>
        {
            if (change.Action == NotifyCollectionChangedAction.Add)
                registered.Release();
            if (change.Action == NotifyCollectionChangedAction.Remove &&
                Interlocked.Increment(ref removedCount) == connectionCount)
                allRemoved.TrySetResult();
        };

        var ready = new TaskCompletionSource<IPEndPoint>(TaskCreationOptions.RunContinuationsAsynchronously);
        var listening = manager.Listen(0, IPAddress.Loopback, endpoint => ready.TrySetResult(endpoint),
            retryOnFailure: false);
        var peers = new List<TcpClient>();
        var notifyDuringRegistration = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var registeringNotifications = NotifyAsync(notifyDuringRegistration.Task);

        try
        {
            await AwaitWhileListening(ready.Task, listening);
            var endpoint = await ready.Task;
            for (var i = 0; i < connectionCount; i++)
            {
                var peer = new TcpClient();
                peers.Add(peer);
                await peer.ConnectAsync(endpoint);
                await AwaitWhileListening(registered.WaitAsync(), listening);
                if (i == 0) notifyDuringRegistration.SetResult();
                // 登録と切断を交互に発生させ、一覧の読み取りと競合させる。
                if (i % 2 == 0)
                    peer.Dispose();
            }

            var begin = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            var finishingNotifications = NotifyAsync(begin.Task);
            var disconnecting = AfterBeginAsync(() =>
            {
                peers.ForEach(peer => peer.Dispose());
                return Task.CompletedTask;
            });
            var finishing = AfterBeginAsync(() =>
            {
                manager.Finish();
                return Task.CompletedTask;
            });
            begin.SetResult();
            await Task.WhenAll(registeringNotifications, finishingNotifications,
                disconnecting, finishing, listening);
            await allRemoved.Task;

            Assert.Empty(manager.ClientList);
            Assert.False(manager.HasLocalClient());

            async Task AfterBeginAsync(Func<Task> action)
            {
                await begin.Task;
                await action();
            }
        }
        finally
        {
            peers.ForEach(peer => peer.Dispose());
            manager.Finish();
            notifyDuringRegistration.TrySetResult();
            await Task.WhenAll(listening, registeringNotifications);
        }

        async Task NotifyAsync(Task begin)
        {
            await begin;
            for (var i = 0; i < connectionCount * 4; i++)
            {
                manager.HasLocalClient();
                manager.GetMacAddress();
                await manager.OnLogFile("接続競合の回帰テスト");
            }
        }
    }

    private static async Task AwaitWhileListening(Task milestone, Task listening)
    {
        if (await Task.WhenAny(milestone, listening) == listening)
        {
            await listening;
            Assert.Fail("接続の合図を受ける前に待ち受けが終了した");
        }
        await milestone;
    }
}
