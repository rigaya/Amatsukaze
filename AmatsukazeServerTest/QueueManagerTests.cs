using Amatsukaze.Server;
using Amatsukaze.Shared;
using Xunit;
using ChangeItemData = Amatsukaze.Server.ChangeItemData;
using ChangeItemType = Amatsukaze.Server.ChangeItemType;
using ProcMode = Amatsukaze.Server.ProcMode;
using OutputInfo = Amatsukaze.Server.OutputInfo;
using AddQueueRequest = Amatsukaze.Server.AddQueueRequest;

namespace AmatsukazeServerTest;

public sealed class QueueManagerTests
{
    [Theory]
    [InlineData(new[] { 2, 4 }, 5, new[] { 1, 3, 5, 2, 4 })]
    [InlineData(new[] { 4, 2 }, 0, new[] { 4, 2, 1, 3, 5 })]
    [InlineData(new[] { 2, 4 }, 3, new[] { 1, 3, 2, 4, 5 })]
    [InlineData(new[] { 2, 2, 99 }, -10, new[] { 2, 1, 3, 4, 5 })]
    [InlineData(new[] { 3 }, 100, new[] { 1, 2, 4, 5, 3 })]
    [InlineData(new[] { 5, 4, 3, 2, 1 }, 2, new[] { 5, 4, 3, 2, 1 })]
    public async Task 複数移動は要求順と移動前の挿入位置を守る(int[] ids, int drop, int[] expected)
    {
        var fixture = new RestQueueTestFixture();
        await fixture.Seed(Enumerable.Range(1, 5).Select(i => RestQueueTestFixture.Item(i)).ToArray());
        await fixture.Manager.MoveItems(new QueueMoveManyRequest { ItemIds = ids.ToList(), DropIndex = drop });
        Assert.Equal(expected, fixture.Manager.Queue.Select(i => i.Id));
        Assert.Equal(Enumerable.Range(0, 5), fixture.Manager.Queue.Select(i => i.Order));
        Assert.Equal(expected, fixture.Store.GetQueueView(null!).Items.Select(i => i.Id));
    }

    [Theory]
    [InlineData(null)]
    [InlineData(new int[0])]
    [InlineData(new[] { 99 })]
    public async Task 移動対象が無ければキューと通知版を変更しない(int[]? ids)
    {
        var fixture = new RestQueueTestFixture();
        await fixture.Seed(RestQueueTestFixture.Item(1));
        var version = fixture.Store.GetQueueView(null!).Version;
        await fixture.Manager.MoveItems(ids == null ? null! : new QueueMoveManyRequest { ItemIds = ids.ToList() });
        Assert.Equal(new[] { 1 }, fixture.Manager.Queue.Select(i => i.Id));
        Assert.Equal(version, fixture.Store.GetQueueView(null!).Version);
    }

    [Fact]
    public void スナップショットは無効項目を除外しリスト変更を隔離する()
    {
        var fixture = new RestQueueTestFixture();
        var item = RestQueueTestFixture.Item(1);
        fixture.Manager.Queue.AddRange(new[] { item, null!, new QueueItem { Id = 2, SrcPath = "" } });
        var snapshot = fixture.Manager.GetQueueSnapshot();
        Assert.Same(item, Assert.Single(snapshot));
        snapshot.Clear();
        Assert.Equal(3, fixture.Manager.Queue.Count);
    }

    [Fact]
    public async Task 単一移動と優先度変更を状態ストアへ通知する()
    {
        var fixture = new RestQueueTestFixture();
        await fixture.Seed(RestQueueTestFixture.Item(1), RestQueueTestFixture.Item(2));
        await fixture.Manager.ChangeItem(new ChangeItemData { ItemId = 1, ChangeType = ChangeItemType.Move, Position = 1 });
        await fixture.Manager.ChangeItem(new ChangeItemData { ItemId = 1, ChangeType = ChangeItemType.Priority, Priority = 5 });
        Assert.Equal(new[] { 2, 1 }, fixture.Manager.GetQueueSnapshot().Select(i => i.Id));
        Assert.Equal(5, fixture.Store.GetQueueView(null!).Items.Last().Priority);
        Assert.False(fixture.Store.GetLastOperationResult().IsFailed);
    }

    [Theory]
    [InlineData(0, ChangeItemType.ResetState, "指定されたアイテムが見つかりません")]
    [InlineData(1, ChangeItemType.ResetState, "エンコード中のアイテムはリトライできません")]
    [InlineData(1, ChangeItemType.UpdateProfile, "エンコード中のアイテムはプロファイル更新できません")]
    [InlineData(1, ChangeItemType.Profile, "エンコード中はプロファイル変更できません")]
    [InlineData(2, ChangeItemType.ResetState, "このアイテムは追加処理に失敗しているため操作できません")]
    [InlineData(2, ChangeItemType.Profile, "このアイテムはプロファイル変更できません")]
    [InlineData(3, ChangeItemType.Duplicate, "通常モードで追加されたアイテムは複製できません")]
    [InlineData(3, ChangeItemType.ForceStart, "待ち状態にないアイテムは開始できません")]
    public async Task 不正な状態の操作は拒否してキューを維持する(int id, ChangeItemType action, string error)
    {
        var fixture = new RestQueueTestFixture();
        await fixture.Seed(RestQueueTestFixture.Item(1, QueueState.Encoding),
            RestQueueTestFixture.Item(2, QueueState.PreFailed), RestQueueTestFixture.Item(3, QueueState.LogoPending));
        await fixture.Manager.ChangeItem(new ChangeItemData { ItemId = id, ChangeType = action });
        Assert.True(fixture.Store.GetLastOperationResult().IsFailed);
        Assert.Contains(error, fixture.Store.GetLastOperationResult().Message);
        Assert.Equal(new[] { QueueState.Encoding, QueueState.PreFailed, QueueState.LogoPending }, fixture.Manager.Queue.Select(i => i.State));
    }

