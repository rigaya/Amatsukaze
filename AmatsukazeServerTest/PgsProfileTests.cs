using Amatsukaze.Server;
using System.Diagnostics;
using System.Runtime.Serialization;
using System.Text;
using System.Xml.Linq;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class PgsProfileTests
{
    [Fact]
    public void PGS項目のない旧プロファイルはPGS有効と空フォントで復元される()
    {
        var serializer = new DataContractSerializer(typeof(ProfileSetting));
        using var stream = new MemoryStream();
        serializer.WriteObject(stream, new ProfileSetting
        {
            DisablePgsSub = true,
            PgsFontFamily = "保存前のフォント",
        });
        stream.Position = 0;
        var document = XDocument.Load(stream);
        foreach (var name in new[] { "DisablePgsSub", "PgsFontFamily" })
        {
            var element = Assert.Single(document.Root!.Elements(), element => element.Name.LocalName == name);
            element.Remove();
        }
        using var reader = document.CreateReader();
        var restored = ServerSupport.NormalizeProfile(Assert.IsType<ProfileSetting>(serializer.ReadObject(reader)));

        Assert.False(restored.DisablePgsSub);
        Assert.Equal("", restored.PgsFontFamily);
        Assert.Equal("", restored.GetPgsSubtitleArguments());
    }

    [Fact]
    public void 新規プロファイルとnullフォントは既定フォントを使う()
    {
        var created = ServerSupport.NormalizeProfile(null);
        var restored = ServerSupport.NormalizeProfile(new ProfileSetting { PgsFontFamily = null! });

        Assert.False(created.DisablePgsSub);
        Assert.Equal("", created.PgsFontFamily);
        Assert.Equal("", restored.PgsFontFamily);
        Assert.Equal("", restored.GetPgsSubtitleArguments());
    }

    [Theory]
    [InlineData(false, "")]
    [InlineData(true, "Noto Sans CJK JP")]
    [InlineData(false, "日本語 \"書体\"\\")]
    public void PGS設定は保存復元とプロファイル複製で維持される(bool disabled, string font)
    {
        var source = new ProfileSetting { DisablePgsSub = disabled, PgsFontFamily = font };
        var serializer = new DataContractSerializer(typeof(ProfileSetting));
        using var stream = new MemoryStream();
        serializer.WriteObject(stream, source);
        stream.Position = 0;
        var restored = Assert.IsType<ProfileSetting>(serializer.ReadObject(stream));
        var copied = ServerSupport.DeepCopy(source);

        Assert.Equal(disabled, restored.DisablePgsSub);
        Assert.Equal(font, restored.PgsFontFamily);
        Assert.NotSame(source, copied);
        Assert.Equal(disabled, copied.DisablePgsSub);
        Assert.Equal(font, copied.PgsFontFamily);
        copied.PgsFontFamily = "変更後の書体";
        Assert.Equal(font, source.PgsFontFamily);
    }

    [Fact]
    public void CLI引数は既定とPGS無効とフォント指定を反映する()
    {
        Assert.Equal("", new ProfileSetting().GetPgsSubtitleArguments());
        Assert.Equal(" --no-pgs-sub", new ProfileSetting { DisablePgsSub = true }.GetPgsSubtitleArguments());
        Assert.Equal(" --pgs-font \"Noto Sans CJK JP\"",
            new ProfileSetting { PgsFontFamily = "Noto Sans CJK JP" }.GetPgsSubtitleArguments());
        Assert.Equal(" --no-pgs-sub --pgs-font \"指定書体\"",
            new ProfileSetting { DisablePgsSub = true, PgsFontFamily = "指定書体" }.GetPgsSubtitleArguments());
    }

    [Theory]
    [InlineData(false, "")]
    [InlineData(true, "Noto Sans CJK JP")]
    [InlineData(false, "日本語の書体")]
    public void 字幕全体を無効にした場合はPGS引数を追加しない(bool disabled, string font)
    {
        var profile = new ProfileSetting { DisableSubs = true, DisablePgsSub = disabled, PgsFontFamily = font };

        Assert.Equal("", profile.GetPgsSubtitleArguments());
    }

    [Theory]
    [InlineData(false, "Noto Sans CJK JP")]
    [InlineData(true, "日本語 空白 書体")]
    [InlineData(false, "書体 \"引用符\"")]
    [InlineData(false, "書体\\名前")]
    [InlineData(false, "末尾\\")]
    [InlineData(true, "末尾2\\\\")]
    [InlineData(false, "書体\\\"特殊\\\\\"\\")]
    [InlineData(false, " 日本語 空白 ")]
    [InlineData(true, "")]
    public async Task CLIの引用符とバックスラッシュと日本語は実際の子argvへ保持される(bool disabled, string font)
    {
        if (!OperatingSystem.IsLinux()) return;
        var executable = File.Exists("/usr/bin/printf") ? "/usr/bin/printf" : "/bin/printf";
        Assert.True(File.Exists(executable), "引数の疎通確認に使うprintfが見つからない");
        var profile = new ProfileSetting { DisablePgsSub = disabled, PgsFontFamily = font };
        using var process = new Process
        {
            StartInfo = new ProcessStartInfo
            {
                FileName = executable,
                // printfのNUL区切り出力で、シェルを通さず各argvの内容を区別する。
                Arguments = "\"%s\\0\"" + profile.GetPgsSubtitleArguments(),
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8,
                StandardErrorEncoding = Encoding.UTF8,
            },
        };
        Assert.True(process.Start());
        var output = process.StandardOutput.ReadToEndAsync();
        var error = process.StandardError.ReadToEndAsync();
        try
        {
            await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(10));
        }
        catch (TimeoutException)
        {
            process.Kill(entireProcessTree: true);
            throw;
        }
        Assert.Equal(0, process.ExitCode);
        Assert.Equal("", await error);
        var expected = new List<string>();
        if (disabled) expected.Add("--no-pgs-sub");
        if (font.Length > 0)
        {
            expected.Add("--pgs-font");
            expected.Add(font);
        }
        var received = (await output).Split('\0');
        Assert.Equal("", received[^1]);
        Assert.Equal(expected, received[..^1]);
    }
}
