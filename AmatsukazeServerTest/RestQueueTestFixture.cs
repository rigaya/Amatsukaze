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
        SetProperty(Server, "Client", Store);
        SetField(Server, "scheduledQueue", new ScheduledQueue());
        SetField(Server, "workerPool", new WorkerPool());
        Manager = (QueueManager)RuntimeHelpers.GetUninitializedObject(typeof(QueueManager));
        SetField(Manager, "server", Server);
        SetField(Manager, "queueSync", new object());
        SetProperty(Manager, "Queue", new List<QueueItem>());
        SetField(Server, "queueManager", Manager);
    }

    public RestApiHost CreateRestHost()
    {
        // サービス生成自体はネイティブDLLを使わない。DisposeでTrimのタイマーを停止する。
        return new RestApiHost(Server, Store, 0);
    }

    // 補助APIの確認に必要な要求フラグと終了通知。フィールド名への依存をfixtureへ集約する。
    public bool AddQueueCanceled => (bool)GetField(Manager, "addQueueCanceled");
    public bool LogoRescanRequested => (bool)GetField(Server, "serviceListUpdated");
    public void SetFinishRequested(Action callback) => SetField(Server, "finishRequested", callback);
    // REST追加要求の変換だけを検証する。受信スレッドを起動せず、実TS解析には渡さない。
    public System.Threading.Tasks.Dataflow.BufferBlock<object> CaptureQueueRequests()
    {
        var requests = new System.Threading.Tasks.Dataflow.BufferBlock<object>();
        SetField(Server, "queueQ", requests);
        return requests;
    }

    public static object GetField(object target, string name) =>
        (target.GetType().GetField(name, BindingFlags.Instance | BindingFlags.NonPublic)
            ?? throw new InvalidOperationException($"必須フィールドが見つかりません: {target.GetType().Name}.{name}"))
            .GetValue(target) ?? throw new InvalidOperationException($"必須フィールドが未設定です: {name}");

    public static void SetProperty(object target, string name, object value) =>
        (target.GetType().GetProperty(name, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic)
            ?? throw new InvalidOperationException($"必須プロパティが見つかりません: {target.GetType().Name}.{name}"))
            .SetValue(target, value);

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
