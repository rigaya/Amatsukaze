using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;

namespace Amatsukaze.Server.Update
{
    internal sealed class LocalVersionInfo
    {
        public string Version { get; init; }
        public string Path { get; init; }
        public string FirstOutputLine { get; init; }
        public int? ExitCode { get; init; }
        public bool TimedOut { get; init; }
        public bool NotInstalled { get; init; }
    }

    internal static class VersionProbe
    {
        private static readonly TimeSpan ProbeTimeout = TimeSpan.FromSeconds(5);

        public static async Task<LocalVersionInfo> ProbeAsync(UpdateTargetDef target,
            EncodeServer server, Setting setting, UpdateLog log, CancellationToken cancellationToken)
        {
            if (target.IsApplication)
            {
                var path = GetCurrentProcessPath();
                var appMetadata = GetFileMetadata(path);
                var appVersion = string.IsNullOrWhiteSpace(server.Version) ? null : server.Version;
                log.Write(target.Id, "S02_LOCAL_VERSION", appVersion == null ? "NG" : "OK",
                    ("method", "assembly"),
                    ("path", path),
                    ("out", appVersion),
                    ("version", appVersion ?? "Unknown"),
                    ("size", appMetadata.Size),
                    ("modified", appMetadata.Modified));
                return new LocalVersionInfo
                {
                    Version = appVersion,
                    Path = path,
                    FirstOutputLine = appVersion,
                };
            }

            var executablePath = target.GetExecutablePath(setting);
            // 設定が裸のコマンド名でも PATH を解決してから実体の有無を見る。
            // 解決できない = どこにも無い、なので未インストールとして扱う。
            var installedPath = string.IsNullOrWhiteSpace(executablePath)
                ? null : ResolveExecutablePath(executablePath);
            if (string.IsNullOrWhiteSpace(installedPath) || !File.Exists(installedPath))
            {
                log.Write(target.Id, "S02_LOCAL_VERSION", "SKIP",
                    ("method", "exec"),
                    ("path", string.IsNullOrWhiteSpace(executablePath) ? "?" : executablePath),
                    ("cmd", target.VersionArgument),
                    ("version", "Unknown"),
                    ("reason", "not_installed"));
                return new LocalVersionInfo
                {
                    Version = null,
                    Path = executablePath,
                    NotInstalled = true,
                };
            }
            using var process = new Process
            {
                StartInfo = new ProcessStartInfo
                {
                    FileName = executablePath,
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                },
            };
            process.StartInfo.ArgumentList.Add(target.VersionArgument);
            try
            {
                if (!process.Start())
                {
                    throw new InvalidOperationException("プロセスを開始できませんでした");
                }
            }
            catch (Exception ex)
            {
                log.Write(target.Id, "S02_LOCAL_VERSION", "NG",
                    ("method", "exec"),
                    ("path", executablePath),
                    ("cmd", target.VersionArgument),
                    ("version", "Unknown"),
                    ("type", ex.GetType().Name),
                    ("reason", ex.Message));
                return new LocalVersionInfo { Version = null, Path = executablePath };
            }

            var stdoutTask = process.StandardOutput.ReadToEndAsync();
            var stderrTask = process.StandardError.ReadToEndAsync();
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            timeout.CancelAfter(ProbeTimeout);
            var timedOut = false;
            try
            {
                await process.WaitForExitAsync(timeout.Token).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
            {
                timedOut = true;
                try
                {
                    process.Kill(true);
                }
                catch
                {
                }
                try
                {
                    await process.WaitForExitAsync(CancellationToken.None).WaitAsync(TimeSpan.FromSeconds(1))
                        .ConfigureAwait(false);
                }
                catch
                {
                }
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                try
                {
                    process.Kill(true);
                }
                catch
                {
                }
                throw;
            }

            cancellationToken.ThrowIfCancellationRequested();
            string stdout;
            string stderr;
            try
            {
                stdout = await stdoutTask.WaitAsync(TimeSpan.FromSeconds(1)).ConfigureAwait(false);
            }
            catch
            {
                stdout = string.Empty;
            }
            try
            {
                stderr = await stderrTask.WaitAsync(TimeSpan.FromSeconds(1)).ConfigureAwait(false);
            }
            catch
            {
                stderr = string.Empty;
            }
            var combined = stdout + Environment.NewLine + stderr;
            var firstLine = combined.Split(new[] { "\r\n", "\n", "\r" },
                    StringSplitOptions.RemoveEmptyEntries)
                .Select(line => line.Trim())
                .FirstOrDefault(line => line.Length > 0);
            var match = timedOut ? null : target.VersionRegex?.Match(combined);
            var version = match?.Success == true ? match.Groups["ver"].Value : null;
            var resolvedPath = ResolveExecutablePath(executablePath);
            var metadata = GetFileMetadata(resolvedPath);
            int? exitCode = null;
            try
            {
                if (process.HasExited)
                {
                    exitCode = process.ExitCode;
                }
            }
            catch
            {
            }
            log.Write(target.Id, "S02_LOCAL_VERSION", version == null ? "NG" : "OK",
                ("method", "exec"),
                ("path", executablePath),
                ("cmd", target.VersionArgument),
                ("out", firstLine),
                ("version", version ?? "Unknown"),
                ("exit", exitCode),
                ("timeout", timedOut ? "yes" : "no"),
                ("size", metadata.Size),
                ("modified", metadata.Modified));
            return new LocalVersionInfo
            {
                Version = version,
                Path = executablePath,
                FirstOutputLine = firstLine,
                ExitCode = exitCode,
                TimedOut = timedOut,
            };
        }

        private static string GetCurrentProcessPath()
        {
            try
            {
                return Process.GetCurrentProcess().MainModule?.FileName ?? AppContext.BaseDirectory;
            }
            catch
            {
                return AppContext.BaseDirectory;
            }
        }

        private static string ResolveExecutablePath(string executablePath)
        {
            try
            {
                if (Path.IsPathRooted(executablePath) || executablePath.Contains(Path.DirectorySeparatorChar))
                {
                    return Path.GetFullPath(executablePath);
                }
                var path = Environment.GetEnvironmentVariable("PATH") ?? string.Empty;
                foreach (var directory in path.Split(Path.PathSeparator,
                    StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
                {
                    var candidate = Path.Combine(directory, executablePath);
                    if (File.Exists(candidate))
                    {
                        return candidate;
                    }
                }
            }
            catch
            {
            }
            return executablePath;
        }

        private static (long? Size, object Modified) GetFileMetadata(string path)
        {
            try
            {
                var file = new FileInfo(path);
                if (file.Exists)
                {
                    return (file.Length, file.LastWriteTimeUtc.ToString("O", CultureInfo.InvariantCulture));
                }
            }
            catch
            {
            }
            return (null, "?");
        }
    }
}
