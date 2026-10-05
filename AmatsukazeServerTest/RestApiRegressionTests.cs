using System.Globalization;
using System.Text;
using System.Text.Json;
using Amatsukaze.Server;
using Xunit;

namespace AmatsukazeServerTest;

[Collection("REST一時保存先")]
public sealed class RestApiRegressionTests
{
    [Theory]
    [InlineData("既存")]
    [InlineData("EXISTING")]
    public async Task 自動選択の重複リネームは元設定と一覧を保つ(string target)
    {
        await using var f = new RestFileSystemFixture();
        foreach (var name in new[] { "元", "既存", "existing" })
            await f.Api.Send("POST", "/api/autoselect", body: RestApiConfigurationTests.Json(new AutoSelectProfile { Name = name, Conditions = new() }));
        var before = f.AutoSelects.ToDictionary(p => p.Key, p => p.Value);
        var dto = (await f.Api.Send("GET", "/api/autoselect")).Body;
        var response = await f.Api.Send("PUT", "/api/autoselect/{name}", body: "{\"name\":\"元\",\"conditions\":[]}", routeValues: new { name = target });
        var id = RestApiConfigurationTests.RequestId(response);
        Assert.True(f.Api.Queue.Store.TryGetMessageForRequestId(id, out var message));
        Assert.Equal("error", message.Level);
        Assert.Equal(3, f.AutoSelects.Count);
        foreach (var p in before)
        {
            Assert.Same(p.Value, f.AutoSelects[p.Key]);
            Assert.Equal(p.Key, p.Value.Name);
        }
        Assert.Equal(dto, (await f.Api.Send("GET", "/api/autoselect")).Body);
    }

    [Theory]
    [InlineData("same")]
    [InlineData("SAME")]
    public async Task 自動選択の同名と大文字小文字だけのリネームは設定を失わない(string target)
    {
        await using var f = new RestFileSystemFixture();
        var original = new AutoSelectProfile { Name = "same", Conditions = new() };
        await f.Api.Send("POST", "/api/autoselect", body: RestApiConfigurationTests.Json(original));
        var before = f.AutoSelects[original.Name];
        var dto = (await f.Api.Send("GET", "/api/autoselect")).Body;
        if (target == original.Name)
        {
            // RESTでは同名は通常更新になるので、明示的なNewName指定も直接確認する。
            await f.Api.Queue.Server.SetAutoSelect(new AutoSelectUpdate { Type = UpdateType.Update, Profile = original, NewName = target });
            Assert.Equal(dto, (await f.Api.Send("GET", "/api/autoselect")).Body);
        }
        else RestApiConfigurationTests.RequestId(await f.Api.Send("PUT", "/api/autoselect/{name}", body: RestApiConfigurationTests.Json(original), routeValues: new { name = target }));
        Assert.Same(before, Assert.Single(f.AutoSelects).Value);
        Assert.Equal(target, before.Name);
        Assert.Equal(target, Assert.Single(f.Api.Queue.Store.GetAutoSelects()).Name);
    }

    [Theory]
    [InlineData("/api/logs/encode.csv", "カンマ,あり")]
    [InlineData("/api/logs/encode.csv", "引用\"符")]
    [InlineData("/api/logs/encode.csv", "複数\r\n行")]
    [InlineData("/api/logs/encode.csv", "複数\n行")]
    [InlineData("/api/logs/encode.csv", "複数\r行")]
    [InlineData("/api/logs/check.csv", "カンマ,あり")]
    [InlineData("/api/logs/check.csv", "引用\"符")]
    [InlineData("/api/logs/check.csv", "複数\r\n行")]
    [InlineData("/api/logs/check.csv", "複数\n行")]
    [InlineData("/api/logs/check.csv", "複数\r行")]
    public async Task CSVの特殊文字は一つのフィールドとして往復する(string path, string value)
    {
        await using var f = new RestEndpointTestFixture();
        var src = "/架空/" + value + ".ts";
        await f.Queue.Store.OnUIData(new UIData {
            LogData = new LogData { Items = new() { new() { SrcPath = src, Reason = value, OutPath = new() { value } } } },
            CheckLogData = new CheckLogData { Items = new() { new() { SrcPath = src, Reason = value } } },
        });
        var response = await f.Send("GET", path);
        Assert.Equal(200, response.Status);
        Assert.EndsWith("\r\n", response.Body);
        var rows = ParseCsv(response.Body);
        Assert.Equal(2, rows.Count);
        Assert.Equal(rows[0].Count, rows[1].Count);
        Assert.Equal(value, rows[1][path.Contains("encode") ? 1 : 5]);
        Assert.Equal(src, rows[1][2]);
        if (path.Contains("encode")) Assert.Equal(value, rows[1][3]);
    }

    [Fact]
    public async Task CSVの数値は小数点がカンマの文化でも不変表記を使う()
    {
        await using var f = new RestEndpointTestFixture();
        var previous = CultureInfo.CurrentCulture;
        try
        {
            CultureInfo.CurrentCulture = CultureInfo.GetCultureInfo("fr-FR");
            await f.Queue.Store.OnUIData(new UIData { LogData = new LogData { Items = new() { new() {
                SrcPath = "/架空/番組.ts", SrcFileSize = 1000, OutFileSize = 125,
                EncodeStartDate = new DateTime(2026, 10, 5), EncodeFinishDate = new DateTime(2026, 10, 5).AddSeconds(1.5),
                SrcVideoDuration = TimeSpan.FromSeconds(2.5), OutVideoDuration = TimeSpan.FromSeconds(3.5),
                AudioDiff = new AudioDiff { NotIncludedPer = 4.5, AvgDiff = 5.5, MaxDiff = 6.5, MaxDiffPos = 7.5 },
            } } } });
            var rows = ParseCsv((await f.Send("GET", "/api/logs/encode.csv")).Body);
            Assert.Equal(22, rows[1].Count);
            Assert.Equal(new[] { "1.5", "2.5", "3.5" }, rows[1].Skip(7).Take(3));
            Assert.Equal("12.50", rows[1][14]);
            Assert.Equal(new[] { "4.5", "5.5", "6.5", "7.5" }, rows[1].Skip(18));
        }
        finally { CultureInfo.CurrentCulture = previous; }
    }

    // 引用内の改行を含めてCSVを読み、出力の列数と元の値を独立に確認する。
    private static List<List<string>> ParseCsv(string csv)
    {
        var rows = new List<List<string>>();
        var row = new List<string>();
        var field = new StringBuilder();
        var quoted = false;
        for (var i = 0; i < csv.Length; i++)
        {
            var c = csv[i];
            if (c == '"')
            {
                if (quoted && i + 1 < csv.Length && csv[i + 1] == '"') { field.Append('"'); i++; }
                else quoted = !quoted;
            }
            else if (!quoted && (c == ',' || c == '\r' || c == '\n'))
            {
                row.Add(field.ToString()); field.Clear();
                if (c != ',')
                {
                    if (c == '\r' && i + 1 < csv.Length && csv[i + 1] == '\n') i++;
                    rows.Add(row); row = new();
                }
            }
            else field.Append(c);
        }
        Assert.False(quoted);
        if (field.Length > 0 || row.Count > 0) { row.Add(field.ToString()); rows.Add(row); }
        return rows;
    }
}
