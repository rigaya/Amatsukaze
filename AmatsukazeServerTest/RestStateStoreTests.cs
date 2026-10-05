using Amatsukaze.Server;
using Amatsukaze.Server.Rest;
using Amatsukaze.Shared;
using Xunit;
using State = Amatsukaze.Server.State;
using ProcMode = Amatsukaze.Server.ProcMode;

namespace AmatsukazeServerTest;

public sealed class RestStateStoreTests
{
    private static Task Update(RestStateStore store, UpdateType type, QueueItem item, int position = 0) =>
        store.OnUIData(new UIData { QueueUpdate = new QueueUpdate { Type = type, Item = item, Position = position } });

    [Fact]
    public async Task 受信キューと取得モデルを深くコピーして入力変更を隔離する()
    {
        var store = new RestStateStore(null!);
        var original = RestQueueTestFixture.Item(1);
        await Update(store, UpdateType.Add, original);
        original.ServiceName = "変更";
        original.Tags.Add("外部変更");
        Assert.True(store.TryGetQueueItem(1, out var copy));
        copy.ServiceName = "取得後変更";
        copy.Tags.Clear();
        Assert.True(store.TryGetQueueItem(1, out var second));
        Assert.Equal("放送局", second.ServiceName);
        Assert.Equal(new[] { "録画" }, second.Tags);
        Assert.False(store.TryGetQueueItem(99, out _));
    }

    [Theory]
    [InlineData("file", "番組2", 2)]
    [InlineData("service", "special", 2)]
    [InlineData("profile", "選択", 2)]
    [InlineData("FILE", "番組1", 1)]
    public async Task 検索対象を指定して大文字小文字を無視して絞り込む(string target, string search, int expected)
    {
        var store = new RestStateStore(null!);
        var second = RestQueueTestFixture.Item(2);
        second.ServiceName = "SPECIAL";
        second.Profile = new ProfileSetting { Name = "選択プロファイル" };
        await Update(store, UpdateType.Add, RestQueueTestFixture.Item(1));
        await Update(store, UpdateType.Add, second);
        var view = store.GetQueueView(new QueueFilter { Search = search, SearchTargets = new() { target } });
        Assert.Equal(expected, Assert.Single(view.Items).Id);
        Assert.Equal(2, view.Counters.Active);
    }

    [Fact]
    public async Task 状態日付と映像サイズ不足の失敗除外を組み合わせる()
    {
        var store = new RestStateStore(null!);
        var date = new DateTime(2026, 10, 5);
        foreach (var id in Enumerable.Range(1, 3))
        {
            var item = RestQueueTestFixture.Item(id, QueueState.Complete);
            item.EncodeStart = date.AddHours(id);
            item.EncodeTime = TimeSpan.FromMinutes(10);
            if (id == 3)
            {
                item.State = QueueState.PreFailed;
                item.FailReason = "映像が小さすぎます";
            }
            await Update(store, UpdateType.Add, item);
        }
        var view = store.GetQueueView(new QueueFilter
        {
            States = new() { "complete", "prefailed" }, DateFrom = date.AddHours(2),
            DateTo = date.AddHours(4), HideOneSeg = true,
        });
        Assert.Equal(2, Assert.Single(view.Items).Id);
        Assert.Equal(2, view.Counters.Complete);
        Assert.Equal(1, view.Counters.Failed);
        await store.OnCommonData(new CommonData { Setting = new Setting { HideOneSeg = true } });
        Assert.Equal(new[] { 1, 2 }, store.GetQueueView(null!).Items.Select(i => i.Id));
    }

    [Theory]
    [InlineData(0, "待ち", true, false)]
    [InlineData(1, "エンコード中→1", true, false)]
    [InlineData(2, "完了", false, true)]
    [InlineData(3, "失敗", false, false)]
    [InlineData(4, "失敗", false, false)]
    [InlineData(5, "ペンディング", true, false)]
    [InlineData(6, "キャンセル", false, false)]
    public async Task 状態を表示ラベルと操作可否とカウンターへ変換する(int state, string label, bool active, bool trim)
    {
        var store = new RestStateStore(null!);
        var item = RestQueueTestFixture.Item(1, (QueueState)state);
        await Update(store, UpdateType.Add, item);
        var view = store.GetQueueView(null!);
        var dto = Assert.Single(view.Items);
        Assert.Equal(label, dto.StateLabel);
        Assert.Equal(trim, dto.CanTrimAdjust);
        Assert.Equal(active ? 1 : 0, view.Counters.Active);
        Assert.Equal(item.State.ToString(), dto.State);
        Assert.Null(dto.EncodeStart);
        Assert.Null(dto.EncodeFinish);
        Assert.Equal("番組1.ts", dto.FileName);
        Assert.Equal(Amatsukaze.Shared.ProcMode.Batch, dto.Mode);
    }

    [Fact]
    public async Task キューの追加更新移動削除と全同期後の差分を返す()
    {
        var store = new RestStateStore(null!);
        var one = RestQueueTestFixture.Item(1);
        await Update(store, UpdateType.Add, one);
        var baseline = store.GetQueueView(null!);
        one.Priority = 5;
        await Update(store, UpdateType.Update, one);
        await Update(store, UpdateType.Add, RestQueueTestFixture.Item(2));
        await Update(store, UpdateType.Move, one, 1);
        await Update(store, UpdateType.Remove, one);
        var changes = store.GetQueueChanges(baseline.Version + 1);
        Assert.False(changes.FullSyncRequired);
        Assert.Equal(new[] { QueueChangeType.Add, QueueChangeType.Move, QueueChangeType.Remove }, changes.Changes.Select(c => c.Type));
        Assert.Equal(2, Assert.Single(store.GetQueueView(null!).Items).Id);
        Assert.NotEqual(baseline.Digest, store.GetQueueView(null!).Digest);
        Assert.True(store.GetQueueChanges(long.MaxValue).FullSyncRequired);
        await store.OnUIData(new UIData { QueueData = new QueueData { Items = new() { one } } });
        Assert.True(store.GetQueueChanges(changes.ToVersion).FullSyncRequired);
        await Update(store, UpdateType.Clear, one);
        Assert.Empty(store.GetQueueView(null!).Items);
        Assert.Empty(store.GetQueueChanges(store.GetQueueView(null!).Version).Changes);
    }

