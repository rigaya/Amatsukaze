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

        public static async Task<UpdateExecutableProbeResult> RunAsync(string executable,
            string argument, CancellationToken cancellationToken)
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
            Process process = null;
            try
            {
                process = Process.Start(start);
                if (process == null)
                {
                    return new UpdateExecutableProbeResult(-1, string.Empty, true);
                }
                using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
                timeout.CancelAfter(ProbeTimeout);
                var stdout = process.StandardOutput.ReadToEndAsync(timeout.Token);
                var stderr = process.StandardError.ReadToEndAsync(timeout.Token);
                await process.WaitForExitAsync(timeout.Token).ConfigureAwait(false);
                return new UpdateExecutableProbeResult(process.ExitCode,
                    (await stdout.ConfigureAwait(false)) + "\n" +
                    (await stderr.ConfigureAwait(false)), false);
            }
            catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
            {
                KillTree(process);
                return new UpdateExecutableProbeResult(-1, "タイムアウト", true);
            }
            catch (OperationCanceledException)
            {
                KillTree(process);
                throw;
            }
            catch (Exception ex)
            {
                KillTree(process);
                return new UpdateExecutableProbeResult(-1,
                    ex.GetType().Name + ":" + ex.Message, true);
            }
            finally
            {
                process?.Dispose();
            }
        }

        private static void KillTree(Process process)
        {
            try
            {
                if (process != null && !process.HasExited)
                {
                    process.Kill(entireProcessTree: true);
                }
            }
            catch
            {
                // 終了競合は無視する。
            }
        }
    }

    internal sealed record UpdateExecutableProbeResult(int ExitCode, string Output,
        bool LaunchFailed);
}
