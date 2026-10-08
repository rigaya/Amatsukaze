using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Amatsukaze.Server.Rest;
using Amatsukaze.Shared;
using Xunit;

namespace AmatsukazeServerTest;

[Collection("REST一時保存先")]
public sealed class CmSegAnnotationTests
{
    [Fact]
    public void TS欠落時も正解入力だけを無効化して既存ファイルを保つ()
    {
        using var f = new AnnotationFixture();
        File.WriteAllText(f.AnnotationPath, "既存の正解ファイル");
        File.Delete(f.SourcePath);
        Assert.Throws<FileNotFoundException>(() => f.Open());
        var store = CmSegAnnotationStore.OpenForSession(f.SourcePath, f.Root, 300,
            new Dictionary<long, int> { [3003] = 10 }, f.Segments, out var error);
        Assert.Null(store);
        Assert.Contains("入力TSが見つかりません", error);
        Assert.Equal("既存の正解ファイル", File.ReadAllText(f.AnnotationPath));
        Assert.Equal(f.JlsText, File.ReadAllText(Path.Combine(f.Root, "jls0.txt")));
        Assert.Empty(Directory.GetFiles(f.Root, "*.tmp"));
        Assert.Empty(Directory.GetFiles(f.Root, "*.bak.json"));
    }

    [Fact]
    public void 保存先がディレクトリの場合は理由を返し検査ファイルを残さない()
    {
        using var f = new AnnotationFixture();
        Directory.CreateDirectory(f.AnnotationPath);
        Assert.Null(CmSegAnnotationStore.OpenForSession(f.SourcePath, f.Root, 300,
            new Dictionary<long, int> { [3003] = 10 }, f.Segments, out var error));
        Assert.Contains("保存先がディレクトリ", error);
        Assert.True(Directory.Exists(f.AnnotationPath));
        Assert.Empty(Directory.GetFiles(f.Root, "*.tmp"));
    }

    [Fact]
    public void Unixの保存先親ディレクトリが書込不可なら理由を返し元ファイルを保つ()
    {
        if (OperatingSystem.IsWindows()) return;
        using var f = new AnnotationFixture();
        File.WriteAllText(f.AnnotationPath, "既存の正解ファイル");
        var mode = File.GetUnixFileMode(f.Root);
        try
        {
            File.SetUnixFileMode(f.Root, UnixFileMode.UserRead | UnixFileMode.UserExecute);
            Assert.Null(CmSegAnnotationStore.OpenForSession(f.SourcePath, f.Root, 300,
                new Dictionary<long, int> { [3003] = 10 }, f.Segments, out var error));
            Assert.False(string.IsNullOrEmpty(error));
            Assert.Equal("既存の正解ファイル", File.ReadAllText(f.AnnotationPath));
            Assert.Equal(f.JlsText, File.ReadAllText(Path.Combine(f.Root, "jls0.txt")));
            Assert.Empty(Directory.GetFiles(f.Root, "*.tmp"));
            Assert.Empty(Directory.GetFiles(f.Root, "*.bak.json"));
        }
        finally { File.SetUnixFileMode(f.Root, mode); }
    }

    [Fact]
    public void FPS情報不正もセッション初期化へ例外を伝えず既存ファイルを保つ()
    {
        using var f = new AnnotationFixture();
        File.WriteAllText(f.AnnotationPath, "既存の正解ファイル");
        Assert.Null(CmSegAnnotationStore.OpenForSession(f.SourcePath, f.Root, 300,
            new Dictionary<long, int> { [0] = 10 }, f.Segments, out var error));
        Assert.Contains("フレームレートを取得できません", error);
        Assert.Equal("既存の正解ファイル", File.ReadAllText(f.AnnotationPath));
        Assert.Empty(Directory.GetFiles(f.Root, "*.tmp"));
        Assert.Empty(Directory.GetFiles(f.Root, "*.bak.json"));
    }

