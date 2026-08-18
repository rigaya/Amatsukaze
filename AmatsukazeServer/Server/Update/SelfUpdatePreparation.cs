using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;

namespace Amatsukaze.Server.Update
{
    internal enum SelfUpdatePathAction
    {
        Reject,
        OverwriteCopy,
        ReplaceDirectory,
        AddOnly,
        Ignore,
    }

    internal sealed record RestartCommandLine(string ProcessPath, string[] Arguments);

    internal sealed record PreparedSelfUpdate(string WorkingRoot, string StagingDirectory,
        string Version, RestartCommandLine RestartCommandLine);

    internal sealed record SelfUpdatePathRule(string Prefix, SelfUpdatePathAction Action,
        UpdateOSKind? OS = null);

    // 本体更新の許可リスト。P4b の配置処理もこの定義を参照する。
    internal static class SelfUpdatePolicy
    {
        // より限定的な規則を先に置く。
        private static readonly SelfUpdatePathRule[] PathRules =
        {
            new SelfUpdatePathRule("exe_files/wwwroot", SelfUpdatePathAction.ReplaceDirectory),
            new SelfUpdatePathRule("exe_files", SelfUpdatePathAction.OverwriteCopy),
            new SelfUpdatePathRule("JL/user", SelfUpdatePathAction.AddOnly),
            new SelfUpdatePathRule("JL", SelfUpdatePathAction.OverwriteCopy),
            new SelfUpdatePathRule("avs", SelfUpdatePathAction.AddOnly),
            new SelfUpdatePathRule("bat", SelfUpdatePathAction.AddOnly),
            new SelfUpdatePathRule("profile", SelfUpdatePathAction.AddOnly),
            new SelfUpdatePathRule("config", SelfUpdatePathAction.Ignore),
            new SelfUpdatePathRule("data", SelfUpdatePathAction.Ignore),
            new SelfUpdatePathRule("logo", SelfUpdatePathAction.Ignore),
            new SelfUpdatePathRule("drcs", SelfUpdatePathAction.Ignore),
            new SelfUpdatePathRule("avscache", SelfUpdatePathAction.Ignore),
            new SelfUpdatePathRule("scripts", SelfUpdatePathAction.OverwriteCopy,
                UpdateOSKind.Linux),
        };
        private static readonly HashSet<string> LinuxRootFiles = new HashSet<string>(
            new[] { "AmatsukazeServer.sh" }, StringComparer.Ordinal);
        private static readonly HashSet<string> WindowsRootFiles = new HashSet<string>(
            new[] { "Amatsukaze.bat", "AmatsukazeClient.bat", "AmatsukazeServer.bat",
                "AmatsukazeServerCLI.bat", "Readme.txt" }, StringComparer.OrdinalIgnoreCase);

        internal static IReadOnlyCollection<string> GetRootFiles(UpdateOSKind os) =>
            os == UpdateOSKind.Windows ? WindowsRootFiles : LinuxRootFiles;

        internal static void ValidateTopLevel(string stagingDirectory, UpdateOSKind os)
        {
            var staging = Path.GetFullPath(stagingDirectory);
            foreach (var entry in Directory.EnumerateFileSystemEntries(staging, "*",
                SearchOption.TopDirectoryOnly))
            {
                var name = Path.GetFileName(entry);
                var directory = Directory.Exists(entry);
                var allowedDirectory = PathRules.Any(rule => RuleApplies(rule, os) &&
                    string.Equals(rule.Prefix.Split('/')[0], name, StringComparison.Ordinal));
                var allowedFile = !directory && GetRootFiles(os).Contains(name);
                if (directory ? !allowedDirectory : !allowedFile)
                {
                    throw new UpdatePreparationException("UNEXPECTED_ARCHIVE_LAYOUT", "S09_STAGE",
                        "配布アーカイブに許可されていないトップレベル項目があります: " + name);
                }
            }
        }

        internal static SelfUpdatePathAction GetAction(string relativePath, UpdateOSKind os)
        {
            var path = NormalizeRelativePath(relativePath);
            if (path.Length == 0) return SelfUpdatePathAction.Reject;
            if (!path.Contains('/') && GetRootFiles(os).Contains(path))
                return SelfUpdatePathAction.OverwriteCopy;
            foreach (var rule in PathRules)
            {
                if (RuleApplies(rule, os) && IsUnder(path, rule.Prefix))
                    return rule.Action;
            }
            return SelfUpdatePathAction.Reject;
        }

