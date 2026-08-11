using System.Security.Cryptography;
using Amatsukaze.Server.Update;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class LiveUpdatePreparationTests
{
    [EnvironmentFact("AMT_LIVE_NO_EXTRACTOR", "AMT_LIVE_APP_ROOT",
        "AMT_LIVE_ARTIFACT_DIR")]
    public async Task バンドル展開器不在を診断する()
    {
        var appRoot = Environment.GetEnvironmentVariable("AMT_LIVE_APP_ROOT")!;
        var artifactDir = Environment.GetEnvironmentVariable("AMT_LIVE_ARTIFACT_DIR")!;
        string logPath;
        using (var log = new UpdateLog(appRoot))
        {
            logPath = log.FilePath;
            var extractor = UpdateManager.FindExtractor(appRoot);
            Assert.Equal("(none)", extractor);
            var exception = await Assert.ThrowsAsync<UpdatePreparationException>(() =>
                new ArchiveExtractor(extractor).ExtractAsync(
                    new DownloadResult("unused.zip", 1, "-", TimeSpan.Zero, "zip"),
                    Path.Combine(appRoot, "exe_files", ".update_tmp", log.TransactionId,
                        "extract", "test"), "test", log, CancellationToken.None));
            Assert.Equal("UNSUPPORTED_ENV", exception.Code);
        }
        File.Copy(logPath, Path.Combine(artifactDir, "no_extractor_transaction.log"), true);
    }

    [EnvironmentFact("AMT_LIVE_APP_ROOT", "AMT_LIVE_ARTIFACT_DIR")]
    public async Task GitHub実アセット三対象を設置直前まで検証する()
    {
        var appRoot = Environment.GetEnvironmentVariable("AMT_LIVE_APP_ROOT")!;
        var artifactDir = Environment.GetEnvironmentVariable("AMT_LIVE_ARTIFACT_DIR")!;
        Directory.CreateDirectory(artifactDir);
        var before = Snapshot(Path.Combine(appRoot, "exe_files"));
        await File.WriteAllLinesAsync(Path.Combine(artifactDir, "exe_files_before.sha256"), before);

        Assert.True(UpdateCatalog.TryInitialize(out var catalogError), catalogError);
        string logPath;
        using (var log = new UpdateLog(appRoot))
        {
            logPath = log.FilePath;
            await UpdateDiagnostics.LogEnvironmentAsync(log, appRoot);
            using var transaction = UpdateTransaction.Create(appRoot, log.TransactionId);
            using var releases = new ReleaseClient(string.Empty);
            using var downloader = new UpdateDownloader(string.Empty);
            var environment = UpdateRuntimeEnvironment.Detect();
            foreach (var target in UpdateCatalog.Targets.Where(target =>
                target.Id is "x264" or "x265" or "SVT-AV1"))
            {
                var rule = Assert.Single(target.AssetRules, rule => rule.AppliesTo(environment));
                var candidates = await releases.GetReleasesAsync(target.Repository, target.Id,
                    target.ReleaseSelect, log, CancellationToken.None);
                Assert.NotNull(candidates);
                ReleaseInfo? selectedRelease = null;
                ReleaseAssetInfo? selectedAsset = null;
                string? expectedVersion = null;
                var scanned = 0;
                foreach (var release in candidates)
                {
                    scanned++;
                    foreach (var asset in release.Assets)
                    {
                        var match = rule.Match(asset.Name);
                        if (!match.Success) continue;
                        selectedRelease = release;
                        selectedAsset = asset;
                        expectedVersion = match.Groups["ver"].Value;
                        break;
                    }
                    if (selectedAsset != null) break;
                }
                Assert.NotNull(selectedRelease);
                Assert.NotNull(selectedAsset);
                Assert.False(string.IsNullOrWhiteSpace(expectedVersion));
                log.Write(target.Id, "S04_LATEST", "OK", ("tag", selectedRelease.TagName),
                    ("version", expectedVersion), ("scanned", scanned));
                log.Write(target.Id, "S05_SELECT_ASSET", "OK", ("asset", selectedAsset.Name),
                    ("size", selectedAsset.Size), ("digest", selectedAsset.Digest));

                var download = await downloader.DownloadAsync(selectedAsset,
                    transaction.GetTargetDownloadDirectory(target.Id), target.Id, log, null,
                    CancellationToken.None);
                var extraction = await new ArchiveExtractor(UpdateManager.FindExtractor(appRoot))
                    .ExtractAsync(download, transaction.GetTargetExtractDirectory(target.Id),
                        target.Id, log, CancellationToken.None, target.Payload);
                var prepared = await new UpdateStaging().PrepareAsync(target, extraction,
                    expectedVersion, log, CancellationToken.None);
                Assert.StartsWith(transaction.RootDir, prepared.FilePath, StringComparison.Ordinal);
            }
        }

        var after = Snapshot(Path.Combine(appRoot, "exe_files"));
        await File.WriteAllLinesAsync(Path.Combine(artifactDir, "exe_files_after.sha256"), after);
        Assert.Equal(before, after);
        await File.WriteAllTextAsync(Path.Combine(artifactDir, "exe_files_hash.diff"), string.Empty);
        File.Copy(logPath, Path.Combine(artifactDir, "live_update_transaction.log"), true);
    }

    private static string[] Snapshot(string exeFiles)
    {
        return Directory.EnumerateFiles(exeFiles, "*", SearchOption.AllDirectories)
            .Where(path => !Path.GetRelativePath(exeFiles, path).Replace('\\', '/')
                .StartsWith(".update_tmp/", StringComparison.Ordinal))
            .Select(path =>
            {
                using var stream = File.OpenRead(path);
                var hash = Convert.ToHexString(SHA256.HashData(stream)).ToLowerInvariant();
                return hash + "  " + Path.GetRelativePath(exeFiles, path).Replace('\\', '/');
            })
            .OrderBy(line => line, StringComparer.Ordinal)
            .ToArray();
    }
}

internal sealed class EnvironmentFactAttribute : FactAttribute
{
    public EnvironmentFactAttribute(params string[] variables)
    {
        var missing = variables.Where(variable =>
            string.IsNullOrWhiteSpace(Environment.GetEnvironmentVariable(variable))).ToArray();
        if (missing.Length > 0)
        {
            Skip = "実機テスト用の環境変数が未設定です: " + string.Join(",", missing);
        }
    }
}
