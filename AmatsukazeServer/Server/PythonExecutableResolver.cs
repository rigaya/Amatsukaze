using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;

namespace Amatsukaze.Server
{
    internal sealed class PythonExecutable
    {
        public string FileName { get; }
        public string LauncherArguments { get; }

        public PythonExecutable(string fileName, string launcherArguments)
        {
            FileName = fileName;
            LauncherArguments = launcherArguments;
        }

        public string PrependLauncherArguments(string arguments)
        {
            return string.IsNullOrEmpty(LauncherArguments)
                ? arguments
                : LauncherArguments + " " + arguments;
        }
    }

    internal static class PythonExecutableResolver
    {
        private const int ProbeTimeoutMilliseconds = 5000;
        private const string ProbeMarker = "AMATSUKAZE_PYTHON3_OK";

        public static PythonExecutable Resolve()
        {
            foreach (var candidate in EnumerateCandidates())
            {
                if (CanRunPython3(candidate))
                {
                    return candidate;
                }
            }
            return null;
        }

        public static PythonExecutable ResolveOrThrow(string purpose)
        {
            var candidates = OperatingSystem.IsWindows()
                ? "py.exe、python.exe、python3.exe"
                : "python3、python";
            return Resolve() ?? throw new InvalidOperationException(
                purpose + "の実行に必要なPython 3が見つかりません。" +
                "exe_filesにPythonを配置するか、" + candidates + "のいずれかにPATHを通してください。");
        }

        private static IEnumerable<PythonExecutable> EnumerateCandidates()
        {
            var isWindows = OperatingSystem.IsWindows();
            var localNames = isWindows
                ? new[] { "python.exe", "python3.exe", "py.exe" }
                : new[] { "python3", "python" };
            var pathNames = isWindows
                ? new[] { "py.exe", "python.exe", "python3.exe" }
                : new[] { "python3", "python" };
            var seen = new HashSet<string>(isWindows
                ? StringComparer.OrdinalIgnoreCase
                : StringComparer.Ordinal);

            foreach (var name in localNames)
            {
                var path = Path.Combine(AppContext.BaseDirectory, name);
                if (File.Exists(path) && seen.Add(Path.GetFullPath(path)))
                {
                    yield return CreateCandidate(path);
                }
            }

            var pathEnvironment = Environment.GetEnvironmentVariable("PATH") ?? string.Empty;
            var pathDirectories = pathEnvironment.Split(Path.PathSeparator);
            foreach (var name in pathNames)
            {
                foreach (var directoryValue in pathDirectories)
                {
                    var directory = directoryValue.Trim().Trim('"');
                    if (directory.Length == 0)
                    {
                        continue;
                    }
                    string path;
                    try
                    {
                        path = Path.GetFullPath(Path.Combine(directory, name));
                    }
                    catch (Exception)
                    {
                        continue;
                    }
                    if (File.Exists(path) && seen.Add(path))
                    {
                        yield return CreateCandidate(path);
                    }
                }
            }
        }

        private static PythonExecutable CreateCandidate(string path)
        {
            var isLauncher = OperatingSystem.IsWindows()
                && string.Equals(Path.GetFileName(path), "py.exe", StringComparison.OrdinalIgnoreCase);
            return new PythonExecutable(path, isLauncher ? "-3" : string.Empty);
        }

        private static bool CanRunPython3(PythonExecutable candidate)
        {
            try
            {
                using var process = new Process
                {
                    StartInfo = new ProcessStartInfo
                    {
                        FileName = candidate.FileName,
                        UseShellExecute = false,
                        CreateNoWindow = true,
                        RedirectStandardOutput = true,
                        RedirectStandardError = true
                    }
                };
                if (!string.IsNullOrEmpty(candidate.LauncherArguments))
                {
                    process.StartInfo.ArgumentList.Add(candidate.LauncherArguments);
                }
                process.StartInfo.ArgumentList.Add("-c");
                process.StartInfo.ArgumentList.Add(
                    "import sys; print('" + ProbeMarker +
                    "') if sys.version_info >= (3, 6) else sys.exit(1)");
                if (!process.Start())
                {
                    return false;
                }
                var standardOutput = process.StandardOutput.ReadToEndAsync();
                var standardError = process.StandardError.ReadToEndAsync();
                if (!process.WaitForExit(ProbeTimeoutMilliseconds))
                {
                    process.Kill(true);
                    process.WaitForExit();
                    return false;
                }
                standardError.GetAwaiter().GetResult();
                return process.ExitCode == 0
                    && standardOutput.GetAwaiter().GetResult()
                        .Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries)
                        .Contains(ProbeMarker, StringComparer.Ordinal);
            }
            catch
            {
                return false;
            }
        }
    }
}