    [Fact]
    public void Windowsで退避対象がロック中でも初期化例外をセッションへ伝えない()
    {
        // Unixでは開いているファイルを移動できるため、この失敗条件はWindows固有。
        if (!OperatingSystem.IsWindows()) return;
        using var f = new AnnotationFixture();
        File.WriteAllText(f.AnnotationPath, "退避できない旧ファイル");
        using var held = new FileStream(f.AnnotationPath, FileMode.Open, FileAccess.Read, FileShare.Read);
        Assert.Null(CmSegAnnotationStore.OpenForSession(f.SourcePath, f.Root, 300,
            new Dictionary<long, int> { [3003] = 10 }, f.Segments, out var error));
        Assert.False(string.IsNullOrEmpty(error));
        Assert.Equal("退避できない旧ファイル", File.ReadAllText(f.AnnotationPath));
        Assert.Empty(Directory.GetFiles(f.Root, "*.tmp"));
        Assert.Empty(Directory.GetFiles(f.Root, "*.bak.json"));
    }

    [Fact]
    public void 無効時はTSもFPS情報もなくても理由を返さず何も作成しない()
    {
        using var f = new AnnotationFixture("0");
        File.Delete(f.SourcePath);
        File.WriteAllText(f.AnnotationPath, "既存の正解ファイル");
        var before = Directory.GetFileSystemEntries(f.Root).OrderBy(x => x).ToArray();
        Assert.Null(CmSegAnnotationStore.OpenForSession(f.SourcePath, f.Root, 300, null!, null!, out var error));
        Assert.Null(error);
        Assert.Equal(before, Directory.GetFileSystemEntries(f.Root).OrderBy(x => x).ToArray());
        Assert.Equal("既存の正解ファイル", File.ReadAllText(f.AnnotationPath));
        Assert.Equal(f.JlsText, File.ReadAllText(Path.Combine(f.Root, "jls0.txt")));
    }