        private static bool RuleApplies(SelfUpdatePathRule rule, UpdateOSKind os) =>
            !rule.OS.HasValue || rule.OS.Value == os;

        private static string NormalizeRelativePath(string path)
        {
            if (string.IsNullOrWhiteSpace(path) || Path.IsPathRooted(path)) return string.Empty;
            var normalized = path.Replace('\\', '/').TrimEnd('/');
            while (normalized.StartsWith("./", StringComparison.Ordinal))
            {
                normalized = normalized.Substring(2);
            }
            if (normalized.Split('/', StringSplitOptions.RemoveEmptyEntries)
                .Any(part => part == "..")) return string.Empty;
            return normalized;
        }

        private static bool IsUnder(string path, string root) =>
            string.Equals(path, root, StringComparison.Ordinal) ||
            path.StartsWith(root + "/", StringComparison.Ordinal);
    }

    internal sealed class SelfUpdatePreparation
    {
        // 配布アーカイブに対する展開後サイズの見積もり倍率。
        // 実測 (1.0.8.8): Linux 158MB→521MB (3.3倍) / Windows 180MB→787MB (4.4倍)。
        private const long ExpandedSizeRatio = 5;
        private const long MainExecutableMinimumBytes = 1024L * 1024;
        private const long RuntimeLibraryMinimumBytes = 64L * 1024;
        private readonly string appRoot;
        private readonly string proxy;
        private readonly string extractorPath;

        public SelfUpdatePreparation(string appRoot, string proxy, string extractorPath)
        {
            this.appRoot = Path.GetFullPath(appRoot);
            this.proxy = proxy;
            this.extractorPath = extractorPath;
        }

        public async Task<PreparedSelfUpdate> PrepareAsync(ReleaseAssetInfo asset,
            string expectedVersion, UpdateOSKind os, UpdateLog log,
            IProgress<DownloadProgress> progress, CancellationToken cancellationToken)
        {
            ArgumentNullException.ThrowIfNull(asset);
            ArgumentNullException.ThrowIfNull(log);
            using var workspace = SelfUpdateWorkspace.Create(appRoot, log.TransactionId);
            EnsurePreconditions(workspace, asset, log);
            using var downloader = new UpdateDownloader(proxy);
            var download = await downloader.DownloadAsync(asset, workspace.DownloadDirectory,
                "Amatsukaze", log, progress, cancellationToken).ConfigureAwait(false);
            var extraction = await new ArchiveExtractor(extractorPath).ExtractAsync(download,
                workspace.CandidateDirectory, "Amatsukaze", log, cancellationToken)
                .ConfigureAwait(false);
            try
            {
                SelfUpdatePolicy.ValidateTopLevel(extraction.DirectoryPath, os);
                ValidateProduct(extraction.DirectoryPath, os);
            }
            catch (UpdatePreparationException ex)
            {
                log.Write("Amatsukaze", "S09_STAGE", "NG", ("code", ex.Code),
                    ("path", extraction.DirectoryPath), ("message", ex.Message));
                throw;
            }
            var restart = CaptureRestartCommandLine();
            try
            {
                workspace.Commit();
            }
            catch (Exception ex)
            {
                log.Write("Amatsukaze", "S09_STAGE", "NG",
                    ("code", "STAGING_COMMIT_FAILED"),
                    ("path", workspace.StagingDirectory),
                    ("type", ex.GetType().Name), ("message", ex.Message));
                throw new UpdatePreparationException("STAGING_COMMIT_FAILED", "S09_STAGE",
                    "検証済みの本体更新を staging へ確定できませんでした", ex);
            }
            log.Write("Amatsukaze", "S09_STAGE", "OK",
                ("path", workspace.StagingDirectory), ("version", expectedVersion),
                ("entries", extraction.EntryCount), ("expanded", extraction.TotalBytes),
                ("process", restart.ProcessPath ?? "(unknown)"),
                ("arguments", string.Join(" ", restart.Arguments)));
            return new PreparedSelfUpdate(workspace.RootDirectory, workspace.StagingDirectory,
                expectedVersion, restart);
        }

