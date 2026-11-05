using Amatsukaze.Server;
using System.Xml.Linq;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class NicoJKReferenceFinderTests
{
    [Fact]
    public async Task 実応答から同じ回の別局録画を見つける()
    {
        using var f = new Fixture();
        var reference = f.AddRecording("#ゾンビさがしてます_02_テレビ朝日.ts", 1064, Fixture.Earlier);
        Assert.Equal(reference, await f.Find());
        Assert.Equal(3, f.Requests.Count);
        Assert.Single(f.Reads);
        Assert.Contains("Range=20261011_003000-20261011_013000", f.Requests[0]);
    }

    [Fact]
    public async Task 最初の放送なら基準を選ばない()
    {
        using var f = new Fixture();
        f.Range = f.Episodes;
        Assert.Null((await f.Finder.FindAsync(f.Source, 1064, Fixture.Earlier, f.Logs.Add)).Reference);
        Assert.Empty(f.Reads);
        Assert.InRange(f.Requests.Count, 1, 3);
    }

    [Theory]
    [InlineData("#ゾンビさがしてます ０２.m2ts", true)]
    [InlineData("別作品_02.ts", false)]
    [InlineData("#ゾンビさがしてます_02.mp4", false)]
    public async Task 名前と拡張子で候補を絞る(string name, bool found)
    {
        using var f = new Fixture();
        var reference = f.AddRecording(name, 1064, Fixture.Earlier);
        Assert.Equal(found ? reference : null, await f.Find());
        Assert.Equal(found ? 1 : 0, f.Reads.Count);
    }

    [Fact]
    public void 名前は幅大小文字空白記号をそろえる()
    {
        Assert.Equal("fx戦士くるみちゃん02", NicoJKReferenceFinder.NormalizeName("ＦＸ 戦士・くるみちゃん＃０２！"));
    }

    [Theory]
    [InlineData(".nicojk.ini")]
    [InlineData(".nicojk.xml")]
    [InlineData(".nicojk.jkl")]
    [InlineData(".xml")]
    [InlineData(".jkl")]
    public void 手動指定を検出する(string extension)
    {
        using var f = new Fixture();
        Assert.False(NicoJKReferenceFinder.HasManualSource(f.Source));
        File.WriteAllText(Path.ChangeExtension(f.Source, extension), "");
        Assert.True(NicoJKReferenceFinder.HasManualSource(f.Source));
    }

    [Theory]
    [InlineData(1064, 300, true)]
    [InlineData(1064, 301, false)]
    [InlineData(1048, 0, false)]
    public async Task TSの局と開始時刻で確定する(int sid, int difference, bool found)
    {
        using var f = new Fixture();
        var reference = f.AddRecording("ゾンビさがしてます.ts", sid, Fixture.Earlier.AddSeconds(difference));
        Assert.Equal(found ? reference : null, await f.Find());
    }

    [Fact]
    public async Task 対象放送が十分近くなければ何も選ばない()
    {
        using var f = new Fixture();
        f.AddRecording("ゾンビさがしてます.ts", 1064, Fixture.Earlier);
        Assert.Null((await f.Finder.FindAsync(f.Source, 151, Fixture.Target.AddMinutes(11), f.Logs.Add)).Reference);
        Assert.Empty(f.Reads);
    }

    [Fact]
    public async Task 基準ではない系列局は候補にしない()
    {
        using var f = new Fixture();
        File.AppendAllText(Path.Combine(f.ToolDirectory, "syoboi_ch.txt"), "67\t3088\tメ～テレ\t0\n");
        File.AppendAllText(Path.Combine(f.ToolDirectory, "ch_sid.txt"), "5\t15\t3088\t0\tメ～テレ\n");
        f.Episodes = Fixture.Programs(Fixture.Program(8028, 67, "2", "2026-10-10 23:30:00"));
        f.AddRecording("ゾンビさがしてます.ts", 3088, Fixture.Earlier);
        Assert.Null(await f.Find());
        Assert.Empty(f.Reads);
    }

    [Fact]
    public async Task 削除された放送は基準にしない()
    {
        using var f = new Fixture();
        f.Episodes = ChangePrograms(f.Episodes, x => x.Element("Deleted")!.Value = "1");
        f.AddRecording("ゾンビさがしてます.ts", 1064, Fixture.Earlier);
        Assert.Null(await f.Find());
        Assert.Empty(f.Reads);
    }

    [Fact]
    public async Task サブフォルダと入力自身は読まない()
    {
        using var f = new Fixture();
        f.AddRecording("sub/ゾンビさがしてます.ts", 1064, Fixture.Earlier);
        Assert.Null(await f.Find());
        Assert.Empty(f.Reads);
    }

    [Fact]
    public async Task 短い作品名でも照合する()
    {
        using var f = new Fixture();
        var title = XDocument.Parse(f.Title);
        title.Descendants("ShortTitle").Single().Value = "ゾンます";
        f.Title = title.ToString();
        var reference = f.AddRecording("ゾンます_02.ts", 1064, Fixture.Earlier);
        Assert.Equal(reference, await f.Find());
    }

    [Fact]
    public async Task 非ゼロのStOffsetを二重加算しない()
    {
        using var f = new Fixture();
        // TID8019#3の実例: 23:45の放送が15分遅れ、StTimeは変更後の00:00、StOffsetは変更量900秒。
        f.Range = Fixture.Programs(Fixture.Program(8019, 6, "3", "2026-10-22 00:00:00", stOffset: 900));
        f.Episodes = Fixture.Programs(Fixture.Program(8019, 20, "3", "2026-10-21 22:00:00", stOffset: 900));
        f.Title = Fixture.TitleResponse(8019, "#ゾンビさがしてます");
        var earlier = new DateTime(2026, 10, 21, 22, 0, 0);
        var reference = f.AddRecording("ゾンビさがしてます.ts", 333, earlier);
        Assert.Equal(reference, (await f.Finder.FindAsync(f.Source, 1064,
            new DateTime(2026, 10, 22, 0, 0, 0), f.Logs.Add)).Reference);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task 通信や応答の失敗は警告して戻る(bool malformed)
    {
        using var f = new Fixture();
        f.FetchFailure = malformed ? null : new IOException("通信失敗");
        if (malformed) f.Range = "壊れたXML";
        Assert.Null(await f.Find());
        Assert.NotEmpty(f.Logs);
    }

    [Fact]
    public async Task TSを読めなくてもジョブを失敗させない()
    {
        using var f = new Fixture();
        f.AddRecording("ゾンビさがしてます.ts", 1064, Fixture.Earlier);
        f.ReadFailure = true;
        Assert.Null(await f.Find());
        Assert.NotEmpty(f.Logs);
    }

    [Fact]
    public async Task TS読み込み数は78までに収める()
    {
        using var f = new Fixture();
        for (var i = 0; i < 85; i++) f.AddRecording($"ゾンビさがしてます_{i:00}.ts", 1048, Fixture.Earlier);
        Assert.Null(await f.Find());
        Assert.Equal(78, f.Reads.Count);
        Assert.Equal(f.Reads.Count, f.Reads.Distinct().Count());
    }

    [Fact]
    public async Task 早い候補局の録画がなく百本あっても次の局を少数の読み込みで見つける()
    {
        using var f = new Fixture();
        f.Episodes = Fixture.Programs(
            Fixture.Program(8028, 20, "2", "2026-10-10 23:00:00"),
            Fixture.Program(8028, 6, "2", "2026-10-10 23:30:00"));
        for (int i = 0; i < 99; ++i)
        {
            var other = f.AddRecording($"ゾンビさがしてます_{i:00}.ts", 1048, Fixture.Earlier.AddDays(-1));
            File.SetLastWriteTime(other, Fixture.Earlier.AddSeconds(i));
        }
        var reference = f.AddRecording("z_ゾンビさがしてます.ts", 1064, Fixture.Earlier);
        File.SetLastWriteTime(reference, Fixture.Earlier.AddMinutes(30));
        Assert.Equal(reference, await f.Find());
        Assert.Equal(4, f.Reads.Count);
        Assert.Equal(reference, f.Reads.Last());
        Assert.Equal(f.Reads.Count, f.Reads.Distinct().Count());
    }

    [Fact]
    public async Task 応答とTS情報は次回探索で再利用する()
    {
        using var f = new Fixture();
        var reference = f.AddRecording("ゾンビさがしてます.ts", 1064, Fixture.Earlier);
        Assert.Equal(reference, await f.Find());
        Assert.Equal(reference, await f.Find());
        Assert.Equal(3, f.Requests.Count);
        Assert.Single(f.Reads);
        Assert.Equal(3, Directory.GetFiles(f.CacheDirectory).Length);
    }

    [Theory]
    [InlineData(1, "replace")]
    [InlineData(2, "merge")]
    public async Task 自動選択した録画をモード付きの引数にする(int mode, string argument)
    {
        using var f = new Fixture();
        var reference = f.AddRecording("ゾンビさがしてます.ts", 1064, Fixture.Earlier);
        var profile = new ProfileSetting { EnableNicoJK = true, NicoJKReferenceMode = mode };
        Assert.Equal(" --nicojk-ref \"" + reference + "\" --nicojk-ref-mode " + argument,
            (await f.Finder.GetArgumentsAsync(f.QueueItem(), profile, f.Logs.Add)).Arguments);
        Assert.Equal(3, f.Requests.Count);
    }

    [Theory]
    [InlineData(false, 1, 0, 7)]
    [InlineData(true, 0, 0, 7)]
    [InlineData(true, 3, 0, 7)]
    [InlineData(true, 1, 0, 6)]
    [InlineData(true, 1, 2, 7)]
    public async Task 自動選択の条件を満たさなければ通信しない(bool enabled, int mode, int space, int level1)
    {
        using var f = new Fixture();
        var item = f.QueueItem();
        item.Genre = new List<GenreItem> { new() { Space = space, Level1 = level1 } };
        var profile = new ProfileSetting { EnableNicoJK = enabled, NicoJKReferenceMode = mode };
        Assert.Equal(("", false), await f.Finder.GetArgumentsAsync(item, profile, f.Logs.Add));
        Assert.Empty(f.Requests);
        Assert.Empty(f.Reads);
    }

    [Theory]
    [InlineData(".nicojk.ini")]
    [InlineData(".nicojk.xml")]
    [InlineData(".nicojk.jkl")]
    [InlineData(".xml")]
    [InlineData(".jkl")]
    public async Task 手動指定を優先して自動選択の通信をしない(string extension)
    {
        using var f = new Fixture();
        File.WriteAllText(Path.ChangeExtension(f.Source, extension), "");
        var profile = new ProfileSetting { EnableNicoJK = true, NicoJKReferenceMode = 1 };
        Assert.Equal(("", false), await f.Finder.GetArgumentsAsync(f.QueueItem(), profile, f.Logs.Add));
        Assert.Empty(f.Requests);
        Assert.Empty(f.Reads);
    }

    [Fact]
    public async Task 自動選択の通信に失敗したら引数を追加しない()
    {
        using var f = new Fixture();
        f.FetchFailure = new IOException("通信失敗");
        var profile = new ProfileSetting { EnableNicoJK = true, NicoJKReferenceMode = 1 };
        Assert.Equal(("", false), await f.Finder.GetArgumentsAsync(f.QueueItem(), profile, f.Logs.Add));
        Assert.NotEmpty(f.Logs);
    }

    [Fact]
    public async Task 同時探索でも応答キャッシュを共有して通信を三回に収める()
    {
        using var f = new Fixture();
        f.YieldFetch = true;
        var reference = f.AddRecording("ゾンビさがしてます.ts", 1064, Fixture.Earlier);
        var results = await Task.WhenAll(f.Find(), f.Find());
        Assert.All(results, result => Assert.Equal(reference, result));
        Assert.Equal(3, f.Requests.Count);
        Assert.Equal(3, Directory.GetFiles(f.CacheDirectory).Length);
    }

    [Fact]
    public async Task 追加通信の開始間隔を十秒空けキャッシュ利用では待たない()
    {
        using var f = new Fixture();
        var reference = f.AddRecording("ゾンビさがしてます.ts", 1064, Fixture.Earlier);
        Assert.Equal(reference, await f.Find());
        Assert.Equal(3, f.RequestTimes.Count);
        Assert.Equal(TimeSpan.FromSeconds(10), f.RequestTimes[1] - f.RequestTimes[0]);
        Assert.Equal(TimeSpan.FromSeconds(10), f.RequestTimes[2] - f.RequestTimes[1]);
        Assert.Equal(new[] { TimeSpan.FromSeconds(10), TimeSpan.FromSeconds(10) }, f.Delays);
        Assert.Equal(reference, await f.Find());
        Assert.Equal(3, f.RequestTimes.Count);
        Assert.Equal(2, f.Delays.Count);
        Assert.Contains(f.Logs, line => line.Contains("待"));
    }

    [Fact]
    public async Task 通信で経過した時間を待機時間から引く()
    {
        using var f = new Fixture();
        f.FetchDuration = TimeSpan.FromSeconds(4);
        f.AddRecording("ゾンビさがしてます.ts", 1064, Fixture.Earlier);
        Assert.NotNull(await f.Find());
        Assert.Equal(new[] { TimeSpan.FromSeconds(6), TimeSpan.FromSeconds(6) }, f.Delays);
    }

    [Fact]
    public async Task 通信に失敗しても同じ探索では再試行しない()
    {
        using var f = new Fixture();
        f.FetchFailure = new IOException("通信失敗");
        Assert.Null(await f.Find());
        Assert.Single(f.RequestTimes);
        Assert.Empty(f.Delays);
    }

    [Fact]
    public async Task Season表記と第何期表記が違っても作品名で候補にする()
    {
        using var f = new Fixture();
        f.SetTitle("ゾンビさがしてます Season2");
        var reference = f.AddRecording("ゾンビさがしてます 第2期_02.ts", 1064, Fixture.Earlier);
        Assert.Equal(reference, await f.Find());
    }

    [Fact]
    public async Task 長い副題を省いたファイル名でも先頭五文字で候補にする()
    {
        using var f = new Fixture();
        f.SetTitle("転生したら剣でした ～異世界の旅を仲間と歩む物語～");
        var reference = f.AddRecording("転生したら_02.ts", 1064, Fixture.Earlier);
        Assert.Equal(reference, await f.Find());
    }

    [Fact]
    public async Task 同じ作品が五十本あっても終了時刻に近い録画を最初に読む()
    {
        using var f = new Fixture();
        f.SetTitle("作品名 長い副題");
        var end = Fixture.Earlier.AddMinutes(30);
        for (int i = 0; i < 49; ++i)
        {
            var other = f.AddRecording($"{i:00}_作品名_長い副題.ts", 1064, Fixture.Earlier.AddDays(-i - 1));
            File.SetLastWriteTime(other, end.AddDays(-i - 1));
        }
        var reference = f.AddRecording("z_作品名.ts", 1064, Fixture.Earlier);
        File.SetLastWriteTime(reference, end);
        Assert.Equal(reference, await f.Find());
        Assert.Equal(reference, Assert.Single(f.Reads));
    }

    [Fact]
    public async Task 更新時刻がすべて同じでも上限内なら見つかる()
    {
        using var f = new Fixture();
        var first = f.AddRecording("a_ゾンビさがしてます.ts", 1064, Fixture.Earlier.AddDays(-1));
        var reference = f.AddRecording("m_ゾンビさがしてます.ts", 1064, Fixture.Earlier);
        var last = f.AddRecording("z_ゾンビさがしてます.ts", 1064, Fixture.Earlier.AddDays(1));
        foreach (var path in new[] { first, reference, last }) File.SetLastWriteTime(path, Fixture.Earlier);
        Assert.Equal(reference, await f.Find());
        Assert.Equal(new[] { first, reference }, f.Reads);
    }

    [Theory]
    [InlineData("2026 １２３ Ａ", "2026_123_A.ts")]
    [InlineData("作品名 2026 Ａ", "2026_A.ts")]
    public async Task 数字のみの区間と一文字の区間からは候補を作らない(string title, string filename)
    {
        using var f = new Fixture();
        f.SetTitle(title);
        f.AddRecording(filename, 1064, Fixture.Earlier);
        Assert.Null(await f.Find());
        Assert.Empty(f.Reads);
    }

    [Theory]
    [InlineData(-300)]
    [InlineData(0)]
    [InlineData(300)]
    public async Task 系列局とキー局が五分以内なら今までどおりにする(int difference)
    {
        using var f = new Fixture();
        f.SetAffiliate();
        f.Episodes = Fixture.Programs(Fixture.Program(8028, 6, "2",
            Fixture.Target.AddSeconds(difference).ToString("yyyy-MM-dd HH:mm:ss", System.Globalization.CultureInfo.InvariantCulture)));
        var result = await f.AffiliateArguments();
        Assert.Equal(("", false), result);
        Assert.Contains(f.Logs, line => line.Contains("同時"));
    }

    [Theory]
    [InlineData(1)]
    [InlineData(2)]
    public async Task 遅れネットの録画があれば統合設定でも置き換える(int mode)
    {
        using var f = new Fixture();
        f.SetAffiliate();
        var reference = f.AddRecording("ゾンビさがしてます.ts", 1064, Fixture.Earlier);
        Assert.Equal((" --nicojk-ref \"" + reference + "\" --nicojk-ref-mode replace", false),
            await f.AffiliateArguments(mode));
        Assert.Contains(f.Logs, line => line.Contains("置き換") || line.Contains("replace"));
    }

    [Theory]
    [InlineData(301)]
    [InlineData(5400)]
    public async Task キー局と五分を超えて違い録画もなければ実況を付けない(int difference)
    {
        using var f = new Fixture();
        f.SetAffiliate();
        f.Episodes = Fixture.Programs(Fixture.Program(8028, 6, "2",
            Fixture.Target.AddSeconds(-difference).ToString("yyyy-MM-dd HH:mm:ss", System.Globalization.CultureInfo.InvariantCulture)));
        Assert.Equal(("", true), await f.AffiliateArguments());
        Assert.Contains(f.Logs, line => line.Contains("付けません"));
    }

    [Fact]
    public async Task キー局の同じ回の放送がなければ実況を付けない()
    {
        using var f = new Fixture();
        f.SetAffiliate();
        f.Episodes = Fixture.Programs(Fixture.Program(8028, 67, "2", "2026-10-11 01:00:00"));
        Assert.Equal(("", true), await f.AffiliateArguments());
        Assert.Empty(f.Reads);
    }

    [Fact]
    public async Task 系列局の録画した放送が特定できなければ今までどおりにする()
    {
        using var f = new Fixture();
        f.SetAffiliate();
        f.Range = Fixture.Programs();
        Assert.Equal(("", false), await f.AffiliateArguments());
        Assert.Single(f.Requests);
    }

    [Fact]
    public async Task 系列局の話数がなければ今までどおりにする()
    {
        using var f = new Fixture();
        f.SetAffiliate();
        f.Range = ChangePrograms(f.Range, x => x.Element("Count")!.Value = "");
        Assert.Equal(("", false), await f.AffiliateArguments());
        Assert.Single(f.Requests);
    }

    [Theory]
    [InlineData(3088)]
    [InlineData(4000)]
    public async Task 同じChIDの複数ServiceIdをどちらも対象として判定する(int serviceId)
    {
        using var f = new Fixture();
        f.SetAffiliate();
        File.AppendAllText(Path.Combine(f.ToolDirectory, "syoboi_ch.txt"), "67\t4000\tメ～テレ支局\t0\n");
        File.AppendAllText(Path.Combine(f.ToolDirectory, "ch_sid.txt"), "5\t15\t4000\t0\tメ～テレ支局\n");
        Assert.Equal(("", true), await f.AffiliateArguments(serviceId: serviceId));
        Assert.Contains("ChID=67", f.Requests[0]);
    }

    private static string ChangePrograms(string xml, Action<XElement> change)
    {
        var doc = XDocument.Parse(xml);
        foreach (var item in doc.Descendants("ProgItem")) change(item);
        return doc.ToString();
    }

    private sealed class Fixture : IDisposable
    {
        public static readonly DateTime Target = new(2026, 10, 11, 1, 0, 0);
        public static readonly DateTime Earlier = new(2026, 10, 10, 23, 30, 0);
        public string Root { get; } = Path.Combine(Path.GetTempPath(), "AmatsukazeNicoJKTests", Guid.NewGuid().ToString("N"));
        public string ToolDirectory => Path.Combine(Root, "tools");
        public string CacheDirectory => Path.Combine(Root, "cache");
        public string Source => Path.Combine(Root, "#ゾンビさがしてます_02_BS朝日.ts");
        // しょぼいカレンダーの応答は、照合に使う項目だけを実際の応答(2026-11-05取得)から写した。
        // #ゾンビさがしてます#02: BS朝日(ChID 18)は10/11 01:00、テレ朝(ChID 6)などは10/10 23:30。
        public string Range = Programs(Program(8028, 18, "2", "2026-10-11 01:00:00"));
        public string Episodes = Programs(
            Program(8028, 67, "2", "2026-10-10 23:30:00"),
            Program(8028, 81, "2", "2026-10-10 23:30:00"),
            Program(8028, 18, "2", "2026-10-11 01:00:00"),
            Program(8028, 6, "2", "2026-10-10 23:30:00"),
            Program(8028, 20, "2", "2026-10-12 21:00:00"),
            Program(8028, 88, "2", "2026-10-10 23:30:00"),
            Program(8028, 276, "2", "2026-10-11 00:00:00"));
        public string Title = TitleResponse(8028, "#ゾンビさがしてます");
        public List<string> Requests { get; } = new();
        public List<string> Reads { get; } = new();
        public List<DateTime> RequestTimes { get; } = new();
        public List<TimeSpan> Delays { get; } = new();
        public DateTime Now = DateTime.UtcNow;
        public TimeSpan FetchDuration;
        public List<string> Logs { get; } = new();
        public Exception? FetchFailure;
        public bool ReadFailure;
        public bool YieldFetch;
        private readonly Dictionary<string, (int[] ServiceIds, DateTime Start)> recordings = new();
        public NicoJKReferenceFinder Finder { get; }

        public Fixture()
        {
            Directory.CreateDirectory(ToolDirectory);
            File.WriteAllText(Source, "対象");
            File.WriteAllText(Path.Combine(ToolDirectory, "syoboi_ch.txt"),
                "6\t1064\tテレビ朝日\t1\n18\t151\tBS朝日\t1\n20\t333\tAT-X\t1\n");
            File.WriteAllText(Path.Combine(ToolDirectory, "ch_sid.txt"),
                "5\t15\t1064\t0\tテレビ朝日\n151\t4\t151\t0\tBS朝日\n333\t7\t333\t0\tAT-X\n");
            Finder = new NicoJKReferenceFinder(ToolDirectory, CacheDirectory, async url =>
            {
                Requests.Add(url);
                RequestTimes.Add(Now);
                Now += FetchDuration;
                if (FetchFailure != null) throw FetchFailure;
                if (YieldFetch) await Task.Yield();
                return url.Contains("Command=TitleLookup") ? Title : url.Contains("Range=") ? Range : Episodes;
            }, path =>
            {
                Reads.Add(path);
                if (ReadFailure) throw new IOException("TS読み込み失敗");
                return recordings[path];
            }, () => Now, duration =>
            {
                Delays.Add(duration);
                Now += duration;
                return Task.CompletedTask;
            });
        }

        public void SetAffiliate()
        {
            File.AppendAllText(Path.Combine(ToolDirectory, "syoboi_ch.txt"), "67\t3088\tメ～テレ\t0\n");
            File.AppendAllText(Path.Combine(ToolDirectory, "ch_sid.txt"), "5\t15\t3088\t0\tメ～テレ\n");
            Range = Programs(Program(8028, 67, "2", "2026-10-11 01:00:00"));
        }

        public Task<(string Arguments, bool DisableNicoJK)> AffiliateArguments(int mode = 2, int serviceId = 3088)
        {
            var item = QueueItem();
            item.ServiceId = serviceId;
            return Finder.GetArgumentsAsync(item,
                new ProfileSetting { EnableNicoJK = true, NicoJKReferenceMode = mode }, Logs.Add);
        }

        public void SetTitle(string title, string shortTitle = "")
        {
            var document = XDocument.Parse(Title);
            document.Descendants("Title").Single().Value = title;
            document.Descendants("ShortTitle").Single().Value = shortTitle;
            Title = document.ToString();
        }

        public string AddRecording(string name, int sid, DateTime start)
        {
            var path = Path.Combine(Root, name);
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path, "録画");
            recordings[path] = (new[] { sid }, start);
            return path;
        }

        public QueueItem QueueItem() => new()
        {
            SrcPath = Source,
            ServiceId = 151,
            EITStartTime = Target,
            Genre = new List<GenreItem> { new() { Space = (int)GenreSpace.ARIB, Level1 = 7 } }
        };

        public async Task<string> Find() => (await Finder.FindAsync(Source, 151, Target, Logs.Add)).Reference;
        public static XElement Program(int tid, int chid, string count, string stTime, int stOffset = 0) =>
            new("ProgItem", new XElement("TID", tid), new XElement("ChID", chid), new XElement("Count", count),
                new XElement("StTime", stTime),
                new XElement("EdTime", DateTime.ParseExact(stTime, "yyyy-MM-dd HH:mm:ss", System.Globalization.CultureInfo.InvariantCulture)
                    .AddMinutes(30).ToString("yyyy-MM-dd HH:mm:ss", System.Globalization.CultureInfo.InvariantCulture)),
                new XElement("StOffset", stOffset), new XElement("Deleted", 0));

        public static string Programs(params XElement[] items) =>
            new XElement("ProgLookupResponse", new XElement("ProgItems", items),
                new XElement("Result", new XElement("Code", 200))).ToString();

        public static string TitleResponse(int tid, string title, string shortTitle = "") =>
            new XElement("TitleLookupResponse",
                new XElement("TitleItems", new XElement("TitleItem", new XElement("TID", tid),
                    new XElement("Title", title), new XElement("ShortTitle", shortTitle))),
                new XElement("Result", new XElement("Code", 200))).ToString();
        public void Dispose() => Directory.Delete(Root, true);
    }
}