    [Fact]
    public void 共通分類定義は全十クラスのキーとIDが一意で有効性を厳密に判定する()
    {
        Assert.Equal(new[] { "main", "sponsor", "sponsor_over_main", "next", "endcard", "self_promo", "other_promo", "cm", "station", "unknown" },
            CmSegLabels.Classes.Select(c => c.Id));
        Assert.Equal(new[] { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" }, CmSegLabels.Classes.Select(c => c.Key));
        Assert.Equal(10, CmSegLabels.Classes.Select(c => c.Id).Distinct().Count());
        Assert.Equal(10, CmSegLabels.Classes.Select(c => c.Key).Distinct().Count());
        Assert.All(CmSegLabels.Classes, c => Assert.True(CmSegLabels.IsValid(c.Id)));
        foreach (var invalid in new[] { null, "", "MAIN", "main ", "invalid" })
            Assert.False(CmSegLabels.IsValid(invalid!));
    }

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData("0")]
    [InlineData("true")]
    [InlineData(" 1")]
    public async Task 無効時はファイルに触れず不正本文でもAPIが404を返す(string? value)
    {
        using var f = new AnnotationFixture(value);
        File.WriteAllText(f.AnnotationPath, "変更してはいけない既存ファイル");
        Assert.False(CmSegAnnotationStore.IsEnabled);
        Assert.Null(f.Open());
        // 入力TSが存在しなくても、無効時はファイル情報を読みに行かない。
        Assert.Null(CmSegAnnotationStore.Open(f.SourcePath + ".missing", f.Root, 300, "30000/1001", f.Segments));
        await using var api = new RestEndpointTestFixture();
        var get = await api.Send("GET", "/api/trim/sessions/{sessionId}/cmseg", routeValues: new { sessionId = "missing" });
        var put = await api.Send("PUT", "/api/trim/sessions/{sessionId}/cmseg", body: "{不正なJSON", routeValues: new { sessionId = "missing" }, contentType: "text/plain");
        Assert.Equal(404, get.Status);
        Assert.Equal(404, put.Status);
        Assert.Equal("変更してはいけない既存ファイル", File.ReadAllText(f.AnnotationPath));
        Assert.Empty(Directory.GetFiles(f.Root, "*.bak.json"));
        var response = RestApiConfigurationTests.Json(new TrimAdjustSessionResponse());
        Assert.DoesNotContain("cmSegAnnotationEnabled", response);
        Assert.Contains("\"cmSegAnnotationEnabled\":true", RestApiConfigurationTests.Json(new TrimAdjustSessionResponse { CmSegAnnotationEnabled = true }));
    }

    [Theory]
    [InlineData("L", "main")]
    [InlineData("Mix", "main")]
    [InlineData("L-Edge(add)", "main")]
    [InlineData("N-Edge(add)", "main")]
    [InlineData("Sponsor(add)", "sponsor")]
    [InlineData("Sponsor(cut)", "sponsor")]
    [InlineData("Trailer(add)", "next")]
    [InlineData("Trailer", "next")]
    [InlineData("Trailer(cut-cancel)", "next")]
    [InlineData("Endcard(add)", "endcard")]
    [InlineData("Trailer(cut)", "other_promo")]
    [InlineData("CM", "cm")]
    [InlineData("Nologo", "cm")]
    [InlineData("Nologo(cut)", "cm")]
    [InlineData("N-Edge(cut)", "cm")]
    [InlineData("L-Edge(cut)", "cm")]
    [InlineData("未定義ラベル", "unknown")]
    public void ラベル対応は保持状態によらず設計表に従う(string label, string expected)
    {
        Assert.Equal(expected, CmSegLabels.InitialLabel(label, true));
        Assert.Equal(expected, CmSegLabels.InitialLabel(label, false));
    }

    [Theory]
    [InlineData("Border")]
    [InlineData("Border15s")]
    [InlineData("")]
    public void 境界と旧形式の初期分類はjls保持状態に従う(string label)
    {
        Assert.Equal("main", CmSegLabels.InitialLabel(label, true));
        Assert.Equal("cm", CmSegLabels.InitialLabel(label, false));
    }

    [Fact]
    public void 保存後はメタ情報と分類と確認状態を復元し取得結果は独立する()
    {
        using var f = new AnnotationFixture();
        var store = f.Open()!;
        Assert.True(CmSegAnnotationStore.IsEnabled);
        Assert.False(File.Exists(f.AnnotationPath));
        store.Save(new CmSegSaveRequest
        {
            Reviewed = true,
            // APIは区間番号で対応付けるため、送信順序に依存しない。
            Segments = new() { new() { Idx = 2, Label = "sponsor_over_main" }, new() { Idx = 1, Label = "station" } }
        });
        using var json = JsonDocument.Parse(File.ReadAllText(f.AnnotationPath));
        var root = json.RootElement;
        Assert.Equal(1, root.GetProperty("version").GetInt32());
        Assert.Equal(0, root.GetProperty("videoIndex").GetInt32());
        Assert.Equal(300, root.GetProperty("numFrames").GetInt32());
        Assert.Equal("30000/1001", root.GetProperty("fps").GetString());
        Assert.Equal(f.Root, root.GetProperty("tempDir").GetString());
        Assert.Equal(f.SourcePath, root.GetProperty("source").GetProperty("path").GetString());
        Assert.Equal(new FileInfo(f.SourcePath).Length, root.GetProperty("source").GetProperty("size").GetInt64());
        Assert.Equal(File.GetLastWriteTimeUtc(f.SourcePath), root.GetProperty("source").GetProperty("mtime").GetDateTime());
        Assert.Equal(Convert.ToHexString(SHA1.HashData(Encoding.UTF8.GetBytes(f.JlsText))).ToLowerInvariant(), root.GetProperty("jlsSha1").GetString());
        Assert.True(root.GetProperty("reviewed").GetBoolean());
        Assert.InRange(root.GetProperty("updatedAt").GetDateTime(), DateTime.UtcNow.AddMinutes(-1), DateTime.UtcNow.AddMinutes(1));
        Assert.False(root.TryGetProperty("warning", out _));
        Assert.Empty(Directory.GetFiles(f.Root, "*.tmp"));
        var restored = f.Open()!.Get();
        Assert.True(restored.Reviewed);
        Assert.Equal(new[] { "station", "sponsor_over_main" }, restored.Segments.Select(s => s.Label));
        Assert.All(restored.Segments, s => Assert.True(s.Edited));
        Assert.Equal(new[] { 0, 150 }, restored.Segments.Select(s => s.Start));
        Assert.Equal(new[] { 149, 299 }, restored.Segments.Select(s => s.End));
        Assert.Equal(new[] { "CM", "Sponsor(add)" }, restored.Segments.Select(s => s.JlsLabel));
        var snapshot = store.Get();
        snapshot.Source.Path = "変更";
        snapshot.Segments[0].Label = "unknown";
        Assert.Equal(f.SourcePath, store.Get().Source.Path);
        Assert.Equal("station", store.Get().Segments[0].Label);
        store.Save(f.Request());
        Assert.False(store.Get().Reviewed);
        Assert.All(store.Get().Segments, s => Assert.False(s.Edited));
        Assert.All(f.Open()!.Get().Segments, s => Assert.False(s.Edited));
    }

    [Theory]
    [InlineData("start")]
    [InlineData("end")]
    [InlineData("count")]
    [InlineData("json")]
    [InlineData("class")]
    [InlineData("version")]
    public void 不一致または不正な旧ファイルは内容を保って退避し初期化する(string change)
    {
        using var f = new AnnotationFixture();
        var original = f.Open()!;
        var request = f.Request();
        request.Reviewed = true;
        request.Segments[0].Label = "unknown";
        original.Save(request);
        if (change == "start") f.Segments[0].Start++;
        if (change == "end") f.Segments[0].End--;
        if (change == "count") f.Segments.RemoveAt(1);
        if (change == "json") File.WriteAllText(f.AnnotationPath, "{不正なJSON");
        if (change == "class") File.WriteAllText(f.AnnotationPath, File.ReadAllText(f.AnnotationPath).Replace("\"unknown\"", "\"invalid\""));
        if (change == "version") File.WriteAllText(f.AnnotationPath, File.ReadAllText(f.AnnotationPath).Replace("\"version\": 1", "\"version\": 2"));
        var oldContents = File.ReadAllText(f.AnnotationPath);
        var annotation = f.Open()!.Get();
        var backup = Assert.Single(Directory.GetFiles(f.Root, "record.ts.cmseg.*.bak.json"));
        Assert.Equal(oldContents, File.ReadAllText(backup));
        Assert.False(File.Exists(f.AnnotationPath));
        Assert.Contains(backup, annotation.Warning);
        Assert.False(annotation.Reviewed);
        Assert.Equal("cm", annotation.Segments[0].Label);
        Assert.All(annotation.Segments, s => Assert.False(s.Edited));
    }

    [Theory]
    [InlineData("class")]
    [InlineData("idx")]
    [InlineData("duplicate")]
    [InlineData("count")]
    [InlineData("null")]
    public void 不正な更新はファイルとメモリ内の分類を保つ(string change)
    {
        using var f = new AnnotationFixture();
        var store = f.Open()!;
        store.Save(f.Request());
        var previous = File.ReadAllText(f.AnnotationPath);
        var request = f.Request();
        request.Reviewed = true;
        if (change == "class") request.Segments[0].Label = "invalid";
        if (change == "idx") request.Segments[0].Idx = 99;
        if (change == "duplicate") request.Segments[1].Idx = 1;
        if (change == "count") request.Segments.RemoveAt(1);
        if (change == "null") request.Segments[0] = null!;
        Assert.Throws<ArgumentException>(() => store.Save(request));
        Assert.Equal(previous, File.ReadAllText(f.AnnotationPath));
        Assert.False(store.Get().Reviewed);
        Assert.Equal("cm", store.Get().Segments[0].Label);
        Assert.Empty(Directory.GetFiles(f.Root, "*.tmp"));
    }

    [Theory]
    [InlineData("main")]
    [InlineData("sponsor")]
    [InlineData("sponsor_over_main")]
    [InlineData("next")]
    [InlineData("endcard")]
    [InlineData("self_promo")]
    [InlineData("other_promo")]
    [InlineData("cm")]
    [InlineData("station")]
    [InlineData("unknown")]
    public void 定義された全クラスを保存して再開できる(string label)
    {
        using var f = new AnnotationFixture();
        var request = f.Request();
        request.Segments[0].Label = label;
        f.Open()!.Save(request);
        var segment = f.Open()!.Get().Segments[0];
        Assert.Equal(label, segment.Label);
        Assert.Equal(label != "cm", segment.Edited);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void 同じ録画の先行セッションが保存したら後続の古いセッションは上書きできない(bool savedBeforeOpen)
    {
        using var f = new AnnotationFixture();
        if (savedBeforeOpen) f.Open()!.Save(f.Request());
        var first = f.Open()!;
        var second = f.Open()!;
        var request = f.Request();
        request.Reviewed = true;
        request.Segments[0].Label = "main";
        first.Save(request);
        var savedContents = File.ReadAllText(f.AnnotationPath);
        var secondBefore = RestApiConfigurationTests.Json(second.Get());
        var conflicting = f.Request();
        conflicting.Segments[0].Label = "unknown";
        Assert.Throws<CmSegAnnotationConflictException>(() => second.Save(conflicting));
        Assert.Equal(savedContents, File.ReadAllText(f.AnnotationPath));
        Assert.Equal(secondBefore, RestApiConfigurationTests.Json(second.Get()));
        Assert.Equal("main", f.Open()!.Get().Segments[0].Label);
        Assert.Empty(Directory.GetFiles(f.Root, "*.tmp"));
        // 自身の保存後にはハッシュが更新され、同じセッションの次の保存が成功する。
        first.Save(f.Request());
        Assert.Equal("cm", f.Open()!.Get().Segments[0].Label);
    }

    [Fact]
    public void 再解析による区間変更で退避されたファイルを古いセッションは復活させない()
    {
        using var f = new AnnotationFixture();
        var old = f.Open()!;
        old.Save(f.Request());
        var oldContents = File.ReadAllText(f.AnnotationPath);
        var oldSnapshot = RestApiConfigurationTests.Json(old.Get());
        f.Segments[0].End--;
        var reopened = f.Open()!;
        Assert.NotNull(reopened.Get().Warning);
        var backup = Assert.Single(Directory.GetFiles(f.Root, "record.ts.cmseg.*.bak.json"));
        Assert.Throws<CmSegAnnotationConflictException>(() => old.Save(f.Request()));
        Assert.False(File.Exists(f.AnnotationPath));
        Assert.Equal(oldContents, File.ReadAllText(backup));
        Assert.Equal(oldSnapshot, RestApiConfigurationTests.Json(old.Get()));
        reopened.Save(f.Request());
        Assert.Equal(148, f.Open()!.Get().Segments[0].End);
    }

    [Fact]
    public void 同じ区間境界でもjls内容が変化したら保存を拒否しファイルとメモリを保つ()
    {
        using var f = new AnnotationFixture();
        var store = f.Open()!;
        store.Save(f.Request());
        var previousContents = File.ReadAllText(f.AnnotationPath);
        var previousSnapshot = RestApiConfigurationTests.Json(store.Get());
        File.WriteAllText(Path.Combine(f.Root, "jls0.txt"), f.JlsText.Replace("Sponsor(add)", "Trailer(add)"));
        var request = f.Request();
        request.Reviewed = true;
        request.Segments[0].Label = "station";
        Assert.Throws<CmSegAnnotationConflictException>(() => store.Save(request));
        Assert.Equal(previousContents, File.ReadAllText(f.AnnotationPath));
        Assert.Equal(previousSnapshot, RestApiConfigurationTests.Json(store.Get()));
        Assert.Empty(Directory.GetFiles(f.Root, "*.tmp"));
    }

    [Fact]
    public void 有効なセッションも環境変数を無効にすると読み書きを止める()
    {
        using var f = new AnnotationFixture();
        var store = f.Open()!;
        store.Save(f.Request());
        var previous = File.ReadAllText(f.AnnotationPath);
        Environment.SetEnvironmentVariable("AMT_CMSEG_ANNOTATION", "0");
        Assert.Null(store.Get());
        Assert.Throws<InvalidOperationException>(() => store.Save(f.Request()));
        Assert.Equal(previous, File.ReadAllText(f.AnnotationPath));
    }

    [Theory]
    [InlineData(3003, "30000/1001")]
    [InlineData(1501, "60000/1001")]
    [InlineData(1502, "60000/1001")]
    [InlineData(3754, "24000/1001")]
    [InlineData(3750, "24/1")]
    [InlineData(3600, "25/1")]
    [InlineData(3000, "30/1")]
    [InlineData(1800, "50/1")]
    [InlineData(1500, "60/1")]
    [InlineData(2000, "45/1")]
    public void フレームレートは最頻のPTS間隔から復元する(long ticks, string expected)
    {
        Assert.Equal(expected, CmSegAnnotationStore.ResolveFps(new Dictionary<long, int> { [ticks] = 100, [90000] = 1, [0] = 500 }));
    }

    [Fact]
    public void 有効なPTS間隔がなければフレームレート取得失敗を通知する()
    {
        Assert.Throws<InvalidDataException>(() => CmSegAnnotationStore.ResolveFps(new Dictionary<long, int> { [0] = 5, [-1] = 10 }));
    }

    private sealed class AnnotationFixture : IDisposable
    {
        private readonly string? previous = Environment.GetEnvironmentVariable("AMT_CMSEG_ANNOTATION");
        public string Root { get; } = Path.Combine(Path.GetTempPath(), "amt-cmseg-test-" + Guid.NewGuid().ToString("N"));
        public string SourcePath => Path.Combine(Root, "record.ts");
        public string AnnotationPath => SourcePath + ".cmseg.json";
        public string JlsText { get; } = "0 149 5 0 0 :CM\n150 299 5 0 5 :Sponsor(add)\n";
        public List<JlsSegment> Segments { get; } = new()
        {
            new() { Start = 0, End = 149, Label = "CM", JlsKeep = false },
            new() { Start = 150, End = 299, Label = "Sponsor(add)", JlsKeep = true }
        };

        public AnnotationFixture(string? environmentValue = "1")
        {
            Directory.CreateDirectory(Root);
            File.WriteAllText(SourcePath, "テスト用TS");
            File.WriteAllText(Path.Combine(Root, "jls0.txt"), JlsText);
            Environment.SetEnvironmentVariable("AMT_CMSEG_ANNOTATION", environmentValue);
        }

        public CmSegAnnotationStore? Open() => CmSegAnnotationStore.Open(SourcePath, Root, 300, "30000/1001", Segments);
        public CmSegSaveRequest Request() => new()
        {
            Segments = Segments.Select((s, i) => new CmSegLabelUpdate { Idx = i + 1, Label = CmSegLabels.InitialLabel(s.Label, s.JlsKeep) }).ToList()
        };

        public void Dispose()
        {
            Environment.SetEnvironmentVariable("AMT_CMSEG_ANNOTATION", previous);
            // この試験が作った専用一時ディレクトリだけを回収する。
            Directory.Delete(Root, true);
        }
    }
}
