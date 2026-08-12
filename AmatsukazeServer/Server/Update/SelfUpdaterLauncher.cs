using System;
using System.Diagnostics;
using System.IO;
using System.Threading;
using System.Threading.Tasks;

namespace Amatsukaze.Server.Update
{
    // サーバーから独立して生存する updater を起動し、待機開始の通知を確認する。
    internal static class SelfUpdaterLauncher
    {
        private const int UpdaterReadyTimeoutSeconds = 10;
        private const int ReadyPollMilliseconds = 100;

        public static async Task StartAndWaitReadyAsync(GeneratedSelfUpdater updater,
            UpdateOSKind os, CancellationToken cancellationToken,
            TimeSpan? readyTimeout = null, string launcherOverride = null)
        {
            ArgumentNullException.ThrowIfNull(updater);
            try
            {
                if (File.Exists(updater.ReadyPath)) File.Delete(updater.ReadyPath);
                // setsid は条件によって fork するため、起動時の Process は生死判定に使えない。
                // 起動できたことだけを確認してハンドルを離し、ready ファイルだけを待つ。
                using (Start(updater.ScriptPath, os, launcherOverride)) { }
                var timeout = readyTimeout ?? TimeSpan.FromSeconds(UpdaterReadyTimeoutSeconds);
                var started = Stopwatch.StartNew();
                while (started.Elapsed < timeout)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    if (File.Exists(updater.ReadyPath)) return;
                    await Task.Delay(ReadyPollMilliseconds, cancellationToken)
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

        private static Process Start(string scriptPath, UpdateOSKind os,
            string launcherOverride)
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
