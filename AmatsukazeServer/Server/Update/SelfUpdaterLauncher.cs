using System;
using System.Diagnostics;
using System.IO;
using System.Threading;
using System.Threading.Tasks;

namespace Amatsukaze.Server.Update
{
    internal sealed record SelfUpdaterReadyTiming(TimeProvider Clock, TimeSpan Timeout, TimeSpan PollInterval)
    {
        public static SelfUpdaterReadyTiming Default { get; } =
            new(TimeProvider.System, TimeSpan.FromSeconds(10), TimeSpan.FromMilliseconds(100));
    }

    // サーバーから独立して生存する updater を起動し、待機開始の通知を確認する。
    internal static class SelfUpdaterLauncher
    {
        public static Task StartAndWaitReadyAsync(GeneratedSelfUpdater updater,
            UpdateOSKind os, CancellationToken cancellationToken) =>
            StartAndWaitReadyAsync(updater, os, cancellationToken, SelfUpdaterReadyTiming.Default,
                static (path, targetOs) =>
                {
                    // setsidはforkすることがある。起動に使ったハンドルでupdaterの生死を判定しない。
                    using (Start(path, targetOs)) { }
                    return Task.CompletedTask;
                });

        internal static async Task StartAndWaitReadyAsync(GeneratedSelfUpdater updater,
            UpdateOSKind os, CancellationToken cancellationToken, SelfUpdaterReadyTiming timing,
            Func<string, UpdateOSKind, Task> launch)
        {
            ArgumentNullException.ThrowIfNull(updater);
            try
            {
                if (File.Exists(updater.ReadyPath)) File.Delete(updater.ReadyPath);
                await launch(updater.ScriptPath, os).ConfigureAwait(false);
                var started = timing.Clock.GetTimestamp();
                while (timing.Timeout == Timeout.InfiniteTimeSpan || timing.Clock.GetElapsedTime(started) < timing.Timeout)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    if (File.Exists(updater.ReadyPath)) return;
                    await Task.Delay(timing.PollInterval, timing.Clock, cancellationToken)
                        .ConfigureAwait(false);
                }
                throw new UpdateInstallException("UPDATER_START_FAILED", "S01_PRECHECK",
                    "本体 updater の起動を確認できませんでした");
            }
            catch (OperationCanceledException)
            {
                throw;
            }
            catch (UpdateInstallException)
            {
                throw;
            }
            catch (Exception ex)
            {
                throw new UpdateInstallException("UPDATER_START_FAILED", "S01_PRECHECK",
                    "本体 updater を起動できませんでした", ex);
            }
        }

        internal static Process Start(string scriptPath, UpdateOSKind os,
            string launcherOverride = null)
        {
            var start = new ProcessStartInfo
            {
                UseShellExecute = false,
                CreateNoWindow = true,
            };
            if (os == UpdateOSKind.Windows)
            {
                start.FileName = launcherOverride ?? "cmd.exe";
                start.ArgumentList.Add("/d");
                start.ArgumentList.Add("/s");
                start.ArgumentList.Add("/c");
                // サーバーの標準ハンドルを継承させず、updater 自身のログだけを残す。
                start.ArgumentList.Add("call \"" + scriptPath + "\" >NUL 2>&1");
            }
            else
            {
                var setsid = launcherOverride ?? FindExecutable("setsid");
                start.FileName = "bash";
                start.ArgumentList.Add("-c");
                start.ArgumentList.Add("exec </dev/null >/dev/null 2>&1; exec \"$@\"");
                start.ArgumentList.Add("amatsukaze-updater-launcher");
                // setsid が無い最小環境でも更新できるよう、bash 直接起動へフォールバックする。
                if (!string.IsNullOrEmpty(setsid))
                {
                    start.ArgumentList.Add(setsid);
                    start.ArgumentList.Add("bash");
                }
                else
                {
                    start.ArgumentList.Add("bash");
                }
                start.ArgumentList.Add(scriptPath);
            }
            return Process.Start(start) ?? throw new InvalidOperationException(
                "本体 updater プロセスを生成できませんでした");
        }

        private static string FindExecutable(string name)
        {
            foreach (var directory in (Environment.GetEnvironmentVariable("PATH") ?? string.Empty)
                .Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
            {
                try
                {
                    var candidate = Path.Combine(directory, name);
                    if (File.Exists(candidate)) return candidate;
                }
                catch { }
            }
            return null;
        }
    }
}
