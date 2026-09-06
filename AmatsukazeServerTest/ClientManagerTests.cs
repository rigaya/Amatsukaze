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
    private static readonly TimeSpan TestTimeout = TimeSpan.FromSeconds(15);

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
        await Task.WhenAll(Enumerable.Range(0, 8).Select(_ => Task.Run(client.Close)))
            .WaitAsync(TestTimeout);

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
        await client.Start().WaitAsync(TestTimeout);

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

        var listening = manager.Listen(0);
        // OS が選んだポートを使い、テスト間のポート取り合いを避ける。
        var listener = (TcpListener)typeof(ClientManager)
            .GetField("listener", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(manager)!;
        var endpoint = new IPEndPoint(IPAddress.Loopback, ((IPEndPoint)listener.LocalEndpoint).Port);
        var peers = new List<TcpClient>();
        var begin = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var notifications = Task.Run(async () =>
        {
            await begin.Task;
            for (var i = 0; i < connectionCount * 4; i++)
            {
                manager.HasLocalClient();
                manager.GetMacAddress();
                await manager.OnLogFile("接続競合の回帰テスト");
                await Task.Yield();
            }
        });

        try
        {
            begin.SetResult();
            for (var i = 0; i < connectionCount; i++)
            {
                var peer = new TcpClient();
                peers.Add(peer);
                await peer.ConnectAsync(endpoint).WaitAsync(TestTimeout);
                Assert.True(await registered.WaitAsync(TestTimeout), "接続が登録されませんでした。");
                // 登録と切断を交互に発生させ、一覧の読み取りと競合させる。
                if (i % 2 == 0)
                    peer.Dispose();
            }

            await Task.WhenAll(
                notifications,
                Task.Run(() => peers.ForEach(peer => peer.Dispose())),
                Task.Run(manager.Finish)).WaitAsync(TestTimeout);
            await allRemoved.Task.WaitAsync(TestTimeout);
            await listening.WaitAsync(TestTimeout);

            Assert.Empty(manager.ClientList);
            Assert.False(manager.HasLocalClient());
        }
        finally
        {
            peers.ForEach(peer => peer.Dispose());
            manager.Finish();
            await listening.WaitAsync(TestTimeout);
            await notifications.WaitAsync(TestTimeout);
        }
    }
}
