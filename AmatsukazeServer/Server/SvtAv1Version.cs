using System;
using System.Diagnostics;
using System.IO;
using System.Text.RegularExpressions;

namespace Amatsukaze.Server
{
    internal static class SvtAv1Version
    {
        internal const string VersionOutputPattern =
            @"(?m)^SVT-AV1\s+v(?<ver>(?<major>\d+)\.(?<minor>\d+)\.(?<patch>\d+)(?:-(?<revision>\d+))?)(?=$|[-\s])";

        private static readonly Regex FilenameRegex = new Regex(
            @"^SvtAv1EncApp_v?(?<major>\d+)\.(?<minor>\d+)\.(?<patch>\d+)(?:-(?<revision>\d+))?(?=$|[_.-])",
            RegexOptions.Compiled | RegexOptions.CultureInvariant | RegexOptions.IgnoreCase);

        private static readonly Regex VersionOutputRegex = new Regex(
            VersionOutputPattern,
            RegexOptions.Compiled | RegexOptions.CultureInvariant);

        internal static Version GetVersion(string path)
        {
            return GetVersion(path, GetVersionOutput);
        }

        internal static Version GetVersion(string path, Func<string, string> versionOutputProvider)
        {
            Version version;
            if (TryParseFilename(Path.GetFileName(path), out version))
            {
                return version;
            }

            string output;
            try
            {
                output = versionOutputProvider(path);
            }
            catch
            {
                return null;
            }
            return TryParseVersionOutput(output, out version) ? version : null;
        }

        internal static bool TryParseFilename(string filename, out Version version)
        {
            return TryParse(FilenameRegex.Match(filename ?? string.Empty), out version);
        }

        internal static bool TryParseVersionOutput(string output, out Version version)
        {
            return TryParse(VersionOutputRegex.Match(output ?? string.Empty), out version);
        }

        private static bool TryParse(Match match, out Version version)
        {
            version = null;
            int major;
            int minor;
            int patch;
            int revision = 0;
            if (!match.Success
                || !int.TryParse(match.Groups["major"].Value, out major)
                || !int.TryParse(match.Groups["minor"].Value, out minor)
                || !int.TryParse(match.Groups["patch"].Value, out patch)
                || (match.Groups["revision"].Success
                    && !int.TryParse(match.Groups["revision"].Value, out revision)))
            {
                return false;
            }

            version = new Version(major, minor, patch, revision);
            return true;
        }

        private static string GetVersionOutput(string path)
        {
            using (var process = new Process
            {
                StartInfo = new ProcessStartInfo
                {
                    FileName = path,
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                },
            })
            {
                process.StartInfo.ArgumentList.Add("--version");
                if (!process.Start())
                {
                    return null;
                }

                var stdoutTask = process.StandardOutput.ReadToEndAsync();
                var stderrTask = process.StandardError.ReadToEndAsync();
                if (!process.WaitForExit(5000))
                {
                    try
                    {
                        process.Kill(true);
                    }
                    catch
                    {
                    }
                    process.WaitForExit();
                    return null;
                }

                return stdoutTask.GetAwaiter().GetResult()
                    + Environment.NewLine
                    + stderrTask.GetAwaiter().GetResult();
            }
        }
    }
}
