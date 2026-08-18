using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Net.Http;
using System.Security.Cryptography;
using System.Threading;
using System.Threading.Tasks;

namespace Amatsukaze.Server.Update
{
    internal sealed class UpdateDownloader : IDisposable
    {
        private const int MaxAttempts = 3;
        private static readonly byte[] ArArchiveMagic =
            [0x21, 0x3c, 0x61, 0x72, 0x63, 0x68];
        private static readonly byte[] SevenZipArchiveMagic =
            [0x37, 0x7a, 0xbc, 0xaf, 0x27, 0x1c];
        private static readonly TimeSpan InactivityTimeout = TimeSpan.FromSeconds(30);
        private static readonly TimeSpan ProgressInterval = TimeSpan.FromMilliseconds(250);
        private readonly HttpClient client;
        private readonly bool ownsClient;

        public UpdateDownloader(string proxy)
            : this(UpdateHttpClientFactory.Create(proxy), ownsClient: true)
        {
        }

        internal UpdateDownloader(HttpClient client, bool ownsClient = false)
        {
            this.client = client ?? throw new ArgumentNullException(nameof(client));
            this.ownsClient = ownsClient;
        }

        public async Task<DownloadResult> DownloadAsync(ReleaseAssetInfo asset,
            string destinationDirectory, string target, UpdateLog log,
            IProgress<DownloadProgress> progress, CancellationToken cancellationToken)
        {
            ArgumentNullException.ThrowIfNull(asset);
            Directory.CreateDirectory(destinationDirectory);
            try
            {
                EnsureFreeSpace(destinationDirectory, checked(asset.Size * 4), "S06_DOWNLOAD");
            }
            catch (Exception ex) when (ex is UpdatePreparationException or OverflowException)
            {
                log.Write(target, "S06_DOWNLOAD", "NG", ("code", "NO_SPACE"),
                    ("asset_size", asset.Size), ("required", "asset_size*4"));
                throw ex is UpdatePreparationException preparation ? preparation :
                    new UpdatePreparationException("NO_SPACE", "S06_DOWNLOAD",
                        "必要な空き容量を計算できませんでした", ex);
            }
            var destination = Path.Combine(destinationDirectory, Path.GetFileName(asset.Name));
            var partial = destination + ".part";
            Exception lastError = null;
            for (var attempt = 1; attempt <= MaxAttempts; attempt++)
            {
                DeleteIfExists(partial);
                DeleteIfExists(destination);
                try
                {
                    var result = await DownloadOnceAsync(asset, partial, target, log, progress,
                        attempt, cancellationToken).ConfigureAwait(false);
                    File.Move(partial, destination);
                    return result with { FilePath = destination };
                }
                catch (UpdatePreparationException)
                {
                    DeleteIfExists(partial);
                    throw;
                }
                catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
                {
                    DeleteIfExists(partial);
                    throw;
                }
                catch (Exception ex)
                {
                    lastError = ex;
                    DeleteIfExists(partial);
                    log.Write(target, "S06_DOWNLOAD", attempt < MaxAttempts ? "SKIP" : "NG",
                        ("code", attempt < MaxAttempts ? "DOWNLOAD_RETRY" : "DOWNLOAD_FAILED"),
                        ("url", asset.BrowserDownloadUrl),
                        ("attempt", attempt + "/" + MaxAttempts),
                        ("resume", "no"),
                        ("type", ex.GetType().Name),
                        ("msg", ex.Message));
                    if (attempt < MaxAttempts)
                    {
                        await Task.Delay(TimeSpan.FromSeconds(attempt), cancellationToken)
                            .ConfigureAwait(false);
                    }
                }
            }
            throw new UpdatePreparationException("DOWNLOAD_FAILED", "S06_DOWNLOAD",
                "更新ファイルをダウンロードできませんでした", lastError);
        }

