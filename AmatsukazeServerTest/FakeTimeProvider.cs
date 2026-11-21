using System.Threading.Channels;

namespace AmatsukazeServerTest;

/// <summary>テストだけで使う手動時計。Advanceの呼び出し元で期限順にコールバックを実行する。</summary>
internal sealed class FakeTimeProvider : TimeProvider
{
    private readonly object _gate = new();
    private readonly List<ManualTimer> _timers = new();
    private Channel<TimeSpan>? _timerRegistrations;
    private readonly DateTimeOffset _origin = DateTimeOffset.UnixEpoch;
    private long _ticks;
    private long _nextOrder;

    public override long TimestampFrequency => TimeSpan.TicksPerSecond;

    public override DateTimeOffset GetUtcNow()
    {
        lock (_gate) return _origin.AddTicks(_ticks);
    }

    public override long GetTimestamp()
    {
        lock (_gate) return _ticks;
    }

    public override ITimer CreateTimer(TimerCallback callback, object? state, TimeSpan dueTime, TimeSpan period)
    {
        ArgumentNullException.ThrowIfNull(callback);
        var timer = new ManualTimer(this, callback, state);
        lock (_gate)
        {
            timer.Change(dueTime, period);
            _timers.Add(timer);
            if (dueTime != Timeout.InfiniteTimeSpan) _timerRegistrations?.Writer.TryWrite(dueTime);
        }
        return timer;
    }

    /// <summary>これ以降に登録される有限タイマーの初期期限を、登録順に受け取る。</summary>
    public ChannelReader<TimeSpan> ObserveTimers()
    {
        lock (_gate)
            return (_timerRegistrations ??= Channel.CreateUnbounded<TimeSpan>()).Reader;
    }

    /// <summary>指定時間だけ進める。期限が同じタイマーは登録・変更順に呼び出す。</summary>
    public void Advance(TimeSpan delta)
    {
        ArgumentOutOfRangeException.ThrowIfLessThan(delta, TimeSpan.Zero);
        long target;
        lock (_gate) target = checked(_ticks + delta.Ticks);
        while (true)
        {
            ManualTimer? next;
            lock (_gate)
            {
                next = _timers.Where(t => !t.Disposed && t.Due != long.MaxValue && t.Due <= target)
                    .OrderBy(t => t.Due).ThenBy(t => t.Order).FirstOrDefault();
                if (next == null)
                {
                    _ticks = Math.Max(_ticks, target);
                    return;
                }
                _ticks = Math.Max(_ticks, next.Due);
                // 呼び出し前に次の期限を設定し、コールバック内のChangeとDisposeを優先する。
                next.Due = next.Period > 0 ? checked(next.Due + next.Period) : long.MaxValue;
                next.Order = _nextOrder++;
            }
            next.Callback(next.State);
        }
    }

    private sealed class ManualTimer : ITimer
    {
        private readonly FakeTimeProvider _owner;
        internal readonly TimerCallback Callback;
        internal readonly object? State;
        internal bool Disposed;
        internal long Due = long.MaxValue;
        internal long Period;
        internal long Order;

        internal ManualTimer(FakeTimeProvider owner, TimerCallback callback, object? state)
        {
            _owner = owner;
            Callback = callback;
            State = state;
        }

        public bool Change(TimeSpan dueTime, TimeSpan period)
        {
            Validate(dueTime);
            Validate(period);
            lock (_owner._gate)
            {
                if (Disposed) return false;
                Due = dueTime == Timeout.InfiniteTimeSpan ? long.MaxValue : checked(_owner._ticks + dueTime.Ticks);
                Period = period == Timeout.InfiniteTimeSpan ? 0 : period.Ticks;
                Order = _owner._nextOrder++;
                return true;
            }
        }

        public void Dispose()
        {
            lock (_owner._gate)
            {
                Disposed = true;
                Due = long.MaxValue;
                _owner._timers.Remove(this);
            }
        }

        public ValueTask DisposeAsync()
        {
            Dispose();
            return ValueTask.CompletedTask;
        }

        private static void Validate(TimeSpan value)
        {
            if (value < TimeSpan.Zero && value != Timeout.InfiniteTimeSpan)
                throw new ArgumentOutOfRangeException(nameof(value));
        }
    }
}
