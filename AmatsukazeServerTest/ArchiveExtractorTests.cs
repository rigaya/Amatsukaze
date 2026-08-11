using System.Diagnostics;
using System.IO.Compression;
using System.Security.Cryptography;
using Amatsukaze.Server.Update;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class ArchiveExtractorTests
{
    [Theory]
    [InlineData("zip")]
    [InlineData("tar.xz")]
    public async Task 正常なアーカイブを展開してPayloadを一件に確定する(string format)
    {
        using var fixture = new ArchiveFixture();
        var archive = format == "zip" ? fixture.CreateZip(("tool", Script))
            : fixture.CreateTarXz(("tool", Script));
        using var log = new UpdateLog(fixture.Root);
        var download = fixture.DownloadResult(archive, format);

        var extraction = await fixture.Extractor.ExtractAsync(download, fixture.ExtractDirectory,
            "test", log, CancellationToken.None);
        var prepared = await new UpdateStaging().PrepareAsync(CreateTarget("^tool$"), extraction,
            "1.2.3", log, CancellationToken.None);

        Assert.Equal("1.2.3", prepared.Version);
        Assert.True(File.Exists(prepared.FilePath));
    }

    [Theory]
    [InlineData("../outside")]
    [InlineData("/absolute")]
    [InlineData("//server/share")]
    [InlineData("C:/drive")]
    [InlineData("file:stream")]
    public async Task 危険なパスは展開前に拒否する(string entryName)
    {
        using var fixture = new ArchiveFixture();
        var archive = fixture.CreateZip((entryName, Script));
        using var log = new UpdateLog(fixture.Root);

        var exception = await Assert.ThrowsAsync<UpdatePreparationException>(() =>
            fixture.Extractor.ExtractAsync(fixture.DownloadResult(archive, "zip"),
                fixture.ExtractDirectory, "test", log, CancellationToken.None));

        Assert.Equal("EXTRACT_FAILED", exception.Code);
        Assert.False(File.Exists(Path.Combine(fixture.Root, "outside")));
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task シンボリックリンクとハードリンクを拒否する(bool hardLink)
    {
        if (OperatingSystem.IsWindows()) return;
        using var fixture = new ArchiveFixture();
        var archive = fixture.CreateLinkedTarXz(hardLink);
        using var log = new UpdateLog(fixture.Root);

        var exception = await Assert.ThrowsAsync<UpdatePreparationException>(() =>
            fixture.Extractor.ExtractAsync(fixture.DownloadResult(archive, "tar.xz"),
                fixture.ExtractDirectory, "test", log, CancellationToken.None));
        Assert.Equal("EXTRACT_FAILED", exception.Code);
    }

    [Fact]
    public async Task Zip内のUnixシンボリックリンクを拒否する()
    {
        using var fixture = new ArchiveFixture();
        var archive = fixture.CreateLinkedZip();
        using var log = new UpdateLog(fixture.Root);

        var exception = await Assert.ThrowsAsync<UpdatePreparationException>(() =>
            fixture.Extractor.ExtractAsync(fixture.DownloadResult(archive, "zip"),
                fixture.ExtractDirectory, "test", log, CancellationToken.None));
        Assert.Equal("EXTRACT_FAILED", exception.Code);
    }

    [Fact]
    public async Task 一覧で見えない展開後リンクも拒否する()
    {
        if (OperatingSystem.IsWindows()) return;
        using var fixture = new ArchiveFixture();
        var archive = fixture.CreateZip(("tool", Script));
        using var log = new UpdateLog(fixture.Root);

        var exception = await Assert.ThrowsAsync<UpdatePreparationException>(() =>
            new ArchiveExtractor(fixture.CreateExtractorThatProducesLink()).ExtractAsync(
                fixture.DownloadResult(archive, "zip"), fixture.ExtractDirectory, "test", log,
                CancellationToken.None));
        Assert.Equal("EXTRACT_FAILED", exception.Code);
    }

    [Fact]
    public async Task エントリ数上限を超えたアーカイブを拒否する()
    {
        using var fixture = new ArchiveFixture(new ArchiveSafetyLimits(MaxEntries: 1));
        var archive = fixture.CreateZip(("one", Script), ("two", Script));
        using var log = new UpdateLog(fixture.Root);

        var exception = await Assert.ThrowsAsync<UpdatePreparationException>(() =>
            fixture.Extractor.ExtractAsync(fixture.DownloadResult(archive, "zip"),
                fixture.ExtractDirectory, "test", log, CancellationToken.None));
        Assert.Equal("EXTRACT_FAILED", exception.Code);
    }

    [Fact]
    public async Task 展開後サイズ上限を超えたアーカイブを拒否する()
    {
        using var fixture = new ArchiveFixture(new ArchiveSafetyLimits(MaxExpandedBytes: 1));
        var archive = fixture.CreateZip(("tool", Script));
        using var log = new UpdateLog(fixture.Root);

        var exception = await Assert.ThrowsAsync<UpdatePreparationException>(() =>
            fixture.Extractor.ExtractAsync(fixture.DownloadResult(archive, "zip"),
                fixture.ExtractDirectory, "test", log, CancellationToken.None));
        Assert.Equal("EXTRACT_FAILED", exception.Code);
    }

    [Theory]
    [InlineData(0)]
    [InlineData(2)]
    public async Task Payloadが一件でなければ拒否する(int count)
    {
        using var fixture = new ArchiveFixture();
        Directory.CreateDirectory(fixture.ExtractDirectory);
        for (var index = 0; index < count; index++)
        {
            var directory = index == 0 ? fixture.ExtractDirectory
                : Directory.CreateDirectory(Path.Combine(fixture.ExtractDirectory, "sub")).FullName;
            await File.WriteAllTextAsync(Path.Combine(directory, "tool"), Script);
        }
        using var log = new UpdateLog(fixture.Root);
        var extraction = new ExtractionResult(fixture.ExtractDirectory, count, count * Script.Length, 0);

        var exception = await Assert.ThrowsAsync<UpdatePreparationException>(() =>
            new UpdateStaging().PrepareAsync(CreateTarget("(^|/)tool$"), extraction,
                "1.2.3", log, CancellationToken.None));
        Assert.Equal("EXTRACT_FAILED", exception.Code);
    }

    [Fact]
    public async Task バージョン出力が一致すれば非ゼロ終了も受け入れる()
    {
        if (OperatingSystem.IsWindows()) return;
        using var fixture = new ArchiveFixture();
        Directory.CreateDirectory(fixture.ExtractDirectory);
        await File.WriteAllTextAsync(Path.Combine(fixture.ExtractDirectory, "tool"),
            "#!/bin/sh\necho version 1.2.3\nexit 7\n");
        using var log = new UpdateLog(fixture.Root);

        var prepared = await new UpdateStaging().PrepareAsync(CreateTarget("^tool$"),
            new ExtractionResult(fixture.ExtractDirectory, 1, Script.Length, 0),
            "1.2.3", log, CancellationToken.None);
        Assert.Equal("1.2.3", prepared.Version);
    }

    [Fact]
    public void 未コンパイルのPayloadはプログラミングエラーにする()
    {
        var payload = new PayloadEntry { Pattern = "^tool$" };
        Assert.Throws<InvalidOperationException>(() => payload.IsMatch("tool"));
    }

    [Fact]
    public async Task 展開器が無ければ非対応環境として拒否する()
    {
        using var fixture = new ArchiveFixture();
        var archive = fixture.CreateZip(("tool", Script));
        using var log = new UpdateLog(fixture.Root);

        var exception = await Assert.ThrowsAsync<UpdatePreparationException>(() =>
            new ArchiveExtractor(Path.Combine(fixture.Root, "missing-7zzs")).ExtractAsync(
                fixture.DownloadResult(archive, "zip"), fixture.ExtractDirectory, "test", log,
                CancellationToken.None));
        Assert.Equal("UNSUPPORTED_ENV", exception.Code);
    }

    [Fact]
    public async Task T09_破損アーカイブは終了コードと標準エラーを記録する()
    {
        using var fixture = new ArchiveFixture();
        var archive = fixture.CreateZip(("tool", Script));
        using (var stream = new FileStream(archive, FileMode.Open, FileAccess.Write))
        {
            stream.SetLength(Math.Max(1, stream.Length / 2));
        }
        using var log = new UpdateLog(fixture.Root);

        var exception = await Assert.ThrowsAsync<UpdatePreparationException>(() =>
            fixture.Extractor.ExtractAsync(fixture.DownloadResult(archive, "zip"),
                fixture.ExtractDirectory, "test", log, CancellationToken.None));
        log.Dispose();
        var text = await File.ReadAllTextAsync(log.FilePath);

        Assert.Equal("EXTRACT_FAILED", exception.Code);
        Assert.Contains("[S08_EXTRACT] NG code=EXTRACT_FAILED", text);
        Assert.Contains("exit=", text);
        Assert.Contains("stderr=", text);
    }

    [Fact]
    public async Task T11_展開後にPayloadが消えたらウイルス対策の案内を記録する()
    {
        if (OperatingSystem.IsWindows()) return;
        using var fixture = new ArchiveFixture();
        Directory.CreateDirectory(fixture.ExtractDirectory);
        var payload = Path.Combine(fixture.ExtractDirectory, "tool");
        await File.WriteAllTextAsync(payload, Script);
        using var log = new UpdateLog(fixture.Root);
        var preparation = new UpdateStaging().PrepareAsync(CreateTarget("^tool$"),
            new ExtractionResult(fixture.ExtractDirectory, 1, Script.Length, 0),
            "1.2.3", log, CancellationToken.None);
        await Task.Delay(100);
        File.Delete(payload);

        var exception = await Assert.ThrowsAsync<UpdatePreparationException>(() => preparation);
        log.Dispose();
        var text = await File.ReadAllTextAsync(log.FilePath);

        Assert.Equal("ANTIVIRUS_SUSPECTED", exception.Code);
        Assert.Contains("[S09_STAGE] NG code=ANTIVIRUS_SUSPECTED", text);
        Assert.Contains("exe_files", exception.Message);
        Assert.Contains("除外設定", exception.Message);
    }

    [Fact]
    public async Task T12_実行できないPayloadは既存ファイルを変えず検証失敗にする()
    {
        if (OperatingSystem.IsWindows()) return;
        using var fixture = new ArchiveFixture();
        Directory.CreateDirectory(fixture.ExtractDirectory);
        await File.WriteAllTextAsync(Path.Combine(fixture.ExtractDirectory, "tool"),
            "#!/bin/sh\nexit 1\n");
        using var log = new UpdateLog(fixture.Root);

        var exception = await Assert.ThrowsAsync<UpdatePreparationException>(() =>
            new UpdateStaging().PrepareAsync(CreateTarget("^tool$"),
                new ExtractionResult(fixture.ExtractDirectory, 1, 17, 0),
                "1.2.3", log, CancellationToken.None));
        log.Dispose();
        var text = await File.ReadAllTextAsync(log.FilePath);

        Assert.Equal("VERIFY_FAILED", exception.Code);
        Assert.Contains("[S09_STAGE] NG code=VERIFY_FAILED", text);
    }

    [Fact]
    public async Task 一トランザクションの二対象をまとめて破棄する()
    {
        using var fixture = new ArchiveFixture();
        string firstPath;
        string secondPath;
        string transactionRoot;
        using (var transaction = UpdateTransaction.Create(fixture.Root, "1234abcd"))
        {
            transactionRoot = transaction.RootDir;
            firstPath = await PrepareDirectAsync(transaction, "first", fixture.Root);
            secondPath = await PrepareDirectAsync(transaction, "second", fixture.Root);
            Assert.StartsWith(transactionRoot, firstPath, StringComparison.Ordinal);
            Assert.StartsWith(transactionRoot, secondPath, StringComparison.Ordinal);
            Assert.True(File.Exists(firstPath));
            Assert.True(File.Exists(secondPath));
        }
        Assert.False(Directory.Exists(transactionRoot));
        Assert.False(File.Exists(firstPath));
        Assert.False(File.Exists(secondPath));
    }

    private static async Task<string> PrepareDirectAsync(UpdateTransaction transaction,
        string id, string appRoot)
    {
        var directory = transaction.GetTargetExtractDirectory(id);
        var path = Path.Combine(directory, "tool");
        await File.WriteAllTextAsync(path, Script);
        using var log = new UpdateLog(appRoot);
        var prepared = await new UpdateStaging().PrepareAsync(CreateTarget("^tool$", id),
            new ExtractionResult(directory, 1, Script.Length, 0), "1.2.3", log,
            CancellationToken.None);
        return prepared.FilePath;
    }

    private const string Script = "#!/bin/sh\necho version 1.2.3\n";

    private static UpdateTargetDef CreateTarget(string payloadPattern, string id = "test")
    {
        var target = new UpdateTargetDef
        {
            Id = id,
            DisplayName = id,
            Repository = "test/test",
            AssetRules = Array.Empty<AssetRule>(),
            VersionArgument = "--version",
            VersionPattern = @"version (?<ver>\d+\.\d+\.\d+)",
            Payload = [new PayloadEntry { Pattern = payloadPattern }],
        };
        Assert.True(target.TryCompileRegexes(out var error), error);
        return target;
    }

    private sealed class ArchiveFixture : IDisposable
    {
        private readonly UpdateTransaction transaction;
        private readonly string downloadDirectory;
        private readonly string[] before;

        public ArchiveFixture(ArchiveSafetyLimits? limits = null)
        {
            Root = Path.Combine(Path.GetTempPath(), "amatsukaze-archive-test-" + Guid.NewGuid());
            var exeFiles = Path.Combine(Root, "exe_files");
            Directory.CreateDirectory(Path.Combine(exeFiles, "existing"));
            File.WriteAllText(Path.Combine(exeFiles, "existing", "encoder"), "変更禁止");
            before = Snapshot(exeFiles);
            transaction = UpdateTransaction.Create(Root, "3344ccdd");
            downloadDirectory = transaction.GetTargetDownloadDirectory("case");
            ExtractDirectory = transaction.GetTargetExtractDirectory("case");
            Extractor = new ArchiveExtractor(FindExtractor(), limits);
        }

        public string Root { get; }
        public string ExtractDirectory { get; }
        public ArchiveExtractor Extractor { get; }

        public string CreateZip(params (string Name, string Content)[] entries)
        {
            var path = Path.Combine(downloadDirectory, Guid.NewGuid() + ".zip");
            using var archive = ZipFile.Open(path, ZipArchiveMode.Create);
            foreach (var item in entries)
            {
                using var writer = new StreamWriter(archive.CreateEntry(item.Name).Open());
                writer.Write(item.Content);
            }
            return path;
        }

        public string CreateTarXz(params (string Name, string Content)[] entries)
        {
            var source = Directory.CreateDirectory(Path.Combine(downloadDirectory,
                "source-" + Guid.NewGuid())).FullName;
            foreach (var item in entries)
            {
                var path = Path.Combine(source, item.Name);
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                File.WriteAllText(path, item.Content);
            }
            var tar = Path.Combine(downloadDirectory, Guid.NewGuid() + ".tar");
            Run("tar", ["-cf", tar, "-C", source, "."]);
            Run("xz", [tar]);
            return tar + ".xz";
        }

        public string CreateLinkedTarXz(bool hardLink)
        {
            var source = Directory.CreateDirectory(Path.Combine(downloadDirectory,
                "links-" + Guid.NewGuid())).FullName;
            var target = Path.Combine(source, "target");
            File.WriteAllText(target, Script);
            var link = Path.Combine(source, "link");
            Run("ln", hardLink ? [target, link] : ["-s", "target", link]);
            var tar = Path.Combine(downloadDirectory, Guid.NewGuid() + ".tar");
            Run("tar", ["-cf", tar, "-C", source, "."]);
            Run("xz", [tar]);
            return tar + ".xz";
        }

        public string CreateLinkedZip()
        {
            var path = Path.Combine(downloadDirectory, Guid.NewGuid() + ".zip");
            using var archive = ZipFile.Open(path, ZipArchiveMode.Create);
            var entry = archive.CreateEntry("evil_sym");
            entry.ExternalAttributes = unchecked((int)0xA1FF0000);
            using var writer = new StreamWriter(entry.Open());
            writer.Write("/etc/passwd");
            return path;
        }

        public string CreateExtractorThatProducesLink()
        {
            if (OperatingSystem.IsWindows())
            {
                throw new PlatformNotSupportedException("このテストは Linux 用です");
            }
            var path = Path.Combine(Root, "fake-link-7z.sh");
            File.WriteAllText(path, "#!/bin/sh\nif [ $# -eq 0 ]; then echo '7-Zip 26.02'; exit 0; fi\n" +
                "if [ \"$1\" = \"l\" ]; then printf '%s\\n' '----------' 'Path = tool' 'Size = 1' ''; exit 0; fi\n" +
                "for arg in \"$@\"; do case \"$arg\" in -o*) out=${arg#-o};; esac; done\n" +
                "mkdir -p \"$out\"\nln -s /etc/passwd \"$out/tool\"\n");
            File.SetUnixFileMode(path, File.GetUnixFileMode(path) | UnixFileMode.UserExecute);
            return path;
        }

        public DownloadResult DownloadResult(string archive, string format)
        {
            var info = new FileInfo(archive);
            return new DownloadResult(archive, info.Length, Hash(archive), TimeSpan.Zero, format);
        }

        private static string FindExtractor()
        {
            var path = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory,
                "../../../../../build/exe_files/7z/7zzs"));
            if (!File.Exists(path)) throw new InvalidOperationException("テスト用7zzsがありません: " + path);
            return path;
        }

        private static void Run(string fileName, IEnumerable<string> arguments)
        {
            var start = new ProcessStartInfo(fileName) { UseShellExecute = false };
            foreach (var argument in arguments) start.ArgumentList.Add(argument);
            using var process = Process.Start(start)!;
            process.WaitForExit();
            Assert.Equal(0, process.ExitCode);
        }

        private static string Hash(string path) =>
            Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path))).ToLowerInvariant();

        public void Dispose()
        {
            transaction.Dispose();
            Assert.Equal(before, Snapshot(Path.Combine(Root, "exe_files")));
            Directory.Delete(Root, true);
        }

        private static string[] Snapshot(string root) => Directory.EnumerateFiles(root, "*",
                SearchOption.AllDirectories)
            .Where(path => !Path.GetRelativePath(root, path).Replace('\\', '/')
                .StartsWith(".update_tmp/", StringComparison.Ordinal))
            .Select(path => Path.GetRelativePath(root, path) + ":" + Hash(path))
            .OrderBy(value => value, StringComparer.Ordinal).ToArray();
    }
}