        private async Task<DownloadResult> DownloadOnceAsync(ReleaseAssetInfo asset, string partial,
            string target, UpdateLog log, IProgress<DownloadProgress> progress, int attempt,
            CancellationToken cancellationToken)
        {
            var stopwatch = Stopwatch.StartNew();
            using var request = new HttpRequestMessage(HttpMethod.Get, asset.BrowserDownloadUrl);
            using var response = await client.SendAsync(request, HttpCompletionOption.ResponseHeadersRead,
                cancellationToken).ConfigureAwait(false);
            response.EnsureSuccessStatusCode();
            var finalUri = response.RequestMessage?.RequestUri;
            var total = response.Content.Headers.ContentLength ?? asset.Size;
            long received = 0;
            var lastProgress = TimeSpan.Zero;
            using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
            await using (var input = await response.Content.ReadAsStreamAsync(cancellationToken)
                .ConfigureAwait(false))
            await using (var output = new FileStream(partial, FileMode.Create, FileAccess.Write,
                FileShare.None, 128 * 1024, FileOptions.Asynchronous | FileOptions.SequentialScan))
            {
                var buffer = new byte[128 * 1024];
                while (true)
                {
                    using var idle = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
                    idle.CancelAfter(InactivityTimeout);
                    int count;
                    try
                    {
                        count = await input.ReadAsync(buffer.AsMemory(), idle.Token).ConfigureAwait(false);
                    }
                    catch (OperationCanceledException ex) when (!cancellationToken.IsCancellationRequested)
                    {
                        throw new IOException("30秒間データを受信できませんでした", ex);
                    }
                    if (count == 0)
                    {
                        break;
                    }
                    await output.WriteAsync(buffer.AsMemory(0, count), cancellationToken)
                        .ConfigureAwait(false);
                    hash.AppendData(buffer, 0, count);
                    received += count;
                    if (stopwatch.Elapsed - lastProgress >= ProgressInterval)
                    {
                        progress?.Report(new DownloadProgress(received, total,
                            received / Math.Max(stopwatch.Elapsed.TotalSeconds, 0.001)));
                        lastProgress = stopwatch.Elapsed;
                    }
                }
            }
            stopwatch.Stop();
            progress?.Report(new DownloadProgress(received, total,
                received / Math.Max(stopwatch.Elapsed.TotalSeconds, 0.001)));
            var sha256 = Convert.ToHexString(hash.GetHashAndReset()).ToLowerInvariant();
            log.Write(target, "S06_DOWNLOAD", "OK",
                ("url", asset.BrowserDownloadUrl),
                ("host", finalUri?.Host ?? "?"),
                ("length", total),
                ("received", received),
                ("elapsed", stopwatch.Elapsed.TotalSeconds.ToString("F3", CultureInfo.InvariantCulture) + "s"),
                ("speed", (long)(received / Math.Max(stopwatch.Elapsed.TotalSeconds, 0.001))),
                ("retry", attempt - 1),
                ("resume", "no"));

            var format = DetectFormat(asset.Name, partial);
            var expected = ParseExpectedDigest(asset.Digest);
            var sizeMatch = received == asset.Size;
            var digestMatch = expected == null ? "unavailable" :
                string.Equals(expected, sha256, StringComparison.OrdinalIgnoreCase) ? "yes" : "no";
            if (!sizeMatch || digestMatch == "no" || format == null)
            {
                log.Write(target, "S07_VERIFY", "NG",
                    ("code", "VERIFY_FAILED"), ("size", received), ("expected_size", asset.Size),
                    ("sha256", sha256), ("expected", expected ?? "<none>"),
                    ("match", digestMatch), ("format", format ?? "invalid"));
                throw new UpdatePreparationException("VERIFY_FAILED", "S07_VERIFY",
                    "更新ファイルの検証に失敗しました");
            }
            log.Write(target, "S07_VERIFY", "OK",
                ("size", received), ("expected_size", asset.Size), ("sha256", sha256),
                ("expected", expected ?? "<none>"), ("match", digestMatch), ("format", format));
            return new DownloadResult(partial, received, sha256, stopwatch.Elapsed, format);
        }

        private static string ParseExpectedDigest(string digest)
        {
            const string prefix = "sha256:";
            if (digest?.StartsWith(prefix, StringComparison.OrdinalIgnoreCase) == true &&
                digest.Length == prefix.Length + 64)
            {
                return digest.Substring(prefix.Length);
            }
            return null;
        }

        private static string DetectFormat(string name, string path)
        {
            Span<byte> magic = stackalloc byte[6];
            using var stream = File.OpenRead(path);
            if (stream.Read(magic) < magic.Length)
            {
                return null;
            }
            if (name.EndsWith(".zip", StringComparison.OrdinalIgnoreCase))
            {
                return magic[0] == 0x50 && magic[1] == 0x4b && magic[2] == 0x03 && magic[3] == 0x04
                    ? "zip" : null;
            }
            if (name.EndsWith(".tar.xz", StringComparison.OrdinalIgnoreCase))
            {
                return magic.SequenceEqual(new byte[] { 0xfd, 0x37, 0x7a, 0x58, 0x5a, 0x00 })
                    ? "tar.xz" : null;
            }
            if (name.EndsWith(".deb", StringComparison.OrdinalIgnoreCase))
            {
                return magic.SequenceEqual(ArArchiveMagic) ? "deb" : null;
            }
            if (name.EndsWith(".7z", StringComparison.OrdinalIgnoreCase))
            {
                return magic.SequenceEqual(SevenZipArchiveMagic) ? "7z" : null;
            }
            return null;
        }

        internal static void EnsureFreeSpace(string path, long requiredBytes, string stage)
        {
            var available = SelectDriveForPath(path).AvailableFreeSpace;
            if (available < requiredBytes)
            {
                throw new UpdatePreparationException("NO_SPACE", stage,
                    $"空き容量が不足しています (required={requiredBytes}, available={available})");
            }
        }

        internal static DriveInfo SelectDriveForPath(string path)
        {
            var fullPath = Path.GetFullPath(path);
            var comparison = OperatingSystem.IsWindows()
                ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
            DriveInfo selected = null;
            var selectedLength = -1;
            foreach (var drive in DriveInfo.GetDrives())
            {
                try
                {
                    if (!drive.IsReady) continue;
                    var root = Path.GetFullPath(drive.RootDirectory.FullName)
                        .TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
                    var rootPrefix = root.Length == 0 ? Path.DirectorySeparatorChar.ToString() :
                        root + Path.DirectorySeparatorChar;
                    if ((string.Equals(fullPath, root, comparison) ||
                         fullPath.StartsWith(rootPrefix, comparison)) &&
                        rootPrefix.Length > selectedLength)
                    {
                        selected = drive;
                        selectedLength = rootPrefix.Length;
                    }
                }
                catch
                {
                    // 読み取れないドライブは候補から除外する。
                }
            }
            return selected ?? new DriveInfo(Path.GetPathRoot(fullPath));
        }

        private static void DeleteIfExists(string path)
        {
            if (File.Exists(path))
            {
                File.Delete(path);
            }
        }

        public void Dispose()
        {
            if (ownsClient)
            {
                client.Dispose();
            }
        }
    }
}
