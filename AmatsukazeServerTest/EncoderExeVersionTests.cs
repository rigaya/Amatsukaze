using Amatsukaze.Server;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class EncoderExeVersionTests
{
    [Theory]
    [InlineData(EncoderType.x264, "0.164.3190M 7ed753b", "3190.0.0.0")]
    [InlineData(EncoderType.x264, "0.165.3223M 0480cb0", "3223.0.0.0")]
    [InlineData(EncoderType.x262, "0.165.3349 72a1323", "3349.0.0.0")]
    [InlineData(EncoderType.x265, "4.1+131-1b9f056f2", "4.1.0.131")]
    [InlineData(EncoderType.x265, "4.3+22-98eec4057", "4.3.0.22")]
    public void 実在するFileVersionを4要素へ正規化する(
        EncoderType type, string value, string expected)
    {
        var version = EncoderExeVersion.ParseFileVersion(value, type);

        Assert.NotNull(version);
        Assert.Equal(expected, version.ToString(4));
    }

    [Theory]
    [InlineData("1", "1.0.0.0")]
    [InlineData("1.2", "1.2.0.0")]
    [InlineData("1.2.3", "1.2.3.0")]
    [InlineData("1.2.3.4", "1.2.3.4")]
    public void 数値版数の不足要素をゼロで補う(string value, string expected)
    {
        var version = EncoderExeVersion.ParseFileVersion(value, EncoderType.SVTAV1);

        Assert.NotNull(version);
        Assert.Equal(expected, version.ToString(4));
    }

    [Theory]
    [InlineData(EncoderType.x264, "x264_3223_x64.exe", "3223.0.0.0")]
    [InlineData(EncoderType.x264, "x264_rev3222_x64.exe", "3222.0.0.0")]
    [InlineData(EncoderType.x262, "x262_20260901_x64.exe", "20260901.0.0.0")]
    [InlineData(EncoderType.x265, "x265_4.2_x64.exe", "4.2.0.0")]
    [InlineData(EncoderType.x265, "x265_4.3+22_x64.exe", "4.3.0.22")]
    [InlineData(EncoderType.SVTAV1, "SvtAv1EncApp_1.7.0_x64.exe", "1.7.0.0")]
    [InlineData(EncoderType.SVTAV1, "SvtAv1EncApp_v1.7.0_x64.exe", "1.7.0.0")]
    [InlineData(EncoderType.SVTAV1, "SvtAv1EncApp_4.2.0-107_x64_clang.exe", "4.2.0.107")]
    public void ファイル名を4要素へ正規化する(
        EncoderType type, string filename, string expected)
    {
        var version = EncoderExeVersion.ParseFilename(filename, type);

        Assert.NotNull(version);
        Assert.Equal(expected, version.ToString(4));
    }

    [Theory]
    [InlineData(EncoderType.x264, "unknown")]
    [InlineData(EncoderType.x265, "4.3+invalid")]
    [InlineData(EncoderType.SVTAV1, "")]
    public void 不明なFileVersionは解析しない(EncoderType type, string value)
    {
        Assert.Null(EncoderExeVersion.ParseFileVersion(value, type));
    }
}
