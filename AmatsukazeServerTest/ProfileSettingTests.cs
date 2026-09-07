using Amatsukaze.Server;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class ProfileSettingTests
{
    [Fact]
    public void Tsreplaceはすべての出力選択に対応する()
    {
        Assert.Equal(new[] { 1, 2, 4, 6, 8 }, ProfileSettingExtensions.TsreplaceOutputMasks);
    }

    [Fact]
    public void カット境界再エンコードは通常以外の出力選択に対応する()
    {
        Assert.Equal(new[] { 2, 4, 6, 8 }, ProfileSettingExtensions.Mpeg2PartialOutputMasks);
    }

    [Fact]
    public void 新規プロファイルの最短出力時間は5秒になる()
    {
        var profile = ServerSupport.NormalizeProfile(null);

        Assert.Equal(5, profile.MinOutputDuration);
    }

    [Fact]
    public void 旧プロファイルの最短出力時間は0のまま維持される()
    {
        var profile = ServerSupport.NormalizeProfile(new ProfileSetting());

        Assert.Equal(0, profile.MinOutputDuration);
    }

    [Fact]
    public void 明示した最短出力時間は維持される()
    {
        var profile = ServerSupport.NormalizeProfile(new ProfileSetting
        {
            MinOutputDuration = 12
        });

        Assert.Equal(12, profile.MinOutputDuration);
    }

    [Fact]
    public void 負の最短出力時間は0に補正される()
    {
        var profile = ServerSupport.NormalizeProfile(new ProfileSetting
        {
            MinOutputDuration = -1
        });

        Assert.Equal(0, profile.MinOutputDuration);
    }
}
