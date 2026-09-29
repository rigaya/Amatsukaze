using Amatsukaze.Server;
using System.Runtime.Serialization;
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
    public void 音声形式設定のない旧プロファイルは統合になる()
    {
        var serializer = new DataContractSerializer(typeof(ProfileSetting));
        using var stream = new MemoryStream();
        serializer.WriteObject(stream, new ProfileSetting { AudioFormatChangeMode = 1 });
        stream.Position = 0;
        var document = System.Xml.Linq.XDocument.Load(stream);
        document.Root!.Elements().Single(element => element.Name.LocalName == "AudioFormatChangeMode").Remove();
        using var reader = document.CreateReader();
        var restored = Assert.IsType<ProfileSetting>(serializer.ReadObject(reader));

        Assert.Equal(0, restored.AudioFormatChangeMode);
        Assert.Equal("merge", restored.GetAudioFormatChangeModeArgument());
    }

    [Fact]
    public void 新規プロファイルの音声形式は統合になる()
    {
        var profile = ServerSupport.NormalizeProfile(null);

        Assert.Equal(0, profile.AudioFormatChangeMode);
        Assert.Equal("merge", profile.GetAudioFormatChangeModeArgument());
    }

    [Theory]
    [InlineData(0, "merge")]
    [InlineData(1, "split")]
    [InlineData(2, "separate")]
    public void 音声形式設定は保存と読み込みで維持される(int mode, string argument)
    {
        var serializer = new DataContractSerializer(typeof(ProfileSetting));
        using var stream = new MemoryStream();
        serializer.WriteObject(stream, new ProfileSetting { AudioFormatChangeMode = mode });
        stream.Position = 0;
        var restored = Assert.IsType<ProfileSetting>(serializer.ReadObject(stream));

        Assert.Equal(mode, restored.AudioFormatChangeMode);
        Assert.Equal(argument, restored.GetAudioFormatChangeModeArgument());
    }

    [Theory]
    [InlineData(-1)]
    [InlineData(3)]
    [InlineData(int.MaxValue)]
    public void 範囲外の音声形式は統合として扱う(int mode)
    {
        var profile = new ProfileSetting { AudioFormatChangeMode = mode };

        Assert.Equal(0, ProfileSettingExtensions.NormalizeAudioFormatChangeMode(mode));
        Assert.Equal("merge", profile.GetAudioFormatChangeModeArgument());
        Assert.Equal("トラック統合", profile.GetAudioFormatChangeModeDisplayName());
    }

    [Fact]
    public void 新規プロファイルの最短出力時間は5秒になる()
    {
        var profile = ServerSupport.NormalizeProfile(null);

        Assert.Equal(5, profile.MinOutputDuration);
    }

    [Fact]
    public void 新規プロファイルではCM解析ログを出力しない()
    {
        var profile = ServerSupport.NormalizeProfile(null);

        Assert.False(profile.EnableCMLogFile);
    }

    [Fact]
    public void CM解析ログ設定は保存と読み込みで維持される()
    {
        var serializer = new DataContractSerializer(typeof(ProfileSetting));
        var source = new ProfileSetting { EnableCMLogFile = true };
        using var stream = new MemoryStream();

        serializer.WriteObject(stream, source);
        stream.Position = 0;
        var restored = Assert.IsType<ProfileSetting>(serializer.ReadObject(stream));

        Assert.True(restored.EnableCMLogFile);
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
