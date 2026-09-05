using System;
using Amatsukaze.Server;
using Amatsukaze.Server.Update;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class SvtAv1VersionTests
{
    [Theory]
    [InlineData("SvtAv1EncApp_1.4.0-15_x64.exe", "1.4.0.15")]
    [InlineData("SvtAv1EncApp_1.4.1-103_x64.exe", "1.4.1.103")]
    [InlineData("SvtAv1EncApp_2.2.1-41_x64.exe", "2.2.1.41")]
    [InlineData("SvtAv1EncApp_3.0.1-20_x64.exe", "3.0.1.20")]
    [InlineData("SvtAv1EncApp_3.0.2-59_x64.exe", "3.0.2.59")]
    [InlineData("SvtAv1EncApp_3.1.0-113_x64.exe", "3.1.0.113")]
    [InlineData("SvtAv1EncApp_3.1.0-157_x64_clang.exe", "3.1.0.157")]
    [InlineData("SvtAv1EncApp_3.1.0-291_x64_clang.exe", "3.1.0.291")]
    [InlineData("SvtAv1EncApp_4.1.0-160_x64_clang.exe", "4.1.0.160")]
    [InlineData("SvtAv1EncApp_4.2.0-84_x64_clang.exe", "4.2.0.84")]
    [InlineData("SvtAv1EncApp_v1.7.0_x64.exe", "1.7.0.0")]
    [InlineData("SvtAv1EncApp_3.0.1-20-g7bc9_x64.exe", "3.0.1.20")]
    [InlineData("SvtAv1EncApp_3.1.0-122.exe", "3.1.0.122")]
    [InlineData("SvtAv1EncApp_3.1.0-126-O2.exe", "3.1.0.126")]
    [InlineData("SvtAv1EncApp_3.1.0-69.exe", "3.1.0.69")]
    [InlineData("SvtAv1EncApp_4.0.0_x64_clang.exe", "4.0.0.0")]
    [InlineData("SvtAv1EncApp_4.0.1_x64_clang.exe", "4.0.1.0")]
    public void 実在形式のファイル名からバージョンを取得する(string filename, string expected)
    {
        Assert.True(SvtAv1Version.TryParseFilename(filename, out var version));
        Assert.Equal(Version.Parse(expected), version);
    }

    [Theory]
    [InlineData("SVT-AV1 v1.5.0-55-gc83e0e3d (release)", "1.5.0.55")]
    [InlineData("SVT-AV1 v3.0.1-20-g7bc96cce-dirty (release)", "3.0.1.20")]
    [InlineData("SVT-AV1 v4.0.0 (release)", "4.0.0.0")]
    [InlineData("補助出力\nSVT-AV1 v4.2.0-98-g069628233 (release)\n", "4.2.0.98")]
    public void Version出力からバージョンを取得する(string output, string expected)
    {
        Assert.True(SvtAv1Version.TryParseVersionOutput(output, out var version));
        Assert.Equal(Version.Parse(expected), version);
    }

    [Fact]
    public void ファイル名を解析できる場合は実行結果を要求しない()
    {
        var called = false;

        var version = SvtAv1Version.GetVersion(
            "SvtAv1EncApp_4.2.0-84_x64_clang.exe",
            _ =>
            {
                called = true;
                return "SVT-AV1 v9.9.9-9 (release)";
            });

        Assert.False(called);
        Assert.Equal(Version.Parse("4.2.0.84"), version);
    }

    [Fact]
    public void ファイル名を解析できない場合だけVersion出力を使用する()
    {
        var called = false;

        var version = SvtAv1Version.GetVersion(
            "SvtAv1EncApp_custom.exe",
            _ =>
            {
                called = true;
                return "SVT-AV1 v4.2.0-98-g069628233 (release)";
            });

        Assert.True(called);
        Assert.Equal(Version.Parse("4.2.0.98"), version);
    }

    [Theory]
    [InlineData("SVT-AV1 v4.0.0 (release)", "4.0.0")]
    [InlineData("SVT-AV1 v4.2.0-98-g069628233 (release)", "4.2.0-98")]
    public void 自動更新側も同じVersion出力形式を解析する(string output, string expected)
    {
        Assert.True(UpdateCatalog.TryInitialize(out var error), error);
        var target = Assert.Single(UpdateCatalog.Targets, item => item.Id == "SVT-AV1");

        var match = target.VersionRegex!.Match(output);

        Assert.True(match.Success);
        Assert.Equal(expected, match.Groups["ver"].Value);
    }
}
