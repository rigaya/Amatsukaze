using System.Reflection;
using System.Runtime.CompilerServices;
using Amatsukaze.Server;
using Amatsukaze.Server.Rest;

namespace AmatsukazeServerTest;

internal sealed class RestQueueTestFixture
{
    public EncodeServer Server { get; }
    public RestStateStore Store { get; }
    public QueueManager Manager { get; }

    public RestQueueTestFixture()
    {
        // 実設定の読み込み、ネイティブDLL、待受け、ワーカー起動を避ける。
        // このfixtureは並べ替え、通知、入力拒否だけに使用する。
        // 必須依存: Clientは通知先、scheduledQueueは再スケジュール、
        // server・queueSync・Queueはキュー操作、queueManagerはRESTからの委譲に必要。
        // workerPoolは保守停止状態とキャンセル判定に必要。ワーカーは登録しない。
        Server = (EncodeServer)RuntimeHelpers.GetUninitializedObject(typeof(EncodeServer));
        Store = new RestStateStore(Server);
        typeof(EncodeServer).GetProperty("Client", BindingFlags.Instance | BindingFlags.NonPublic)!
            .SetValue(Server, Store);
        SetField(Server, "scheduledQueue", new ScheduledQueue());
        SetField(Server, "workerPool", new WorkerPool());
        Manager = (QueueManager)RuntimeHelpers.GetUninitializedObject(typeof(QueueManager));
        SetField(Manager, "server", Server);
        SetField(Manager, "queueSync", new object());
        typeof(QueueManager).GetProperty("Queue")!.SetValue(Manager, new List<QueueItem>());
        SetField(Server, "queueManager", Manager);
    }

    public RestApiHost CreateRestHost()
    {
        // REST登録はserverとstateだけを使用する。画像処理サービスの生成は省略する。
        var host = (RestApiHost)RuntimeHelpers.GetUninitializedObject(typeof(RestApiHost));
        SetField(host, "server", Server);
        SetField(host, "state", Store);
        return host;
    }

    public static void SetField(object target, string name, object value) =>
        (target.GetType().GetField(name, BindingFlags.Instance | BindingFlags.NonPublic)
            ?? throw new InvalidOperationException($"必須フィールドが見つかりません: {target.GetType().Name}.{name}"))
            .SetValue(target, value);

    public async Task Seed(params QueueItem[] items)
    {
        Manager.Queue.AddRange(items);
        await Store.OnUIData(new UIData { QueueData = new QueueData { Items = items.ToList() } });
    }

    public static QueueItem Item(int id, QueueState state = QueueState.Queue) => new()
    {
        Id = id, SrcPath = $"/架空の入力/番組{id}.ts", DstPath = "/架空の出力/video",
        State = state, Mode = ProcMode.Batch, ImageWidth = 1920, ImageHeight = 1080,
        ProfileName = "標準", ServiceName = "放送局", Tags = new List<string> { "録画" },
        Priority = 3,
    };
}
