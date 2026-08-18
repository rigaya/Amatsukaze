using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;

namespace Amatsukaze.Server.Update
{
    internal sealed record ArchiveSafetyLimits(int MaxEntries = 100000,
        long MaxExpandedBytes = 8L * 1024 * 1024 * 1024);

    internal sealed class ArchiveExtractor
    {
        private const long ExtractMarginBytes = 100L * 1024 * 1024;
        private static readonly Regex DrivePathRegex = new Regex("^[A-Za-z]:",
            RegexOptions.Compiled | RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));
        private static readonly Regex UnixModeRegex = new Regex(
            @"[dlbcps-][rwxsStT-]{9}$", RegexOptions.Compiled | RegexOptions.CultureInvariant,
            TimeSpan.FromSeconds(1));
        private static readonly IReadOnlyDictionary<string, ArchiveFormatHandler> Handlers =
            new Dictionary<string, ArchiveFormatHandler>(StringComparer.OrdinalIgnoreCase)
            {
                ["zip"] = new ArchiveFormatHandler(false),
                ["tar.xz"] = new ArchiveFormatHandler(true),
                ["deb"] = new ArchiveFormatHandler(true),
                ["7z"] = new ArchiveFormatHandler(false),
            };
        private readonly string extractorPath;
        private readonly ArchiveSafetyLimits limits;

        public ArchiveExtractor(string extractorPath, ArchiveSafetyLimits limits = null)
        {
            this.extractorPath = extractorPath;
            this.limits = limits ?? new ArchiveSafetyLimits();
        }