    [Fact]
    public async Task 差分保持上限を越えた古いカーソルには全同期を要求する()
    {
        var store = new RestStateStore(null!);
        var item = RestQueueTestFixture.Item(1);
        await Update(store, UpdateType.Add, item);
        for (var i = 0; i < 1001; i++) await Update(store, UpdateType.Update, item);
        Assert.True(store.GetQueueChanges(1).FullSyncRequired);
        var current = store.GetQueueView(null!).Version;
        Assert.False(store.GetQueueChanges(current).FullSyncRequired);
        Assert.Empty(store.GetQueueChanges(current).Changes);
    }

    [Fact]
    public async Task 操作コンテキストとエラーレベルでメッセージを絞り最新要求結果を返す()
    {
        var store = new RestStateStore(null!);
        using (OperationContextScope.Use(new OperationContext { Page = "queue", RequestId = "ABC", Action = "add", Source = "rest" }))
        {
            await store.OnOperationResult(new OperationResult { Message = "開始" });
            await store.OnOperationResult(new OperationResult { Message = "失敗", IsFailed = true });
        }
        await store.OnOperationResult(new OperationResult { Message = "別の通知" });
        var view = store.GetMessageChanges(0, "QUEUE", "ABC", new() { "error" }, 50);
        var error = Assert.Single(view.Items);
        Assert.Equal("失敗", error.Message);
        Assert.Equal("rest", error.Source);
        Assert.Equal("add", error.Action);
        Assert.True(store.TryGetMessageForRequestId("ABC", out var latest));
        Assert.Equal(error.Id, latest.Id);
        Assert.False(store.TryGetMessageForRequestId("abc", out _));
        Assert.False(store.TryGetMessageForRequestId(null!, out _));
        var truncated = store.GetMessageChanges(0, null!, null!, null!, 1);
        Assert.True(truncated.Truncated);
        Assert.Equal("別の通知", Assert.Single(truncated.Items).Message);
        Assert.Equal("server", truncated.Items[0].Source);
    }

    [Fact]
    public async Task プロファイルのコピー改名削除と一覧の隔離を検証する()
    {
        var store = new RestStateStore(null!);
        var profile = new ProfileSetting { Name = "標準", X264Option = "フォント" };
        await store.OnProfile(new ProfileUpdate { Type = UpdateType.Add, Profile = profile });
        profile.X264Option = "外部変更";
        Assert.Equal("フォント", Assert.Single(store.GetProfiles()).X264Option);
        await store.OnProfile(new ProfileUpdate { Type = UpdateType.Update, Profile = profile, NewName = "新名" });
        var fetched = Assert.Single(store.GetProfiles());
        Assert.Equal("新名", fetched.Name);
        fetched.Name = "取得後変更";
        Assert.Equal("新名", Assert.Single(store.GetProfiles()).Name);
        await store.OnProfile(new ProfileUpdate { Type = UpdateType.Remove, Profile = new ProfileSetting { Name = "新名" } });
        Assert.Empty(store.GetProfiles());
    }

    [Fact]
    public async Task 未割当のコンソール通知をタスク割当後に再生して削除時に片付ける()
    {
        var store = new RestStateStore(null!);
        await store.OnUIData(new UIData { ConsoleData = new ConsoleData { index = 7, text = new() { "saved" } } });
        await store.OnConsoleUpdate(new ConsoleUpdate { index = 7, data = System.Text.Encoding.ASCII.GetBytes("live\n") });
        var item = RestQueueTestFixture.Item(1, QueueState.Encoding);
        item.ConsoleId = 7;
        item.EncodeStart = new DateTime(2026, 10, 5);
        await Update(store, UpdateType.Add, item);
        Assert.True(store.TryGetConsoleTaskView(1, out var view));
        Assert.Equal(new[] { "saved", "live" }, view.Lines);
        Assert.NotNull(view.Task);
        Assert.Equal(1, view.Task.TaskId);
        view.Lines.Clear();
        Assert.True(store.TryGetConsoleTaskView(1, out var second));
        Assert.Equal(2, second.Lines.Count);
        await Update(store, UpdateType.Remove, item);
        Assert.False(store.TryGetConsoleTaskChanges(1, 0, out _));
    }

    [Theory]
    [InlineData(false, false, false, "停止")]
    [InlineData(true, false, false, "エンコード中")]
    [InlineData(true, true, false, "一時停止中")]
    [InlineData(true, false, true, "一時停止中")]
    public async Task サーバー状態とエラー通知を概要へ反映する(bool running, bool suspend, bool scheduled, string label)
    {
        var store = new RestStateStore(null!);
        await store.OnUIData(new UIData { State = new State { Running = running, Suspend = suspend, ScheduledSuspend = scheduled } });
        await store.OnOperationResult(new OperationResult { Message = "問題", IsFailed = true });
        var summary = store.GetSystemSnapshot().StatusSummary;
        Assert.Equal(label, summary.RunningStateLabel);
        Assert.True(summary.IsError);
        Assert.Equal("問題", summary.LastOperationMessage);
    }
}
