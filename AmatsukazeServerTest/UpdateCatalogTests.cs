using Amatsukaze.Server.Update;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class UpdateCatalogTests
{
    [Theory]
    [InlineData("x264", "x264_3223_amd64_linux.tar.xz", "3223")]
    [InlineData("x265", "x265_4.3+13_amd64_linux.tar.xz", "4.3+13")]
    [InlineData("SVT-AV1", "SvtAv1EncApp_4.2.0-76_amd64_linux_clang.tar.xz", "4.2.0-76")]
    [InlineData("QSVEnc", "qsvencc_8.26_amd64.deb", "8.26")]
    [InlineData("NVEnc", "nvencc_9.31_amd64.deb", "9.31")]
    [InlineData("VCEEnc", "vceencc_9.12_amd64.deb", "9.12")]
    [InlineData("tsreplace", "tsreplace_0.19_amd64.deb", "0.19")]
    public void LinuxX64の実アセット名からバージョンを取得する(
        string targetId, string assetName, string expectedVersion)
    {
        Assert.True(UpdateCatalog.TryInitialize(out var error), error);
        var target = Assert.Single(UpdateCatalog.Targets, item => item.Id == targetId);
        var environment = new UpdateRuntimeEnvironment
        {
            OS = UpdateOSKind.Linux,
            Architecture = UpdateArchitecture.X64,
        };
        var rule = Assert.Single(target.AssetRules, item => item.AppliesTo(environment));

        var match = rule.Match(assetName);

        Assert.True(match.Success);
        Assert.Equal(expectedVersion, match.Groups["ver"].Value);
    }

    [Theory]
    [InlineData("x264", "x264")]
    [InlineData("x265", "x265")]
    [InlineData("SVT-AV1", "SvtAv1EncApp")]
    public void Linuxの実行ファイル名がPayloadに一意に一致する(
        string targetId, string executableName)
    {
        Assert.True(UpdateCatalog.TryInitialize(out var error), error);
        var target = Assert.Single(UpdateCatalog.Targets, item => item.Id == targetId);

        Assert.Single(target.Payload, entry => entry.IsMatch(executableName));
    }

    [Theory]
    [InlineData("tsreplace", "tsreplace_0.19_arm64.deb")]
    [InlineData("QSVEnc", "QSVEncC_8.26_x64.7z")]
    [InlineData("NVEnc", "nvencc_9.31_arm64.deb")]
    [InlineData("VCEEnc", "vceencc_9.12_amd64.zip")]
    public void LinuxX64の規則は別環境のアセット名に一致しない(
        string targetId, string assetName)
    {
        Assert.True(UpdateCatalog.TryInitialize(out var error), error);
        var target = Assert.Single(UpdateCatalog.Targets, item => item.Id == targetId);
        var environment = new UpdateRuntimeEnvironment
        {
            OS = UpdateOSKind.Linux,
            Architecture = UpdateArchitecture.X64,
        };
        var rule = Assert.Single(target.AssetRules, item => item.AppliesTo(environment));

        Assert.False(rule.Match(assetName).Success);
    }

    [Theory]
    [InlineData("QSVEnc", "qsvencc")]
    [InlineData("NVEnc", "nvencc")]
    [InlineData("VCEEnc", "vceencc")]
    [InlineData("tsreplace", "tsreplace")]
    public void LinuxPayloadは裸名とDeb内パスの両方に一致する(
        string targetId, string executableName)
    {
        Assert.True(UpdateCatalog.TryInitialize(out var error), error);
        var target = Assert.Single(UpdateCatalog.Targets, item => item.Id == targetId);
        var payload = Assert.Single(target.Payload, entry => entry.IsMatch(executableName));

        Assert.True(payload.IsMatch(executableName));
        Assert.True(payload.IsMatch("usr/bin/" + executableName));
        Assert.False(payload.IsMatch(executableName + ".exe"));
    }

    [Theory]
    [InlineData("x264", "x264_3223_x64.zip", "3223")]
    [InlineData("x265", "x265_4.3+13_x64.zip", "4.3+13")]
    [InlineData("SVT-AV1", "SvtAv1EncApp_4.2.0-76_x64_clang.zip", "4.2.0-76")]
    [InlineData("QSVEnc", "QSVEncC_8.26_x64.7z", "8.26")]
    [InlineData("NVEnc", "NVEncC_9.31_x64.7z", "9.31")]
    [InlineData("VCEEnc", "VCEEncC_9.12_x64.7z", "9.12")]
    [InlineData("tsreplace", "tsreplace_0.19_x64.7z", "0.19")]
    public void WindowsX64の実アセット名からバージョンを取得する(
        string targetId, string assetName, string expectedVersion)
    {
        Assert.True(UpdateCatalog.TryInitialize(out var error), error);
        var target = Assert.Single(UpdateCatalog.Targets, item => item.Id == targetId);
        var environment = new UpdateRuntimeEnvironment
        {
            OS = UpdateOSKind.Windows,
            Architecture = UpdateArchitecture.X64,
        };
        var rule = Assert.Single(target.AssetRules, item => item.AppliesTo(environment));

        var match = rule.Match(assetName);

        Assert.True(match.Success);
        Assert.Equal(expectedVersion, match.Groups["ver"].Value);
    }

    [Theory]
    [InlineData("x264", "x264_3223_x64.exe", "x264")]
    [InlineData("x265", "x265_4.3+13_x64.exe", "x265")]
    [InlineData("SVT-AV1", "SvtAv1EncApp_4.2.0-76_x64_clang.exe", "SvtAv1EncApp")]
    [InlineData("QSVEnc", "QSVEncC64.exe", "qsvencc")]
    [InlineData("NVEnc", "NVEncC64.exe", "nvencc")]
    [InlineData("VCEEnc", "VCEEncC64.exe", "vceencc")]
    [InlineData("tsreplace", "tsreplace.exe", "tsreplace")]
    public void WindowsPayloadは実行ファイルだけに一致しLinux名と混同しない(
        string targetId, string executableName, string linuxName)
    {
        Assert.True(UpdateCatalog.TryInitialize(out var error), error);
        var target = Assert.Single(UpdateCatalog.Targets, item => item.Id == targetId);
        var payload = Assert.Single(target.Payload, entry => entry.IsMatch(executableName));

        Assert.True(payload.IsMatch(executableName));
        Assert.False(payload.IsMatch(linuxName));
        Assert.False(payload.IsMatch("usr/bin/" + linuxName));
    }
}