        public async Task<ExtractionResult> ExtractAsync(DownloadResult download,
            string destinationDirectory, string target, UpdateLog log,
            CancellationToken cancellationToken, PayloadEntry[] payload = null)
        {
            if (string.IsNullOrWhiteSpace(extractorPath) || !File.Exists(extractorPath))
            {
                log.Write(target, "S01_PRECHECK", "NG", ("code", "NO_EXTRACTOR"));
                throw new UpdatePreparationException("UNSUPPORTED_ENV", "S01_PRECHECK",
                    "7-Zip 展開器が見つかりません");
            }
            if (!Handlers.TryGetValue(download.Format, out var handler))
            {
                throw new UpdatePreparationException("UNSUPPORTED_FORMAT", "S08_EXTRACT",
                    "未対応のアーカイブ形式です");
            }

            var versionResult = await RunDirectAsync(Array.Empty<string>(), cancellationToken)
                .ConfigureAwait(false);
            var versionMatch = Regex.Match(versionResult.StandardOutput,
                @"7-Zip.*?([0-9]+\.[0-9]+)", RegexOptions.CultureInvariant);
            var extractorVersion = versionMatch.Success ? versionMatch.Groups[1].Value : "?";

            ProcessResult listing;
            try
            {
                listing = await handler.ListAsync(this, download.FilePath, cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                throw;
            }
            catch (Exception ex)
            {
                log.Write(target, "S08_EXTRACT", "NG", ("code", "EXTRACT_FAILED"),
                    ("phase", "list"), ("type", ex.GetType().Name), ("msg", ex.Message));
                throw new UpdatePreparationException("EXTRACT_FAILED", "S08_EXTRACT",
                    "アーカイブの事前検査を実行できませんでした", ex);
            }
            var listingExit = Math.Max(listing.FirstExitCode, listing.SecondExitCode ?? 0);
            if (listingExit >= 2)
            {
                log.Write(target, "S08_EXTRACT", "NG", ("code", "EXTRACT_FAILED"),
                    ("phase", "list"), ("exit", listingExit),
                    ("stderr", listing.StandardError));
                throw ExtractFailure("アーカイブの事前検査に失敗しました");
            }
            List<ArchiveEntry> entries;
            long totalBytes;
            try
            {
                entries = ParseListing(listing.StandardOutput);
                ValidateEntries(entries, download.Bytes);
                totalBytes = entries.Sum(entry => entry.Size);
                UpdateDownloader.EnsureFreeSpace(destinationDirectory,
                    checked(totalBytes + ExtractMarginBytes), "S08_EXTRACT");
            }
            catch (UpdatePreparationException ex)
            {
                log.Write(target, "S08_EXTRACT", "NG", ("code", ex.Code),
                    ("phase", "precheck"), ("msg", ex.Message));
                throw;
            }
            Directory.CreateDirectory(destinationDirectory);

            var stopwatch = Stopwatch.StartNew();
            ProcessResult extraction;
            try
            {
                extraction = await handler.ExtractAsync(this, download.FilePath,
                    destinationDirectory, cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                throw;
            }
            catch (Exception ex)
            {
                log.Write(target, "S08_EXTRACT", "NG", ("code", "EXTRACT_FAILED"),
                    ("phase", "extract"), ("type", ex.GetType().Name), ("msg", ex.Message));
                throw new UpdatePreparationException("EXTRACT_FAILED", "S08_EXTRACT",
                    "アーカイブの展開を実行できませんでした", ex);
            }
            stopwatch.Stop();
            var exitCode = Math.Max(listingExit,
                Math.Max(extraction.FirstExitCode, extraction.SecondExitCode ?? 0));
            if (exitCode >= 2)
            {
                log.Write(target, "S08_EXTRACT", "NG", ("code", "EXTRACT_FAILED"),
                    ("tool", extractorPath), ("format", download.Format),
                    ("exit", exitCode), ("stderr", extraction.StandardError));
                throw new UpdatePreparationException("EXTRACT_FAILED", "S08_EXTRACT",
                    "アーカイブを展開できませんでした");
            }
            ValidateExtractedTree(destinationDirectory);
            var payloadMatches = payload == null ? null :
                UpdatePayloadMatcher.FindMatches(destinationDirectory, payload);
            if (payload != null && payloadMatches.Count != 1)
            {
                log.Write(target, "S08_EXTRACT", "NG", ("code", "EXTRACT_FAILED"),
                    ("phase", "payload"),
                    ("expect", string.Join("|", payload.Select(entry => entry.Pattern))),
                    ("found", payloadMatches.Count));
                throw new UpdatePreparationException("EXTRACT_FAILED", "S08_EXTRACT",
                    "配置対象ファイルを展開結果から一意に特定できませんでした");
            }
            log.Write(target, "S08_EXTRACT", "OK", ("tool", extractorPath),
                ("ver", extractorVersion),
                ("format", download.Format), ("command", handler.CommandDescription),
                ("exit", exitCode), ("warning", exitCode == 1 ? "yes" : "no"),
                ("entries", entries.Count), ("expanded", totalBytes),
                ("expect", payload == null ? "<none>" :
                    string.Join("|", payload.Select(entry => entry.Pattern))),
                ("found", payloadMatches == null ? "unavailable" : payloadMatches.Count),
                ("elapsed", stopwatch.Elapsed.TotalSeconds.ToString("F3", CultureInfo.InvariantCulture) + "s"),
                ("stderr", listing.StandardError + extraction.StandardError));
            return new ExtractionResult(destinationDirectory, entries.Count, totalBytes, exitCode);
        }

        private static void ValidateExtractedTree(string root)
        {
            var pending = new Stack<string>();
            pending.Push(root);
            while (pending.Count > 0)
            {
                foreach (var entry in Directory.EnumerateFileSystemEntries(pending.Pop(), "*",
                    SearchOption.TopDirectoryOnly))
                {
                    var attributes = File.GetAttributes(entry);
                    if ((attributes & FileAttributes.ReparsePoint) != 0)
                    {
                        throw ExtractFailure("展開結果にリンクまたはリパースポイントがあります: " +
                            Path.GetRelativePath(root, entry));
                    }
                    if ((attributes & FileAttributes.Directory) != 0)
                    {
                        pending.Push(entry);
                    }
                }
            }
        }

        private void ValidateEntries(IReadOnlyList<ArchiveEntry> entries, long downloadBytes)
        {
            var expandedLimit = Math.Min(checked(downloadBytes * 50), limits.MaxExpandedBytes);
            if (entries.Count > limits.MaxEntries)
            {
                throw ExtractFailure("アーカイブのエントリ数が上限を超えています");
            }
            long total = 0;
            foreach (var entry in entries)
            {
                if (IsUnsafePath(entry.Path) || entry.IsLinkOrReparse)
                {
                    throw ExtractFailure("危険なアーカイブエントリを検出しました: " + entry.Path);
                }
                try
                {
                    total = checked(total + entry.Size);
                }
                catch (OverflowException)
                {
                    throw ExtractFailure("展開後サイズが数値上限を超えています");
                }
                if (total > expandedLimit)
                {
                    throw ExtractFailure("展開後サイズが上限を超えています");
                }
            }
        }

        private static bool IsUnsafePath(string path)
        {
            if (string.IsNullOrWhiteSpace(path))
            {
                return true;
            }
            var normalized = path.Replace('\\', '/');
            if (normalized.StartsWith("/", StringComparison.Ordinal) ||
                normalized.StartsWith("//", StringComparison.Ordinal) ||
                DrivePathRegex.IsMatch(normalized))
            {
                return true;
            }
            var segments = normalized.Split('/', StringSplitOptions.RemoveEmptyEntries);
            return segments.Any(segment => segment == ".." || segment.Contains(':'));
        }

        private static List<ArchiveEntry> ParseListing(string output)
        {
            var result = new List<ArchiveEntry>();
            var inEntries = false;
            var values = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            void Flush()
            {
                if (!values.TryGetValue("Path", out var path))
                {
                    values.Clear();
                    return;
                }
                _ = long.TryParse(values.GetValueOrDefault("Size"), NumberStyles.Integer,
                    CultureInfo.InvariantCulture, out var size);
                var attributes = values.GetValueOrDefault("Attributes") ?? string.Empty;
                var modeText = values.GetValueOrDefault("Mode") ?? attributes;
                var modeMatch = UnixModeRegex.Match(modeText);
                var specialUnixEntry = modeMatch.Success &&
                    "lbcps".Contains(modeMatch.Value[0]);
                var link = values.Any(item => !string.IsNullOrWhiteSpace(item.Value) &&
                    (item.Key.Contains("Symbolic", StringComparison.OrdinalIgnoreCase) ||
                     item.Key.Contains("Hard Link", StringComparison.OrdinalIgnoreCase) ||
                     item.Key.Contains("Reparse", StringComparison.OrdinalIgnoreCase))) ||
                    attributes.Contains('L') ||
                    attributes.Contains("Reparse", StringComparison.OrdinalIgnoreCase) ||
                    specialUnixEntry;
                result.Add(new ArchiveEntry(path, Math.Max(0, size), link));
                values.Clear();
            }
            using var reader = new StringReader(output ?? string.Empty);
            string line;
            while ((line = reader.ReadLine()) != null)
            {
                if (!inEntries)
                {
                    inEntries = line.StartsWith("----------", StringComparison.Ordinal);
                    continue;
                }
                if (line.Length == 0)
                {
                    Flush();
                    continue;
                }
                var separator = line.IndexOf(" = ", StringComparison.Ordinal);
                if (separator > 0)
                {
                    values[line.Substring(0, separator)] = line.Substring(separator + 3);
                }
            }
            Flush();
            if (result.Count == 0)
            {
                throw ExtractFailure("アーカイブの一覧を取得できませんでした");
            }
            return result;
        }

        private static UpdatePreparationException ExtractFailure(string message) =>
            new UpdatePreparationException("EXTRACT_FAILED", "S08_EXTRACT", message);

        private async Task<ProcessResult> RunDirectAsync(IEnumerable<string> arguments,
            CancellationToken cancellationToken)
        {
            using var process = StartProcess(arguments, redirectOutput: true);
            var stdout = process.StandardOutput.ReadToEndAsync(cancellationToken);
            var stderr = process.StandardError.ReadToEndAsync(cancellationToken);
            try
            {
                await process.WaitForExitAsync(cancellationToken).ConfigureAwait(false);
                return new ProcessResult(process.ExitCode, null, await stdout.ConfigureAwait(false),
                    await stderr.ConfigureAwait(false));
            }
            catch
            {
                KillTree(process);
                throw;
            }
        }

        private async Task<ProcessResult> RunPipeAsync(IEnumerable<string> firstArguments,
            IEnumerable<string> secondArguments, bool captureSecondOutput,
            CancellationToken cancellationToken)
        {
            using var first = StartProcess(firstArguments, redirectOutput: true);
            using var second = StartProcess(secondArguments, redirectOutput: captureSecondOutput,
                redirectInput: true);
            var firstError = first.StandardError.ReadToEndAsync(cancellationToken);
            var secondError = second.StandardError.ReadToEndAsync(cancellationToken);
            var secondOutput = captureSecondOutput
                ? second.StandardOutput.ReadToEndAsync(cancellationToken) : Task.FromResult(string.Empty);
            var transfer = TransferAndCloseAsync(first.StandardOutput.BaseStream,
                second.StandardInput.BaseStream, cancellationToken);
            try
            {
                await Task.WhenAll(transfer, first.WaitForExitAsync(cancellationToken),
                    second.WaitForExitAsync(cancellationToken)).ConfigureAwait(false);
                return new ProcessResult(first.ExitCode, second.ExitCode,
                    await secondOutput.ConfigureAwait(false),
                    (await firstError.ConfigureAwait(false)) + (await secondError.ConfigureAwait(false)));
            }
            catch
            {
                KillTree(first);
                KillTree(second);
                throw;
            }
        }

        private Process StartProcess(IEnumerable<string> arguments, bool redirectOutput,
            bool redirectInput = false)
        {
            var start = new ProcessStartInfo(extractorPath)
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardInput = redirectInput,
                RedirectStandardOutput = redirectOutput,
                RedirectStandardError = true,
            };
            foreach (var argument in arguments)
            {
                start.ArgumentList.Add(argument);
            }
            return Process.Start(start) ?? throw ExtractFailure("7-Zip を起動できませんでした");
        }

        private static async Task TransferAndCloseAsync(Stream source, Stream destination,
            CancellationToken cancellationToken)
        {
            try
            {
                await source.CopyToAsync(destination, cancellationToken).ConfigureAwait(false);
            }
            finally
            {
                await destination.DisposeAsync().ConfigureAwait(false);
            }
        }

        private static void KillTree(Process process)
        {
            try
            {
                if (!process.HasExited)
                {
                    process.Kill(entireProcessTree: true);
                }
            }
            catch
            {
                // キャンセル後のプロセス終了競合は無視する。
            }
        }

        private sealed record ArchiveEntry(string Path, long Size, bool IsLinkOrReparse);
        private sealed record ProcessResult(int FirstExitCode, int? SecondExitCode,
            string StandardOutput, string StandardError);

        private sealed class ArchiveFormatHandler
        {
            private readonly bool tarXz;

            public ArchiveFormatHandler(bool tarXz)
            {
                this.tarXz = tarXz;
            }

            public string CommandDescription => tarXz
                ? "7z x -so <archive> | 7z x -si -ttar -o<dir> -y"
                : "7z x <archive> -o<dir> -y";

            public Task<ProcessResult> ListAsync(ArchiveExtractor owner, string archive,
                CancellationToken cancellationToken) => tarXz
                ? owner.RunPipeAsync(new[] { "x", "-so", archive },
                    new[] { "l", "-si", "-ttar", "-slt" }, true, cancellationToken)
                : owner.RunDirectAsync(new[] { "l", "-slt", archive }, cancellationToken);

            public Task<ProcessResult> ExtractAsync(ArchiveExtractor owner, string archive,
                string destination, CancellationToken cancellationToken) => tarXz
                ? owner.RunPipeAsync(new[] { "x", "-so", archive },
                    new[] { "x", "-si", "-ttar", "-o" + destination, "-y" }, true,
                    cancellationToken)
                : owner.RunDirectAsync(new[] { "x", archive, "-o" + destination, "-y" },
                    cancellationToken);
        }
    }
}
