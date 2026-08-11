using System;
using System.Collections.Generic;
using System.IO;

namespace Amatsukaze.Server.Update
{
    internal sealed record DownloadProgress(long ReceivedBytes, long TotalBytes,
        double SpeedBytesPerSec);

    internal sealed record DownloadResult(string FilePath, long Bytes, string Sha256,
        TimeSpan Elapsed, string Format);

    internal sealed record ExtractionResult(string DirectoryPath, int EntryCount,
        long TotalBytes, int ExitCode);

    internal sealed record PreparedUpdate(string TargetId, string FilePath, string DestName,
        string Version, string SourceDirectory = null);

    internal sealed record PayloadMatch(string Path, PayloadEntry Payload);

    internal static class UpdatePayloadMatcher
    {
        public static IReadOnlyList<PayloadMatch> FindMatches(string root,
            IEnumerable<PayloadEntry> payload)
        {
            var matches = new List<PayloadMatch>();
            foreach (var path in Directory.EnumerateFiles(root, "*", SearchOption.AllDirectories))
            {
                var relative = Path.GetRelativePath(root, path).Replace('\\', '/');
                foreach (var entry in payload)
                {
                    if (entry.IsMatch(relative))
                    {
                        matches.Add(new PayloadMatch(path, entry));
                    }
                }
            }
            return matches;
        }
    }

    internal sealed class UpdatePreparationException : Exception
    {
        public UpdatePreparationException(string code, string stage, string message,
            Exception innerException = null) : base(message, innerException)
        {
            Code = code;
            Stage = stage;
        }

        public string Code { get; }
        public string Stage { get; }
    }

}
