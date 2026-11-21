using System;
using System.Threading;
using System.Threading.Tasks;

namespace Amatsukaze.Server.Update
{
    internal sealed record UpdateHttpTiming(TimeProvider Clock, TimeSpan Timeout, TimeSpan RetryInterval)
    {
        public Deadline StartDeadline(CancellationToken cancellationToken) =>
            new Deadline(Clock, Timeout, cancellationToken);

        public Task WaitBeforeRetryAsync(int attempt, CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            return RetryInterval == TimeSpan.Zero ? Task.CompletedTask :
                Task.Delay(RetryInterval * attempt, Clock, cancellationToken);
        }

        internal readonly struct Deadline : IDisposable
        {
            private readonly CancellationTokenSource timer;
            private readonly CancellationTokenSource linked;

            public Deadline(TimeProvider clock, TimeSpan timeout, CancellationToken cancellationToken)
            {
                if (timeout == System.Threading.Timeout.InfiniteTimeSpan)
                {
                    Token = cancellationToken;
                    return;
                }

                timer = new CancellationTokenSource(timeout, clock);
                linked = cancellationToken.CanBeCanceled
                    ? CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, timer.Token) : null;
                Token = linked?.Token ?? timer.Token;
            }

            public CancellationToken Token { get; }

            public void Dispose()
            {
                linked?.Dispose();
                timer?.Dispose();
            }
        }
    }
}