        internal static void ValidateProduct(string stagingDirectory, UpdateOSKind os)
        {
            var executable = os == UpdateOSKind.Windows
                ? "AmatsukazeServerCLI.exe" : "AmatsukazeServerCLI";
            var library = os == UpdateOSKind.Windows ? "Amatsukaze.dll" : "libAmatsukaze.so";
            ValidateRequiredFile(stagingDirectory, Path.Combine("exe_files", executable),
                MainExecutableMinimumBytes);
            ValidateRequiredFile(stagingDirectory, Path.Combine("exe_files", library),
                RuntimeLibraryMinimumBytes);
        }

        internal static RestartCommandLine CaptureRestartCommandLine() =>
            BuildRestartCommandLine(Environment.ProcessPath, Environment.GetCommandLineArgs());

        internal static RestartCommandLine BuildRestartCommandLine(string processPath,
            string[] commandLineArguments)
        {
            var arguments = commandLineArguments ?? Array.Empty<string>();
            // argv[0] は引数ではなく実行アセンブリのパス。単一ファイル発行では実行ファイル自身、
            // 通常発行では同名の .dll になるため、どちらも再起動引数から外す。
            // dotnet 経由起動では ProcessPath が dotnet なので、dll の指定は残る。
            var skip = arguments.Length > 0 && !string.IsNullOrEmpty(processPath) &&
                (PathEquals(arguments[0], processPath) ||
                 PathEquals(arguments[0], Path.ChangeExtension(processPath, ".dll")));
            return new RestartCommandLine(processPath,
                arguments.Skip(skip ? 1 : 0).ToArray());
        }

        private static bool PathEquals(string left, string right)
        {
            if (string.IsNullOrWhiteSpace(left) || string.IsNullOrWhiteSpace(right)) return false;
            try
            {
                var comparison = OperatingSystem.IsWindows()
                    ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
                return string.Equals(Path.GetFullPath(left), Path.GetFullPath(right), comparison);
            }
            catch
            {
                return false;
            }
        }

        private static void ValidateRequiredFile(string stagingDirectory, string relativePath,
            long minimumBytes)
        {
            var path = SelfUpdateWorkspace.ValidateChild(stagingDirectory,
                Path.Combine(stagingDirectory, relativePath));
            if (!File.Exists(path))
            {
                throw new UpdatePreparationException("VERIFY_FAILED", "S09_STAGE",
                    "本体更新の検証対象ファイルがありません: " + relativePath);
            }
            var size = new FileInfo(path).Length;
            if (size < minimumBytes)
            {
                throw new UpdatePreparationException("VERIFY_FAILED", "S09_STAGE",
                    $"本体更新の検証対象ファイルが小さすぎます: {relativePath} ({size} bytes)");
            }
        }

        private static void EnsurePreconditions(SelfUpdateWorkspace workspace,
            ReleaseAssetInfo asset, UpdateLog log)
        {
            var writable = UpdateDiagnostics.CheckDirectoryWritable(workspace.RootDirectory, true);
            if (writable == false)
            {
                log.Write("Amatsukaze", "S01_PRECHECK", "NG",
                    ("code", "WRITE_ACCESS_DENIED"), ("path", workspace.RootDirectory));
                throw new UpdatePreparationException("WRITE_ACCESS_DENIED", "S01_PRECHECK",
                    "本体更新の作業領域へ書き込めません: " + workspace.RootDirectory);
            }
            long required;
            try
            {
                // アーカイブ、展開結果、P4b で作るバックアップの合計を確保する。
                required = checked(asset.Size * (1 + ExpandedSizeRatio * 2));
                UpdateDownloader.EnsureFreeSpace(workspace.RootDirectory, required,
                    "S01_PRECHECK");
            }
            catch (Exception ex) when (ex is UpdatePreparationException or OverflowException)
            {
                log.Write("Amatsukaze", "S01_PRECHECK", "NG", ("code", "NO_SPACE"),
                    ("asset_size", asset.Size), ("expanded_ratio", ExpandedSizeRatio));
                throw ex is UpdatePreparationException preparation ? preparation :
                    new UpdatePreparationException("NO_SPACE", "S01_PRECHECK",
                        "本体更新に必要な空き容量を計算できませんでした", ex);
            }
            log.Write("Amatsukaze", "S01_PRECHECK", "OK",
                ("work_root", workspace.RootDirectory), ("required", required),
                ("staging", workspace.StagingDirectory));
        }
    }
}
