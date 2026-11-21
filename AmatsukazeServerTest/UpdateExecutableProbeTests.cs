using Amatsukaze.Server.Update;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class UpdateExecutableProbeTests
{
    [Fact]
    public async Task バージョン取得が期限切れになるとタイムアウトを返す()
    {
        var clock = new FakeTimeProvider();
        var timers = clock.ObserveTimers();
        var process = new FakeProcess();
        var probe = UpdateExecutableProbe.RunAsync("tool", "--version", CancellationToken.None,
            TimeSpan.FromSeconds(5), clock, start =>
            {
                Assert.Equal("tool", start.FileName);
                Assert.Equal("--version", Assert.Single(start.ArgumentList));
                Assert.False(timers.TryRead(out _));
                return process;
            });

        Assert.True(process.Reading.Task.IsCompletedSuccessfully);
        Assert.True(timers.TryRead(out var timeout));
        Assert.Equal(TimeSpan.FromSeconds(5), timeout);
        Assert.False(probe.IsCompleted);
        clock.Advance(timeout);
        Assert.Equal(new UpdateExecutableProbeResult(-1, "タイムアウト", true), await probe);
        Assert.True(process.Disposed);
    }

    private sealed class FakeProcess : IUpdateExecutableProcess
    {
        public TaskCompletionSource Reading { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public bool Disposed { get; private set; }

        public async Task<UpdateExecutableProbeResult> ReadResultAsync(CancellationToken cancellationToken)
        {
            var stopped = new TaskCompletionSource<UpdateExecutableProbeResult>(
                TaskCreationOptions.RunContinuationsAsynchronously);
            using var registration = cancellationToken.Register(() => stopped.TrySetCanceled(cancellationToken));
            Reading.SetResult();
            return await stopped.Task;
        }

        public void Dispose() => Disposed = true;
    }
}
