using Amatsukaze.Server;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class ProfileSettingTests
{
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