    [Theory]
    [InlineData(-1)]
    [InlineData(int.MinValue)]
    public async Task 負の単一移動位置は通知せずキューを変更しない(int position)
    {
        var fixture = new RestQueueTestFixture();
        await fixture.Seed(RestQueueTestFixture.Item(1), RestQueueTestFixture.Item(2));
        var before = fixture.Store.GetQueueView(null!);
        var original = fixture.Manager.Queue.ToArray();
        await fixture.Manager.ChangeItem(new ChangeItemData { ItemId = 1, ChangeType = ChangeItemType.Move, Position = position });
        Assert.Equal(original, fixture.Manager.Queue);
        var after = fixture.Store.GetQueueView(null!);
        Assert.Equal(before.Version, after.Version);
        Assert.Equal(before.Digest, after.Digest);
        Assert.Null(fixture.Store.GetLastOperationResult());
    }

    [Fact]
    public async Task 範囲外の単一移動はキューを変更しない()
    {
        var fixture = new RestQueueTestFixture();
        await fixture.Seed(RestQueueTestFixture.Item(1));
        await fixture.Manager.ChangeItem(new ChangeItemData { ItemId = 1, ChangeType = ChangeItemType.Move, Position = 1 });
        Assert.True(fixture.Store.GetLastOperationResult().IsFailed);
        Assert.Equal(1, Assert.Single(fixture.Manager.Queue).Id);
    }

    [Theory]
    [InlineData(QueueState.Queue)]
    [InlineData(QueueState.LogoPending)]
    [InlineData(QueueState.Encoding)]
    public async Task アクティブ項目のキャンセルは状態と通知を更新する(QueueState state)
    {
        var fixture = new RestQueueTestFixture();
        await fixture.Seed(RestQueueTestFixture.Item(1, state));
        await fixture.Manager.ChangeItem(new ChangeItemData { ItemId = 1, ChangeType = ChangeItemType.Cancel });
        Assert.Equal(QueueState.Canceled, Assert.Single(fixture.Manager.Queue).State);
        Assert.Equal("Canceled", Assert.Single(fixture.Store.GetQueueView(null!).Items).State);
        Assert.False(fixture.Store.GetLastOperationResult().IsFailed);
    }

    [Fact]
    public async Task 完了項目のキャンセルは状態を変えず拒否する()
    {
        var fixture = new RestQueueTestFixture();
        await fixture.Seed(RestQueueTestFixture.Item(1, QueueState.Complete));
        await fixture.Manager.ChangeItem(new ChangeItemData { ItemId = 1, ChangeType = ChangeItemType.Cancel });
        Assert.Equal(QueueState.Complete, Assert.Single(fixture.Manager.Queue).State);
        Assert.True(fixture.Store.GetLastOperationResult().IsFailed);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task 出力なしまたは空の出力先は登録を拒否する(bool blank)
    {
        var fixture = new RestQueueTestFixture();
        await fixture.Manager.AddQueue(new AddQueueRequest
        {
            Mode = ProcMode.Batch,
            Outputs = blank ? new List<OutputInfo> { new() { DstPath = " " } } : new List<OutputInfo>(),
        });
        Assert.Empty(fixture.Manager.Queue);
        Assert.True(fixture.Store.GetLastOperationResult().IsFailed);
        Assert.Contains(blank ? "出力先ディレクトリが指定されていません" : "出力が1つもありません", fixture.Store.GetLastOperationResult().Message);
    }

    [Theory]
    [InlineData(0, "元のキューアイテムが見つかりません")]
    [InlineData(1, "完了していないキューアイテムは再投入できません")]
    [InlineData(2, "CM解析またはエンコード済みのキューアイテムだけ再投入できます")]
    [InlineData(3, "入力ファイルが見つかりません")]
    public async Task 再投入の前提不足は追加前に拒否する(int id, string error)
    {
        var fixture = new RestQueueTestFixture();
        var wrongMode = RestQueueTestFixture.Item(2, QueueState.Complete);
        wrongMode.Mode = ProcMode.Test;
        var empty = RestQueueTestFixture.Item(3, QueueState.Complete);
        empty.SrcPath = "";
        await fixture.Seed(RestQueueTestFixture.Item(1), wrongMode, empty);
        var ex = await Assert.ThrowsAsync<InvalidOperationException>(() =>
            fixture.Manager.RequeueTrimItem(id, "標準", 3, new List<string>(), "/架空の一時", false));
        Assert.Equal(error, ex.Message);
        Assert.Equal(3, fixture.Manager.Queue.Count);
    }
}
