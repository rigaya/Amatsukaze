using Amatsukaze.Server.Update;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class UpdateCatalogTests
{
    [Theory]
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
        var payload = Assert.Single(target.Payload);

        Assert.True(payload.IsMatch(executableName));
        Assert.True(payload.IsMatch("usr/bin/" + executableName));
        Assert.False(payload.IsMatch(executableName + ".exe"));
    }
}
