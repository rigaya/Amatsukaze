using Amatsukaze.Server.Rest;
using Amatsukaze.Shared;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class TaskConsoleStateTests
{
    [Fact]
    public void 空のコンソールの置換は追加になり続く置換は最終行だけを変える()
    {
        var state = new TaskConsoleState(new ConsoleTaskInfo { TaskId = 7 });
        state.OnReplaceLine("開始");
        state.OnAddLine("進捗0%");
        state.OnReplaceLine("進捗100%");
        Assert.Equal(new[] { "開始", "進捗100%" }, state.Lines);
        var changes = state.GetChanges(1);
        Assert.False(changes.FullSyncRequired);
        Assert.Equal(new[] { ConsoleChangeType.Add, ConsoleChangeType.Replace }, changes.Changes.Select(c => c.Type));
        Assert.Equal(3, changes.ToVersion);
        Assert.True(state.GetChanges(100).FullSyncRequired);
        Assert.Empty(state.GetChanges(3).Changes);
    }

    [Fact]
    public void 保存ログからライブへ切り替えるとクリアを一度通知する()
    {
        var state = new TaskConsoleState(new ConsoleTaskInfo { TaskId = 1 });
        state.LoadFallbackLines(new() { "保存ログ" });
        Assert.True(state.IsFallback);
        state.ResetForLive();
        state.ResetForLive();
        Assert.False(state.IsFallback);
        Assert.Empty(state.Lines);
        Assert.Equal(1, state.Version);
        state.OnAddLine("ライブ");
        Assert.Equal("ライブ", Assert.Single(state.Lines));
        Assert.Equal(ConsoleChangeType.Add, Assert.Single(state.GetChanges(1).Changes).Type);
    }

    [Fact]
    public void 行数と差分数を制限して古いカーソルには全同期を要求する()
    {
        var state = new TaskConsoleState(new ConsoleTaskInfo { TaskId = 1 });
        var count = Math.Max(ConsoleConstants.MaxConsoleLines, ConsoleConstants.MaxConsoleChanges) + 100;
        for (var i = 0; i < count; i++) state.OnAddLine(i.ToString());
        Assert.InRange(state.Lines.Count, 1, ConsoleConstants.MaxConsoleLines);
        Assert.Equal((count - 1).ToString(), state.Lines.Last());
        Assert.True(state.GetChanges(1).FullSyncRequired);
        Assert.Empty(state.GetChanges(state.Version).Changes);
        state.SetTextLines(new() { "同期済み" });
        Assert.Equal(0, state.Version);
        Assert.False(state.GetChanges(0).FullSyncRequired);
        Assert.Equal("同期済み", Assert.Single(state.Lines));
    }
}
