using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;

namespace Amatsukaze.Server.Update
{
    internal sealed class UpdateStaging
    {
        private static readonly TimeSpan ProbeTimeout = TimeSpan.FromSeconds(5);
        // AV が展開直後のファイルを削除するケースを設置前に拾うための猶予。
        // 1秒は経験的な値であり、直後のバージョン実行が本命の検出手段になる。
        private static readonly TimeSpan AntivirusDeletionGrace = TimeSpan.FromSeconds(1);

        public async Task<PreparedUpdate> PrepareAsync(UpdateTargetDef target,
            ExtractionResult extraction, string expectedVersion, UpdateLog log,
            CancellationToken cancellationToken)
        {
            if (target.Payload == null || target.Payload.Length == 0)
            {
                throw new UpdatePreparationException("INVALID_CATALOG", "S09_STAGE",
                    "Payload が宣言されていません");
            }

            var matches = UpdatePayloadMatcher.FindMatches(extraction.DirectoryPath, target.Payload);
            if (matches.Count != 1)
            {
                log.Write(target.Id, "S09_STAGE", "NG", ("code", "EXTRACT_FAILED"),
                    ("payload_matches", matches.Count));
                throw new UpdatePreparationException("EXTRACT_FAILED", "S09_STAGE",
                    "配置対象ファイルを一意に特定できませんでした");
            }

            var selected = matches[0];
            if (!OperatingSystem.IsWindows())
            {
                var mode = File.GetUnixFileMode(selected.Path);
                File.SetUnixFileMode(selected.Path, mode | UnixFileMode.UserExecute);
            }
            await Task.Delay(AntivirusDeletionGrace, cancellationToken).ConfigureAwait(false);
            if (!File.Exists(selected.Path) || new FileInfo(selected.Path).Length == 0)
            {
                throw new UpdatePreparationException("VERIFY_FAILED", "S09_STAGE",
                    "配置対象ファイルが消失または空になりました");
            }

            var probe = await ProbeAsync(selected.Path, target.VersionArgument, cancellationToken)
                .ConfigureAwait(false);
            var match = target.VersionRegex?.Match(probe.Output ?? string.Empty);
            if (probe.ExitFailed || match?.Success != true)
            {
                log.Write(target.Id, "S09_STAGE", "NG", ("code", "VERIFY_FAILED"),
                    ("path", selected.Path), ("exit", probe.ExitCode),
                    ("output", probe.Output));
                throw new UpdatePreparationException("VERIFY_FAILED", "S09_STAGE",
                    "ステージング上の実行ファイルを検証できませんでした");
            }
            var actualVersion = match.Groups["ver"].Success
                ? match.Groups["ver"].Value : match.Value;
            if (!string.Equals(actualVersion, expectedVersion, StringComparison.OrdinalIgnoreCase))
            {
                log.Write(target.Id, "S09_STAGE", "NG", ("code", "VERIFY_FAILED"),
                    ("path", selected.Path), ("version", actualVersion),
                    ("expected", expectedVersion));
                throw new UpdatePreparationException("VERIFY_FAILED", "S09_STAGE",
                    "ステージング上のバージョンがリリース情報と一致しません");
            }

            var destinationName = selected.Payload.DestName ?? Path.GetFileName(selected.Path);
            log.Write(target.Id, "S09_STAGE", "OK", ("path", selected.Path),
                ("chmod", OperatingSystem.IsWindows() ? "n/a" : "u+x"),
                ("recheck", "ok"),
                ("probe", target.VersionArgument), ("exit", probe.ExitCode),
                ("probe_out", probe.Output),
                ("version", actualVersion), ("expected", expectedVersion),
                ("dest", destinationName));
            return new PreparedUpdate(target.Id, selected.Path, destinationName, actualVersion);
        }

        private static async Task<ProbeResult> ProbeAsync(string executable, string argument,
            CancellationToken cancellationToken)
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
                    return new ProbeResult(-1, string.Empty, true);
                }
                using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
                timeout.CancelAfter(ProbeTimeout);
                var stdout = process.StandardOutput.ReadToEndAsync(timeout.Token);
                var stderr = process.StandardError.ReadToEndAsync(timeout.Token);
                await process.WaitForExitAsync(timeout.Token).ConfigureAwait(false);
                return new ProbeResult(process.ExitCode,
                    (await stdout.ConfigureAwait(false)) + "\n" + (await stderr.ConfigureAwait(false)),
                    false);
            }
            catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
            {
                KillTree(process);
                return new ProbeResult(-1, "タイムアウト", true);
            }
            catch (OperationCanceledException)
            {
                KillTree(process);
                throw;
            }
            catch (Exception ex)
            {
                KillTree(process);
                return new ProbeResult(-1, ex.GetType().Name + ":" + ex.Message, true);
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

        private sealed record ProbeResult(int ExitCode, string Output, bool ExitFailed);
    }
}
