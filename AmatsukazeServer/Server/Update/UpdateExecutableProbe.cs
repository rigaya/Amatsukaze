using System;
using System.Diagnostics;
using System.Threading;
using System.Threading.Tasks;

namespace Amatsukaze.Server.Update
{
    // 更新対象の実行ファイルを stdout / stderr の両方から検査する。
    internal static class UpdateExecutableProbe
    {
        private static readonly TimeSpan ProbeTimeout = TimeSpan.FromSeconds(5);

        public static Task<UpdateExecutableProbeResult> RunAsync(string executable,
            string argument, CancellationToken cancellationToken) =>
            RunAsync(executable, argument, cancellationToken, ProbeTimeout, TimeProvider.System);

        internal static Task<UpdateExecutableProbeResult> RunWithoutTimeoutAsync(string executable,
            string argument, CancellationToken cancellationToken) =>
            RunAsync(executable, argument, cancellationToken, Timeout.InfiniteTimeSpan, TimeProvider.System);

        internal static async Task<UpdateExecutableProbeResult> RunAsync(string executable,
            string argument, CancellationToken cancellationToken, TimeSpan timeout, TimeProvider clock,
            Func<ProcessStartInfo, IUpdateExecutableProcess> startProcess = null)
        {
            var start = new ProcessStartInfo(executable)
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
            };
            if (!string.IsNullOrWhiteSpace(argument))
            {
                start.ArgumentList.Add(argument);
            }

            try
            {
                using var process = (startProcess ?? StartProcess)(start);
                if (process == null)
                {
                    return new UpdateExecutableProbeResult(-1, string.Empty, true);
                }

                using var deadline = timeout == Timeout.InfiniteTimeSpan
                    ? null : new CancellationTokenSource(timeout, clock);
                using var linked = deadline == null
                    ? null : CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, deadline.Token);
                try
                {
                    return await process.ReadResultAsync(linked?.Token ?? cancellationToken)
                        .ConfigureAwait(false);
                }
                catch (OperationCanceledException) when (deadline?.IsCancellationRequested == true &&
                    !cancellationToken.IsCancellationRequested)
                {
                    return new UpdateExecutableProbeResult(-1, "タイムアウト", true);
                }
            }
            catch (OperationCanceledException)
            {
                throw;
            }
            catch (Exception ex)
            {
                return new UpdateExecutableProbeResult(-1,
                    ex.GetType().Name + ":" + ex.Message, true);
            }
        }

        private static IUpdateExecutableProcess StartProcess(ProcessStartInfo start)
        {
            var process = Process.Start(start);
            return process == null ? null : new ExecutableProcess(process);
        }

        private sealed class ExecutableProcess(Process process) : IUpdateExecutableProcess
        {
            public async Task<UpdateExecutableProbeResult> ReadResultAsync(CancellationToken cancellationToken)
            {
                var stdout = process.StandardOutput.ReadToEndAsync(cancellationToken);
                var stderr = process.StandardError.ReadToEndAsync(cancellationToken);
                try
                {
                    await Task.WhenAll(process.WaitForExitAsync(cancellationToken), stdout, stderr)
                        .ConfigureAwait(false);
                    return new UpdateExecutableProbeResult(process.ExitCode,
                        (await stdout.ConfigureAwait(false)) + "\n" +
                        (await stderr.ConfigureAwait(false)), false);
                }
                catch
                {
                    if (KillTree(process))
                    {
                        await process.WaitForExitAsync().ConfigureAwait(false);
                    }
                    throw;
                }
            }

            public void Dispose() => process.Dispose();
        }

        private static bool KillTree(Process process)
        {
            try
            {
                if (!process.HasExited)
                {
                    process.Kill(entireProcessTree: true);
                }

                return true;
            }
            catch
            {
                // 終了競合は無視する。
                return false;
            }
        }
    }

    internal interface IUpdateExecutableProcess : IDisposable
    {
        Task<UpdateExecutableProbeResult> ReadResultAsync(CancellationToken cancellationToken);
    }

    internal sealed record UpdateExecutableProbeResult(int ExitCode, string Output,
        bool LaunchFailed);
}
